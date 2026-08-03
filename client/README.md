# MixerReturn client — Phase 2 control surface

The control surface for the MixerReturn **virtual audio device**: the device passes a
physical interface's I/O straight through and adds virtual ports that can be summed and
sent to chosen outputs. This is the screen you configure that from.

Phase 1 — the plugin in the repository root — does the same summing job inside Waves
SuperRack Performer. Phase 2 exists to cover what a plugin structurally cannot: SuperRack
**SoundGrid** (which hosts no VST3), and routing between separate applications.

## Status: the UI, running on a simulated device

**There is no driver yet.** Everything below the `Device` seam in `web-ui/app.js` is
simulated — channel names come from a fixed descriptor and levels from a wandering signal
generator. The `SIMULATED` chip in the menu bar and the status line at the foot both say so,
and they stay lit until a real backend replaces that seam. A control surface that looks
live when it isn't is worse than one that plainly admits it.

One thing is kept honest even in simulation: **the summed return really is the sum of the
inputs**, panned into L and R, rather than its own independent wobble. That is the entire
product, and faking it separately would hide the only part worth looking at.

Save, Load, Setup and Diagnostics open panels that describe what they will do and state
plainly that they are not wired up.

## Running it

```bash
python3 -m http.server 8801 --directory client/web-ui
```

No build step — plain HTML, CSS and JS, so the driver's helper process can serve the
directory as-is.

## How it will ship

Per the fleet pattern, this becomes a local web UI served by the driver's helper, with
[av-launcher](https://github.com/stoatworks-labs/av-launcher) vendored alongside as
`client/launcher/` to give it a tray presence, a port picker and Start/Stop — the same
arrangement `srt-router`, `flock` and `RFutils` use.

## Design

Built to a supplied mockup. The palette was sampled from it rather than guessed:

| Role | Colour |
| --- | --- |
| Background | `#322b23` |
| Panel / strip | `#2d2621` |
| Meter trough, fader track | `#191410` |
| Divider | `#40372c` |
| Amber accent | `#deab4f` |
| Meter green | `#87b457` |
| Fader cap, body text | `#d2c8ba` |

Two deliberate departures from the mockup, both for use rather than looks:

- **The disclosure arrow rotates.** The mockup draws a right-pointing arrow on sections
  that are open; here it points down when open and right when closed, which is the
  convention everyone already reads without thinking.
- **Meter left, fader track right — everywhere.** The mockup mirrors the two in the
  summed-return panel. A surface that flips its own convention halfway across the screen
  is one somebody misreads at speed.

Each strip's number is its fader position; the return panel's readout is a peak-holding
meter of the summed bus, so the two are different kinds of figure by design.
