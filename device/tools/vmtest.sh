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
#   ./tools/vmtest.sh --control    install libASPL's OWN example instead of ours
#   ./tools/vmtest.sh --verify     also push audio through it and check the crosspoint
#   ./tools/vmtest.sh --keep       leave the VM running afterwards to poke at it
#   ./tools/vmtest.sh --shell      just open a shell in a fresh VM
#
# RUN --control FIRST, on a new machine or after any change to the harness.
#
# It installs libASPL's unmodified SinewaveDevice example, which is known to work. If that
# appears, the harness, the VM, the signing and the install path are all proven, and any
# later failure belongs to our driver. If it does NOT appear, nothing this script says
# about our driver means anything.
#
# This is the step that was skipped on the host, and skipping it is what turned a bad
# driver into a day of chasing ghosts: with no control, every failure looked like it could
# be the bundle, the plist, the signature, the UUID, the path or the machine, and each
# theory could be "confirmed" by a test that was itself unsound.
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
CONTROL=0
VERIFY=0
for arg in "$@"; do
  case "$arg" in
    --keep)    KEEP=1 ;;
    --shell)   SHELL_ONLY=1; KEEP=1 ;;
    --control) CONTROL=1 ;;
    --verify)  VERIFY=1 ;;
  esac
done

note() { print -P "%F{cyan}==>%f $*"; }
fail() { print -P "%F{red}ERROR:%f $*" >&2; exit 1; }

command -v tart >/dev/null || fail "tart not installed (brew install cirruslabs/cli/tart)"
# Hard fail, not a flag nobody reads: without sshpass every ssh_vm below hangs on a password
# prompt with no tty, and the run dies minutes later at "ssh never came up" — which reads as
# a broken VM rather than a missing tool.
command -v sshpass >/dev/null || fail "sshpass not installed (brew install esolitos/ipa/sshpass)"

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
if [[ $CONTROL -eq 1 ]]; then
  # libASPL's own example, built and signed exactly like ours so the only difference is
  # whose code is inside.
  SRC="$ROOT/build/_deps/libaspl-src"
  [[ -d "$SRC" ]] || fail "libASPL sources not fetched yet - run a normal build first"

  # The identity the driver build actually resolved, not a guess. Signing the control ad-hoc
  # while the driver carries a Developer ID would leave two variables changing between
  # control and subject, which defeats the entire point of having a control: a passing
  # control would say nothing about the path the real bundle takes.
  CACHE="$ROOT/build/CMakeCache.txt"
  [[ -f "$CACHE" ]] || fail "no $CACHE - run a normal build first"
  SIGN_ID=$(sed -n 's/^MR_SIGN_IDENTITY:STRING=//p' "$CACHE")
  [[ -n "$SIGN_ID" ]] || SIGN_ID="-"
  note "control will be signed with the driver's identity: $SIGN_ID"

  # arm64 only, and it cannot be otherwise: the example builds libASPL through an
  # ExternalProject that does not inherit CMAKE_OSX_ARCHITECTURES, so a universal control
  # links its own module against an arm64-only static lib and fails with "symbol(s) not
  # found for architecture x86_64". Tried and confirmed, so do not spend an afternoon on it.
  #
  # That leaves architecture as the ONE variable still differing between control and subject
  # — the driver is universal. It is called out here rather than left silent because §2 of
  # AGENTS.md rests on the control differing in exactly one thing. If the control passes and
  # the driver fails, an arm64-only driver is therefore a legitimate next test, not a guess:
  #   cmake -B build-arm64 -S . -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build-arm64
  note "control is arm64-only; the driver bundle is universal — the one remaining difference"
  note "building libASPL's SinewaveDevice example as a control"
  cmake -S "$SRC/examples/SinewaveDevice" -B /tmp/mr-control-build \
        -DCMAKE_OSX_ARCHITECTURES="arm64" >/dev/null || fail "control configure failed"
  cmake --build /tmp/mr-control-build >/dev/null || fail "control build failed"
  BUNDLE=$(find /tmp/mr-control-build -maxdepth 2 -name "*.driver" | head -1)
  [[ -n "$BUNDLE" ]] || fail "control build produced no .driver"
  codesign --force --sign "$SIGN_ID" --timestamp "$BUNDLE" || fail "control signing failed"
  codesign --verify --strict "$BUNDLE" || fail "control signature does not verify"
  EXPECT="Sinewave"
elif [[ $SHELL_ONLY -eq 0 ]]; then
  note "building on the host"
  cmake --build "$ROOT/build" >/dev/null || fail "build failed"
  [[ -d "$BUNDLE" ]] || fail "no bundle at $BUNDLE"
  codesign --verify --strict "$BUNDLE" || fail "bundle signature does not verify"
  EXPECT="MixerReturn"
fi

# ---------------------------------------------------------------------------------------
# A fresh VM per run.
# ---------------------------------------------------------------------------------------
# Stop before delete. `tart delete` refuses a RUNNING VM, so a leftover from an interrupted
# run holds the name and the clone below fails — and it fails as "control did not appear" or
# some other downstream nonsense rather than as "the name was taken". Cost an A/B run.
if tart list 2>/dev/null | grep -q " $VM "; then
  note "removing previous $VM"
  tart stop "$VM" 2>/dev/null || true
  sleep 2
  tart delete "$VM" 2>/dev/null || fail "could not delete leftover $VM — 'tart stop $VM' by hand"
fi

note "cloning $BASE -> $VM"
tart clone "$BASE" "$VM"

note "starting VM (headless)"
tart run --no-graphics "$VM" >/tmp/mr-vm-run.log 2>&1 &
sleep 5

# --resolver=arp, NOT the default resolver, and this is load-bearing. tart's default reads
# /var/db/dhcpd_leases keyed by MAC, and every clone of the base image carries the SAME MAC —
# so it hands back a *previous* VM's lease instantly, for a VM that is still booting or is
# not reachable at all. The run then spends its whole ssh budget on a dead address and
# reports "ssh never came up", which reads as a slow VM rather than a wrong address. The arp
# resolver answers only when the host can actually see the guest. See AGENTS.md §6a.
note "waiting for the VM to get an address (arp resolver)"
IP=""
for i in {1..90}; do
  IP=$(tart ip "$VM" --resolver=arp 2>/dev/null || true)
  [[ -n "$IP" ]] && break
  sleep 2
done
if [[ -z "$IP" ]]; then
  STALE=$(tart ip "$VM" 2>/dev/null || true)
  note "for comparison, the default (lease) resolver says: '${STALE:-nothing}'"
  note "if that returned an address while arp did not, the host cannot route to the VM —"
  note "check 'route -n get \$STALE' against 'netstat -rn -f inet | grep 192.168.64'."
  fail "VM never became reachable — see AGENTS.md §6a and /tmp/mr-vm-run.log"
fi
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
scp_vm "$BUNDLE" "$VMUSER@$IP:/tmp/$(basename $BUNDLE)"

note "installing and restarting coreaudiod"
ssh_vm "echo '$VMPASS' | sudo -S cp -R /tmp/$(basename $BUNDLE) /Library/Audio/Plug-Ins/HAL/ 2>/dev/null && echo '$VMPASS' | sudo -S killall coreaudiod 2>/dev/null; sleep 6"

note "what CoreAudio sees"
ssh_vm "system_profiler SPAudioDataType 2>/dev/null | grep -E '^ {8}[A-Za-z].*:\$' || true"

print ""
if ssh_vm "system_profiler SPAudioDataType 2>/dev/null | grep -qi '$EXPECT'"; then
  print -P "%F{green}PASS%f  $EXPECT is present in the VM's device list"

  # -------------------------------------------------------------------------------------
  # --verify: the device APPEARING says only that coreaudiod accepted the bundle. This
  # pushes real audio through it.
  #
  # mrio is built on the HOST and copied in as a binary. The base image has no toolchain,
  # and installing one into every throwaway VM would cost minutes per run — but note mrio
  # must then be built for the VM's architecture (arm64), NOT as a universal or x86_64
  # binary, or it dies in the guest with a confusing exec format error.
  #
  # Deliberately not run against the host's own MixerReturn device, however tempting: an
  # IO-path fault in an AudioServerPlugIn takes coreaudiod down for every application on
  # the machine, which is the whole reason §2 exists. The VM is where audio gets pushed.
  # -------------------------------------------------------------------------------------
  if [[ $VERIFY -eq 1 && $CONTROL -eq 0 ]]; then
    print ""
    note "building mrio on the host (arm64, to match the guest)"
    MRIO=/tmp/mr-mrio-arm64
    clang++ -std=c++20 -O2 -arch arm64 -o "$MRIO" "$ROOT/../tools/mrio.cpp" \
      -framework CoreAudio -framework AudioToolbox -framework CoreFoundation \
      -framework Accelerate || fail "mrio build failed"

    note "copying mrio in"
    scp_vm "$MRIO" "$VMUSER@$IP:/tmp/mrio"
    ssh_vm "chmod +x /tmp/mrio"

    note "devices mrio sees in the guest"
    ssh_vm "/tmp/mrio list 2>&1" || true

    print ""
    note "crosspoint: a distinct tone on every Sum port, measured on the bus returns"
    note "expected with the shipped defaults — every Sum port lands on bus 1 (in 1+2),"
    note "and buses 2..4 (in 3..8) stay silent"
    ssh_vm "/tmp/mrio probe MixerReturn 2>&1" || true
  fi
else
  print -P "%F{red}FAIL%f  $EXPECT did not appear"
  # TWO bugs lived in these three lines and between them they made every failure unreadable,
  # which is most of why the first attempt went in circles. Both found 2026-08-05:
  #
  #   1. `log show` never ran. The guest shell is zsh, and **zsh has a `log` builtin** that
  #      shadows /usr/bin/log — the command died with "zsh:log:1: too many arguments" on
  #      stderr, which `2>/dev/null` then swallowed. Always spell it /usr/bin/log.
  #
  #   2. The predicates pointed at the wrong process. On macOS 26 a HAL plugin is NOT hosted
  #      in coreaudiod — it runs in `com.apple.audio.Core-Audio-Driver-Service.helper`, and
  #      libASPL's Syslog tracer emits through syslog(3), so it lands under that process with
  #      the bundle name as the log *category*, not the sender image. Grepping the whole log
  #      for the driver name is cruder and actually works.
  note "driver's own trace (libASPL syslog tracer; see the comment above about macOS 26)"
  ssh_vm "/usr/bin/log show --last 2m 2>/dev/null | grep -i mixerreturn | tail -40 || true"
  note "and what the HAL said while trying to load it"
  ssh_vm "/usr/bin/log show --last 2m --predicate 'process == \"coreaudiod\"' 2>/dev/null | grep -iE 'plug-?in|driver|MixerReturn|error' | tail -25 || true"
fi
