# AGENTS.md — picking up the Phase 2 device cold

Orientation for whoever continues this, human or model. The repo root's `AGENTS.md` covers
the **plugin** (Phase 1, shipped at v0.3.0). This file covers the **device** (Phase 2, not
working yet) and, more importantly, what has already been tried so it is not tried again.

Read `README.md` in this directory first for the architecture. This file is about state,
method and traps.

---

## 1. Status, stated plainly

**The driver has never loaded in coreaudiod. Not once, on any build.**

Everything else — the design, the shared-memory contract, the control surface, the routing
model — is in reasonable shape. The driver is not. Do not assume any part of it works.

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

### Known-suspect area: the stream format

The last host test showed the device **appear** with libASPL's default format (Int16 stereo,
inconsistent with the 8 channels requested) and then **not appear** after the format was
rebuilt as Float32. That is backwards from expectation and is unexplained.

`MakeFloat32Format()` derives every byte-count field from the channel count, which is correct
in isolation — the HAL validates those against each other, and the earlier version opened but
refused to start precisely because they contradicted. But whether the HAL is now rejecting
the device for a *different* format reason is exactly what the VM run should answer first.

If the VM says the device does not appear, the driver enables libASPL's **syslog tracer**, so
inside the VM:

```
log show --last 2m --predicate 'senderImagePath CONTAINS "MixerReturn"'
```

will show the property calls and their answers. `vmtest.sh` dumps this automatically on
failure. That diagnostic did not exist during the first attempt and is the main reason to
expect faster progress now.

---

## 5. Next steps, in order

1. **Wait for the base image.** `tart pull ghcr.io/cirruslabs/macos-tahoe-base:latest` was
   ~4.3 GB of ~27 GB at last check. Resume it if interrupted; tart is resumable.
2. **Run `--control`.** Prove the harness before believing anything about our driver.
3. **Run the real test.** If it fails, read the trace — do not start guessing.
4. **Get the device to appear at all.** That is the whole milestone. Ignore audio quality,
   latency and the helper daemon until a device shows up in a clean VM.
5. **Then verify the summing numerically.** `mrio` (see the root `AGENTS.md`) writes to the
   Sum ports and reads the buses back; the sum should be bit-exact against what was written.
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
