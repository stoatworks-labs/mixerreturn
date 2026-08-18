# AGENTS.md — picking up the Phase 2 device cold

Orientation for whoever continues this, human or model. The repo root's `AGENTS.md` covers
the **plugin** (Phase 1, shipped at v0.3.0). This file covers the **device** (Phase 2, not
working yet) and, more importantly, what has already been tried so it is not tried again.

Read `README.md` in this directory first for the architecture. This file is about state,
method and traps.

---

## 1. Status, stated plainly

**The driver loads. `MixerReturn` appears in the device list.** Reached 2026-08-05, in a
clean VM, behind a passing control — the milestone §5 called "the whole milestone".

Scope that claim carefully. coreaudiod accepts the bundle, publishes the device, and drives
it: audio written to the Sum ports reaches the driver and **is summed correctly** (§4a). What
does *not* work is the return leg — the HAL never asks the driver to fill the input buffers,
so the buses come back as digital silence. The helper daemon does not exist, and nothing has
been tested in a real host application.

Evidence — every run on a freshly cloned VM, all on 2026-08-05:

| run | build | result |
|---|---|---|
| control | libASPL `SinewaveDevice`, Developer ID signed | PASS — `Sinewave Device (libASPL)` |
| 1 | driver, `ChannelCount = 8` | PASS — `MixerReturn` |
| 2 | driver, `ChannelCount = 8` | PASS — `MixerReturn` |
| 3 | driver, `ChannelCount` back at default 2 | PASS — `MixerReturn` |
| 4 | driver, `ChannelCount = 8` restored — the tree as committed | PASS — `MixerReturn` |

Run 3 is the A/B and it is a **negative** result worth keeping: the channel-count change was
not what made this work. See §4.

What *is* established, with evidence:

- **This machine loads hand-built third-party HAL drivers.** BlackHole, compiled locally
  with the same clang, assembled into a hand-made bundle, Developer ID signed and *not*
  notarised, installs and appears in the device list. So notarisation is not required, the
  signing method is fine, and the environment is not the obstacle.
- **libASPL is now the base** (MIT, compatible). The previous from-scratch AudioServerPlugIn
  was deleted. See §4 for why.
- **The GPL constraint is real.** BlackHole is GPL-3.0 and this repo is MIT, so BlackHole
  may be used as a *black-box behavioural reference* only. Do not copy its source.

---

## 2. The method that must not be skipped

**Never install an unproven driver on the working machine.** A bad AudioServerPlugIn can
take audio down for every application, and that is exactly what happened: six install cycles
on the host, a forced reboot, and a day lost. Use the VM.

```bash
cd ~/Projects/mixerreturn/device
./tools/vmtest.sh --control   # ALWAYS FIRST
./tools/vmtest.sh             # only once the control passes
```

`--control` installs libASPL's own unmodified `SinewaveDevice`, built and signed identically,
so the only difference is whose code is inside. **If the control does not pass, nothing the
harness says about our driver means anything** — fix the harness first.

"Signed identically" only became true on 2026-08-05. The control was being ad-hoc signed
while the driver carried a Developer ID, because `CMakeLists.txt` resolved the identity into
a variable that *shadowed* the cache entry rather than updating it — so `CMakeCache.txt` said
`-` while the bundle got the real certificate. Two variables were moving between control and
subject, which is precisely what a control exists to prevent. The cache is now written with
`FORCE` and `vmtest.sh` reads the identity back out of it.

**One difference remains and cannot easily be removed: architecture.** The driver is
universal; the control is arm64-only, because the example builds libASPL through an
`ExternalProject` that does not inherit `CMAKE_OSX_ARCHITECTURES` and a universal control
fails to link. Tried and confirmed — don't repeat it. If the control passes and the driver
fails, an arm64-only driver is a legitimate next test rather than a guess:

```bash
cmake -B build-arm64 -S . -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build-arm64
```

Each run clones a fresh macOS 26 VM. That is deliberate: a driver that half-registers leaves
CoreAudio in a state where the *next* test lies to you, and the host investigation went in
circles for hours on exactly that.

### The single biggest lesson from the first attempt

**Build the control before the subject.** Compiling BlackHole took ten minutes and settled in
one run what hours of bisection could not. Before that control existed, every failure looked
like it might be the bundle, the plist, the signature, the factory UUID, the path or the
machine — and each theory could be "confirmed" by a test that was itself unsound.

If you find yourself running a fourth variation of an A/B comparison, stop and go build a
known-good reference instead.

---

## 3. Results from the first attempt that are NOT trustworthy

This matters as much as the trustworthy findings. During the from-scratch attempt, `mrprobe`
produced **non-reproducible results**: the same bundle passed and failed across runs. Each of
these looked decisive at the time and none survived retesting:

- the Info.plist's content or encoding (XML vs binary, comments, key set)
- the factory UUID's value, and whether it was hand-written or generated
- duplicate copies of the same factory UUID on disk
- the bundle's path
- Developer ID vs ad-hoc signing, and hardened runtime
- CMake's bundle generation vs a hand-made bundle

**Do not treat any of the above as known.** They are recorded so that finding one of them
"again" is recognised as a repeat rather than a discovery. If one turns out to be real, prove
it in the VM with a control, not on a polluted host.

Two things *were* real bugs and are fixed:

- `mrprobe` was wired as a `POST_BUILD` step, so a failing probe made make delete the
  executable it had just linked, leaving an empty `Contents/MacOS`. Every probe after that
  failed for an entirely different reason than the original. It is now a separate `mr_check`
  target.
- `Initialize` was called with a NULL host, which segfaults a real driver. It is now behind
  `--init` and off by default.

`mrprobe` is still useful for a blatant structural failure — it exercises CFBundle, CFPlugIn,
the factory and QueryInterface in a process you can debug — but **it is not a gate and its
failures are not evidence the driver will not load.**

---

## 4. Why libASPL, and what is left to do

The from-scratch driver answered the HAL's property requests by hand. coreaudiod drops a
device over a single wrong answer, silently — no log line, no device — which is
indistinguishable from the bundle failing to load. That layer is large, unforgiving, and not
where this project's value is. libASPL owns it now.

`src/Driver.cpp` is only the part that is actually MixerReturn:

- **outputs** = `Sum 1..8`, one per rack
- **inputs** = four stereo buses, the summed result coming back
- the crosspoint between them, defaulting every Sum port to bus 1 so the device does
  something useful the moment it appears

### RESOLVED: the "stream format" mystery was never real

This section used to record that the device **appeared** with libASPL's default Int16 stereo
format and **stopped appearing** once the format was rebuilt as Float32 with 8 channels — an
unexplained result, backwards from expectation, and the top suspect for the whole failure.

**It does not reproduce.** The driver as written loads in a clean VM, and the ChannelCount
theory built on top of that observation is dead too — run 3 in §1 reverted it to the default
of 2 and the device still appeared. The A/B was run against a passing control, so it is a
real negative, not another unsound test.

The honest conclusion: this belongs in §3 with the other host-pollution artefacts. Every one
of those had the same shape — a decisive-looking result from a machine where a previous
half-registered driver had left CoreAudio lying to the next test. The lesson is not about
formats. It is that **nothing measured on the polluted host should have been written down as
a finding**, and the driver was very likely loadable well before anyone thought it was.

`MakeFloat32Format()` still derives every byte-count field from the channel count, and that
is still correct — the HAL validates those against each other. Keep it. Just don't credit it
with fixing anything.

If the VM says the device does not appear, the driver enables libASPL's **syslog tracer**, so
inside the VM:

```
log show --last 2m --predicate 'senderImagePath CONTAINS "MixerReturn"'
```

will show the property calls and their answers. `vmtest.sh` dumps this automatically on
failure. That diagnostic did not exist during the first attempt and is the main reason to
expect faster progress now.

---

## 4a. The summing works. The bus return is never asked for.

Measured 2026-08-05 with `./tools/vmtest.sh --verify`. `mrio probe` reports an **entirely
empty** crosspoint matrix — every bus return at −240 dBFS, which is not "quiet", it is exact
digital zero. But the driver is not idle, and the two instrumented log lines below are the
whole finding:

```
[mixerreturn] WriteMixedOutput #1 bytes=16384 frames=512 inPeak=0.200000 bus1Peak=1.551093
[mixerreturn] OnProcessMixedOutput frames=512 chans=8
```

`inPeak=0.2` is exactly mrio's `--amp 0.2` test tone arriving on the Sum ports, and
`bus1Peak=1.55` is eight of them summed. **The output path and the summing arithmetic are
correct.** `OnReadClientInput`, meanwhile, logs **zero** times — the HAL never asks the driver
to fill the input buffers, so mrio captures 96000 frames of zeroes that the HAL zero-filled
itself.

Established, so it need not be re-derived:

- Both streams exist: objectID 3 = output, objectID 6 = input, each with its volume and mute
  control. `mrio list` in the guest agrees — `MixerReturn  8 in / 8 out @ 48000`.
- The IO cycle genuinely runs: ~289 `BeginIOOperation`/`EndIOOperation` pairs,
  `GetZeroTimeStamp` ticking, `StartIO` twice.
- libASPL's `WillDoIOOperationImpl` advertises `ReadInput` whenever `numInputStreams_ > 0`,
  and the trace confirms both `AddStreamAsync` calls succeeded ("applying change in-place"),
  so the count is right and the HAL was told yes.
- `DoIOOperation` is traced against `objectID=2` — the *device*. The stream is a parameter,
  not the traced object, so that trace cannot tell you which stream is being serviced. Don't
  spend time on it as I did.

**The next experiment is a control, not another theory.** `mrio` is trusted against Pro Tools
Audio Bridge on the host — which is the same shape of device, a driver-internal loopback — but
it has never been run against a known-good loopback *inside a VM over ssh*. So the open
question is genuinely two questions, and one command separates them: install **BlackHole** in
the VM and point `mrio probe` at it.

- BlackHole loops back → the harness is sound, and the fault is ours.
- BlackHole is also silent → the fault is the environment, and the prime suspect is **TCC**:
  macOS gates audio *input* behind a microphone permission, an ssh session has no way to
  prompt, and a denial returns silence rather than an error. No TCC denial appeared in the
  guest log, but absence of a log line is not evidence here.

BlackHole is GPL-3.0 and this repo is MIT, so it stays a **black-box behavioural reference** —
install the built bundle, observe, do not read or copy its source. See §1.

### The instrumentation that produced this

`-DMR_TRACE_REALTIME=ON` turns on libASPL's realtime tracing *and* the `MR_RT_LOG` lines in
`Driver.cpp`. It is **OFF by default and must stay that way** — `syslog()` on a realtime audio
thread is not realtime-safe. It exists because "the device appears but no audio comes back" is
otherwise unfalsifiable: libASPL traces the property calls but not the IO callbacks, so there
is no way to distinguish a handler that never runs from one that runs and produces silence.
That distinction took one build to settle and is what turned this from a mystery into a
bounded question.

## 5. Next steps, in order

0. ~~**Unblock host networking.**~~ Cleared itself — see §6a. If a run dies at "VM never
   became reachable", read that section and just retry before doing anything drastic.
1. ~~**Wait for the base image.**~~ **Done 2026-08-05** — `macos-tahoe-base:latest` is pulled
   and cached (~31 GB in `~/.tart/cache/OCIs`). If it ever needs re-pulling: ~27 GB at
   roughly 3 MB/s from ghcr.io, so budget about two hours. It stages into `~/.tart/tmp/<hash>/`
   and only lands in the cache when the whole pull finishes, so `cache/OCIs` reading 0 B
   mid-pull is normal and **killing the pull throws away everything downloaded so far.**
   Check progress with `du -sh ~/.tart/tmp/*`.
2. ~~**Run `--control`.**~~ **Passes** (2026-08-05). Still run it first after any harness
   change — it is cheap and it is the only thing that makes a failure interpretable.
3. ~~**Run the real test.**~~ **Passes**, twice, on fresh VMs.
4. ~~**Get the device to appear at all.**~~ **Done.** See §1.
5. **Verify the summing numerically.** Half done, and the half that works is the interesting
   half — see §4a. `./tools/vmtest.sh --verify` does this now: it cross-compiles `mrio` for
   arm64 on the host, copies the binary into the VM (the base image has no toolchain) and
   runs `mrio probe` against the device. **The summing is correct; the bus return is silent.**
6. **Find out why `OnReadClientInput` is never called.** This is the front of the queue. §4a
   has the evidence and the next experiment.
6. **Only then the helper daemon**, which is what turns this from a pure virtual device into
   a wrapper. `src/mr_shared.h` is the driver↔helper contract and is unimplemented on the
   helper side.

### Deliberately deferred

- **The helper daemon and physical passthrough.** The device currently presents Sum ports and
  bus returns only. Wrapping a real interface needs the helper; nothing about it is written.
- **Windows.** A WDM driver, an entirely separate project. See root `AGENTS.md` §7.

---

## 6. Worth doing in parallel

Request the **`com.apple.developer.driverkit.family.audio`** entitlement against the Apple
developer account. It unlocks **AudioDriverKit driver extensions**, the modern replacement
for AudioServerPlugIn: a DEXT ships inside an app, the user approves it, and it uninstalls
cleanly — it cannot leave a wedged component in `/Library`. That retires this entire risk
category rather than managing it, and it is the right shipping vehicle for a product.
Approval has lead time, so requesting early costs nothing.

---

## 6a. When the VM is unreachable, it is the host's routing table — and it comes and goes

Hit 2026-08-05 on the first run after the base image downloaded: the control **built and
signed correctly**, the VM **booted**, and the run died at `ssh never came up`. Nothing about
it was the driver.

**Check this before believing any "the VM is broken" symptom, and check it again later —
the condition is intermittent.** Within the same session it cleared on its own: the shadowing
route disappeared, `192.168.64` reverted to `link#32 ... bridge102`, and the VM answered ping
at 0.5 ms with no intervention and no sudo. It is a lease the LAN hands out, so it comes back
when the lease renews or the machine changes network. **A failing run is worth simply
retrying** before anything more elaborate.

When it *is* present, this machine's LAN router advertises routes for the standard
virtualisation subnets and they shadow tart's own bridge:

```
$ netstat -rn -f inet | grep 192.168.64
192.168.64         172.16.0.1         UGSc    en0        <-- LAN router wins
192.168.64.4       e6.8a.70.b8.2a.83  UHLWIig bridge102  <-- where the VM actually is

$ route -n get 192.168.64.4
  gateway: 172.16.0.1
  interface: en0
```

So every packet the host sends to the VM leaves via Wi-Fi to the LAN router and is never
seen again. `10.211.55/24`, `10.37.129/24` and `10.147.17/24` — Parallels shared, Parallels
host-only, ZeroTier — are shadowed the same way, so this is a deliberate router
configuration, not an accident of this project.

**`tart ip` actively misleads you here.** Its default resolver reads `/var/db/dhcpd_leases`
keyed by MAC, and *every clone of the base image carries the same MAC*
(`e6:8a:70:b8:2a:83`), so it returns a previous VM's lease **instantly** — 0 s — for a VM
that is still booting or unreachable. `vmtest.sh` then burns its whole 120 s ssh budget
against a dead address and reports a timeout, which reads as a slow VM. Confirm with
`tart ip <vm> --resolver=arp`, which answers only when the host can actually see the VM.

Symptom checklist, so this is recognised rather than rediscovered: VM state `running`,
`tart ip` returns an address immediately, ping gets 100% loss, every port is closed.

### Fixes, and why the obvious ones are not available

Every clean fix needs root, and there is no passwordless sudo here (§7), so **these are the
user's to run, not yours**:

```bash
sudo route delete -net 192.168.64.0/24 172.16.0.1     # DHCP renewal may re-add it
sudo route add -net 192.168.64.0/24 -interface bridge102
```

Or move Apple's vmnet off the shadowed subnet entirely, which survives renewal:

```bash
sudo defaults write /Library/Preferences/SystemConfiguration/com.apple.vmnet \
     Shared_Net_Address -string 192.168.77.1
```

`--net-softnet` does **not** help: it still uses the same vmnet bridge subnet.

**`--net-bridged=en0` was tried and does not work — do not spend time on it again.** It is
the only sudo-free candidate and it puts the VM on the physical LAN, sidestepping
192.168.64/24 entirely, but `en0` here is **Wi-Fi** and it is the only interface tart offers
(`tart run --net-bridged=list` → `["en0 (or \"Wi-Fi\")"]`). The VM boots and reports
`running`, but its MAC never appears in the host's ARP table at all, which is the access
point refusing the second MAC address bridging requires. Verified 2026-08-05 over a full
boot: `tart ip mr-diag --resolver=arp` → `no IP address found`, `arp -an | grep e6:8a:70` →
nothing.

**So the harness is blocked on a root-level host change that only the user can make.** That
is the current state of Phase 2 — not a driver problem, and not something more driver work
can unblock.

## 7. Things that will waste your time if you do not know them

- **A rejected HAL plugin is completely silent.** No device, no log, nothing. Every failure
  mode looks identical from outside. This is the single hardest thing about the work and the
  reason for both the tracer and the VM harness.
- **`/Library/Audio/Plug-Ins/HAL` needs root**, and there is no passwordless sudo here. Every
  host install needs the user. In the VM the admin password is `admin`.
- **coreaudiod must be restarted** to pick up a driver, which drops audio for every client on
  that machine. Never on the host mid-session.
- **`lsof` cannot inspect coreaudiod** without root, so "is our binary mapped" is not a
  question you can answer that way — it returns empty for every driver, including working
  ones, which reads as a false negative.
- **`xcodebuild` needs `xcode-select` pointed at Xcode**, not CommandLineTools. Already set on
  this machine.
- **The Codex "Device Manager" warning about unsupported macOS is unrelated to this work.**
  The host runs macOS 26; that software refuses anything at or above macOS 16. It predates
  anything done here and is not worth investigating as a symptom.
- **`tart list` output is worth checking before a run** — a leftover `mixerreturn-test` VM
  from an interrupted run will be deleted and recreated, which is fine, but a *running* one
  holds the name.

---

## 8. File map

```
device/
  README.md            architecture: the Sum-port model and why it exists
  AGENTS.md            this file
  CMakeLists.txt       fetches libASPL, builds+signs the bundle, generates the factory UUID
  Info.plist.in        from libASPL's template; note sandboxSafe and the MachServices key
  src/Driver.cpp       the driver — Sum ports, crosspoint, bus returns
  src/mr_shared.h      driver <-> helper contract for the future wrapper (helper unwritten)
  tools/vmtest.sh      the test rig; --control first, always
  tools/mrprobe.c      in-process loader; useful, not authoritative (see §3)
```

The control surface lives at `../client/web-ui`, running against a simulated device behind a
`Device` seam in `app.js`. Wiring it to a real backend replaces that seam and puts out the
`SIMULATED` chip. It has not been touched by any of this work.
