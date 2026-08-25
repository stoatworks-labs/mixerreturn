# MixerReturn device — Phase 2

The virtual audio device. It wraps a physical interface, passes its I/O straight through,
and adds a **Sum port per input** that the summing engine collapses into stereo buses.

This is the version of MixerReturn that does the job properly. The plugin (Phase 1, the
rest of this repo) exists because this did not, and it carries a cost the device does not:
see `AGENTS.md` §1a and §7.

## Why the Sum ports are the whole design

SuperRack allows **only one rack to patch to any given output I/O**. So twenty-four racks
can never share one summing output — which is the constraint the entire project exists to
get around.

Give every rack its own `Sum n` port and sum a layer down, inside the device, and the
constraint evaporates. A rack's output patch becomes the routing decision:

| Rack output goes to | What happens |
|---|---|
| a passed-through physical output | ordinary insert, straight back to the desk |
| `Sum n` | into the summing buses, which land on a physical output pair |

Sum ports are **outputs** from the host's point of view — destinations SuperRack writes
into — which is why the control surface groups them with OUTPUTS rather than in a section
of their own.

Measured on 2026-08-04, and the reason this is worth building rather than patching the
plugin: SuperRack's Dugan Automixer lives in a rack's **output stage**, after all eight
plugin slots. Nothing in a rack can sit downstream of it, so a plugin can only ever tap
the pre-automix signal. A Sum port is fed by the rack's *output patch* — the one place
downstream of the Dugan. The device reaches what the plugin structurally cannot.

## Architecture

An AudioServerPlugIn runs inside `coreaudiod` and cannot open another CoreAudio device, so
wrapping needs two pieces and a shared memory region between them:

```
   SuperRack (or any host)
        |  CoreAudio
        v
   +---------------------------+          +----------------------+
   |  MixerReturn.driver       |  shared  |  mixerreturnd        |
   |  (AudioServerPlugIn,      |<-------->|  (helper daemon)     |
   |   inside coreaudiod)      |  memory  |                      |
   |                           |          |  IOProc on the REAL  |
   |  - presents mirrored      |          |  interface           |
   |    physical in/out        |          |  - hw in  -> shm     |
   |  - presents Sum 1..N      |          |  - shm -> hw out     |
   |  - sums Sum ports into    |          |  - control API for   |
   |    8 stereo buses         |          |    the web UI        |
   +---------------------------+          +----------------------+
```

**The hardware is the clock.** The wrapped device's IOProc drives everything; the virtual
device's timeline is anchored to it via `GetZeroTimeStamp`. Getting that anchoring right is
the hard part of this project, not the port model.

**The summing costs no added delay**, which is the technical argument for the device over
the plugin. The host writes every output buffer for a cycle — including all the Sum ports —
before the cycle ends, so the driver has them all in hand at once and can sum them into the
same block. The plugin's entire two-page barrier exists only because plugin instances cannot
see each other's timing; a driver has no ordering to defend against.

## Layout

```
device/
  src/mr_shared.h     the driver <-> helper contract: shared memory layout, ring buffers
  src/mr_driver.c     the AudioServerPlugIn itself
  Info.plist.in       bundle metadata; CFPlugInFactories points at the factory function
```

The control surface already exists at `../client/web-ui`, running against a simulated
device behind a `Device` seam in `app.js`. Wiring it to the helper's control API replaces
that seam and puts out the `SIMULATED` chip.

## Building and installing

```bash
cmake -B build -S . && cmake --build build
```

Installing needs **root**, and there is no way around it — `coreaudiod` only loads drivers
from a root-owned directory, and it has to be restarted to pick one up:

```bash
sudo cp -R build/MixerReturn.driver /Library/Audio/Plug-Ins/HAL/
sudo killall coreaudiod
```

`coreaudiod` restarting drops every audio client on the machine for a second or two, so do
not run it mid-show. If the device does not appear afterwards, `log stream --predicate
'subsystem == "com.apple.coreaudio"'` while restarting is the only thing that says why —
a driver that fails to load fails silently in the device list.

## Status

**The device works as a summing device.** As of 2026-08-25 it loads in a clean VM, presents
`8 in / 8 out @ 48 kHz`, and `./tools/vmtest.sh --verify` measures audio written to every Sum
port arriving on both legs of bus 1 at the level it was sent, with buses 2..4 correctly
silent — the shipped default crosspoint. Reproducible, no manual steps.

**The helper daemon exists as of 2026-08-25** (`helper/mixerreturnd.cpp`). It opens a real
device, runs its IOProc, moves hardware I/O in and out of the shared rings, publishes the
hardware's clock every cycle — measured at +48128 frames per second against a 48 kHz device —
and recovers on its own when coreaudiod restarts or the interface disappears. It was also
settled first, with a control, that POSIX shared memory really is reachable from inside the
driver's sandbox; see `AGENTS.md` §5a.

**It is still not a wrapper end to end.** The driver side is not wired to the region: nothing
reads `captureRing` or writes `playbackRing`, so no audio has yet crossed between the two
processes, the device still presents a fixed 8 Sum ports / 4 buses instead of mirroring the
hardware's real channel counts, and none of this has been run in SuperRack Performer.

The port model and the shared-memory contract are still the parts worth reviewing first.
`AGENTS.md` §4a is worth reading before touching the IO path — the last two faults there
were a macOS permission and a one-line buffer clear, neither of them where they looked.
