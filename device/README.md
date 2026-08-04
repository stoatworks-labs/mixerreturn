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

Early. The port model and the shared-memory contract are the parts worth reviewing first;
the IO path is not yet wired to real hardware.
