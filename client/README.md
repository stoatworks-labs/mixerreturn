# MixerReturn client — Phase 2 control surface

The control surface for the MixerReturn **virtual audio device**. This is the screen you
configure it from.

## The routing model

The device wraps a physical interface and presents to the host:

| | |
| --- | --- |
| **Inputs** | the physical inputs, passed straight through |
| **Outputs** | the physical outputs, passed straight through, **plus one `Sum n` port per input** |

**The Sum ports are outputs as far as the host is concerned.** In SuperRack, a rack takes a
passed-through physical input, and its output goes to one of two places:

- a **passed-through physical output** — the rack behaves as an ordinary insert, straight
  back to the desk;
- a **Sum port** — the rack feeds the summing buses instead.

That single choice, per channel, is the whole workflow. A strip used as an insert works as
it always did; a strip used post-fader for Dugan goes to the sum.

**One Sum port per rack is what makes this work at all.** SuperRack allows only one rack to
patch to any given output I/O, so twenty-four racks cannot share one summing output. Giving
each rack its own port and doing the summing inside the device sidesteps that completely.

Sum ports are assigned to up to **eight stereo buses** through a crosspoint matrix, with
console-style assign switches on each strip for the common case — the idiom is lifted from
the numbered assign switches down a Midas channel strip. Each bus lands on a physical output
pair, which is what returns to the desk as a group's External Input.

Phase 1 — the plugin in the repository root — does the same summing job from inside
SuperRack Performer, one instance per rack. The device does it a layer down, which means no
plugin in the chain at all and the routing decision moves to where the rack is patched.

## Status: the UI, running on a simulated device

**There is no driver yet.** Everything below the `Device` seam in `web-ui/app.js` is
simulated — channel names come from a fixed descriptor and levels from a wandering signal
generator. The `SIMULATED` chip in the menu bar and the status line at the foot both say so,
and they stay lit until a real backend replaces that seam. A control surface that looks
live when it isn't is worse than one that plainly admits it.

One thing is kept honest even in simulation: **the summed return really is the sum of the
inputs**, panned into L and R, rather than its own independent wobble. That is the entire
product, and faking it separately would hide the only part worth looking at.

Sum channel levels are not invented either: each is its corresponding input's post-fader
signal, because that is literally what a rack patched this way would be sending. The bus
meters are then the real sum of whatever is assigned to them.

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

The fader bank is three rows — INPUTS, OUTPUTS, SUM SENDS — rather than the mockup's two,
because the Sum ports needed somewhere to live that wasn't buried among the physical
outputs. The strip switches and the matrix are two views of one set of assignments, and
both repaint from the same state: an open matrix showing a crosspoint that was just
switched off on a strip is exactly the sort of thing that gets patched wrong.

Each strip's number is its fader position; the return panel's readout is a peak-holding
meter of the summed bus, so the two are different kinds of figure by design.
