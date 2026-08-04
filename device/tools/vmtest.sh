#!/bin/zsh
#
# vmtest.sh — install the driver into a throwaway macOS VM and report what CoreAudio makes
# of it. Nothing here touches the host.
#
# A bad AudioServerPlugIn can take down audio for every application on the machine, and the
# only way to find out whether one is bad is to install it into /Library and restart
# coreaudiod. Doing that on a working rig cost this project a reboot and a lot of goodwill;
# doing it in a VM costs `tart delete`.
#
#   ./tools/vmtest.sh              build, install into a fresh VM, report
#   ./tools/vmtest.sh --keep       leave the VM running afterwards to poke at it
#   ./tools/vmtest.sh --shell      just open a shell in a fresh VM
#
# The VM is cloned from a base image every run, so each test starts from a known-good
# system. That matters more than it sounds: a driver that half-registers can leave CoreAudio
# in a state where the *next* test lies to you, which is exactly how the host investigation
# went in circles.

set -euo pipefail

BASE="${MR_VM_BASE:-ghcr.io/cirruslabs/macos-tahoe-base:latest}"
VM="${MR_VM_NAME:-mixerreturn-test}"
VMUSER="${MR_VM_USER:-admin}"
VMPASS="${MR_VM_PASS:-admin}"

HERE="${0:a:h}"
ROOT="${HERE:h}"
BUNDLE="$ROOT/build/MixerReturn.driver"

KEEP=0
SHELL_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --keep)  KEEP=1 ;;
    --shell) SHELL_ONLY=1; KEEP=1 ;;
  esac
done

note() { print -P "%F{cyan}==>%f $*"; }
fail() { print -P "%F{red}ERROR:%f $*" >&2; exit 1; }

command -v tart >/dev/null || fail "tart not installed (brew install cirruslabs/cli/tart)"
command -v sshpass >/dev/null || SSHPASS_MISSING=1

cleanup() {
  if [[ $KEEP -eq 0 ]]; then
    note "stopping and deleting $VM"
    tart stop "$VM" 2>/dev/null || true
    tart delete "$VM" 2>/dev/null || true
  else
    note "leaving $VM running — 'tart stop $VM && tart delete $VM' when done"
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------------------
# Build on the host. The VM is for running, not compiling: a clean base image has no
# toolchain, and installing one into every throwaway VM would make each run take minutes
# instead of seconds.
# ---------------------------------------------------------------------------------------
if [[ $SHELL_ONLY -eq 0 ]]; then
  note "building on the host"
  cmake --build "$ROOT/build" >/dev/null || fail "build failed"
  [[ -d "$BUNDLE" ]] || fail "no bundle at $BUNDLE"
  codesign --verify --strict "$BUNDLE" || fail "bundle signature does not verify"
fi

# ---------------------------------------------------------------------------------------
# A fresh VM per run.
# ---------------------------------------------------------------------------------------
tart list 2>/dev/null | grep -q " $VM " && { note "removing previous $VM"; tart delete "$VM" || true; }

note "cloning $BASE -> $VM"
tart clone "$BASE" "$VM"

note "starting VM (headless)"
tart run --no-graphics "$VM" >/tmp/mr-vm-run.log 2>&1 &
sleep 5

note "waiting for the VM to get an address"
IP=""
for i in {1..60}; do
  IP=$(tart ip "$VM" 2>/dev/null || true)
  [[ -n "$IP" ]] && break
  sleep 2
done
[[ -n "$IP" ]] || fail "VM never got an IP — see /tmp/mr-vm-run.log"
note "VM at $IP"

SSHOPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
         -o ConnectTimeout=5)
ssh_vm() { sshpass -p "$VMPASS" ssh "${SSHOPTS[@]}" "$VMUSER@$IP" "$@"; }
scp_vm() { sshpass -p "$VMPASS" scp -r "${SSHOPTS[@]}" "$@"; }

note "waiting for ssh"
for i in {1..60}; do
  ssh_vm true 2>/dev/null && break
  sleep 2
done
ssh_vm true 2>/dev/null || fail "ssh never came up"

if [[ $SHELL_ONLY -eq 1 ]]; then
  note "opening a shell — VM will be left running"
  sshpass -p "$VMPASS" ssh "${SSHOPTS[@]}" "$VMUSER@$IP"
  exit 0
fi

# ---------------------------------------------------------------------------------------
# Install and interrogate. `sudo -S` because the base images use a password-auth admin.
# ---------------------------------------------------------------------------------------
note "copying the driver in"
scp_vm "$BUNDLE" "$VMUSER@$IP:/tmp/MixerReturn.driver"

note "installing and restarting coreaudiod"
ssh_vm "echo '$VMPASS' | sudo -S cp -R /tmp/MixerReturn.driver /Library/Audio/Plug-Ins/HAL/ 2>/dev/null && echo '$VMPASS' | sudo -S killall coreaudiod 2>/dev/null; sleep 6"

note "what CoreAudio sees"
ssh_vm "system_profiler SPAudioDataType 2>/dev/null | grep -E '^ {8}[A-Za-z].*:\$' || true"

print ""
if ssh_vm "system_profiler SPAudioDataType 2>/dev/null | grep -qi mixerreturn"; then
  print -P "%F{green}PASS%f  MixerReturn is present in the VM's device list"
else
  print -P "%F{red}FAIL%f  MixerReturn did not appear"
  note "driver's own trace (this is why the driver enables the libASPL syslog tracer)"
  ssh_vm "log show --last 2m --predicate 'senderImagePath CONTAINS \"MixerReturn\" OR eventMessage CONTAINS \"MixerReturn\"' 2>/dev/null | tail -40 || true"
  note "and anything coreaudiod said about loading plugins"
  ssh_vm "log show --last 2m --predicate 'process == \"coreaudiod\"' 2>/dev/null | grep -iE 'plug-?in|driver|MixerReturn|error' | tail -25 || true"
fi
