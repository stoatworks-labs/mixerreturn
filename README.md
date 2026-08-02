# MixerReturn

Bolt a Dugan Automixer onto a console that hasn't got one — post-fader, without spending an
insert slot or a second channel strip per mic.

MixerReturn is a VST3/AU/Standalone plugin that gives a plugin host something it doesn't
otherwise have: a **shared summing bus** spanning many instances. Put one instance after
the automixer on each channel's rack, and one more instance set to output the sum. That
sum returns to the desk as a mix's External Input.

## The signal flow

```
SQ channels 1-24
  |  direct outs, set to follow the fader
  v
SLink / Dante / Waves card  ---->  SuperRack Performer
                                     Rack 1..24:  Dugan Automixer -> MixerReturn (Send)
                                     Rack 25:                       MixerReturn (Bus Sum)
                                                                          |
SQ mix "External Input"  <-------------------------------------------------
```

The operator keeps working ordinary channel strips. Dugan sees post-fader signals. No
insert slots are consumed and no channel is used twice.

## Why this exists

SuperRack's patcher allows **only one rack per output I/O**, so twenty-four rack outputs
cannot be patched onto one output to sum them. The desk can't sum them either without
spending twenty-four input channels to do it. This fills that one gap.

## Controls

| Control | What it does |
| --- | --- |
| **Bus** | Which of the 8 shared buses this instance joins. |
| **Send** / **Mute** | Whether this instance contributes its input to the bus. |
| **Send Trim** | -inf to +10 dB on the contribution, matching the SQ's own direct out trim range. |
| **Output** | `Input` (pass through), `Bus Sum` (emit the sum), or `Input + Bus Sum`. |
| **Output Trim** | -inf to +10 dB on this instance's output. |

Sending and receiving are independent, so an instance can be a sender, a receiver, or both.

## The one thing to know

The bus sum is delayed by **exactly one block**, identically for every sender, no matter
what order the host processes the instances in. That uniformity is deliberate and is the
reason the design looks the way it does — a shorter but *uneven* delay across channels
would comb-filter the sum. See [docs/DESIGN.md](docs/DESIGN.md).

## Setting it up on an Allen & Heath SQ

Two console-side details that will bite otherwise:

- **"Follow Fader" on direct outs is global across all 48 channels.** There is no
  post-fader tap point; post-fader comes from that separate switch, and it is all-or-nothing.
  If you also multitrack off direct outs, that recording becomes post-fader too. Route
  multitrack over **tie lines** (post-preamp, bypassing the processing path) instead.
- **Unroute the automixed channels from the main mix.** The summed return must be their
  only path there. A channel reaching the mix both directly and through the return will comb.
  Pre-fader monitor sends stay inside the desk and are fine.

The SQ core runs at 96 kHz, so the plugin host must too.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

VST3, AU and Standalone land under `build/`. `mrtest` is a headless numerical check of the
summing bus:

```bash
./build/mrtest_artefacts/Release/mrtest
```

## Status

v0.1.0. The summing bus is verified numerically — including 24 senders with the processing
order reshuffled on every block — but **has not yet been run inside SuperRack Performer, or
against a real SQ**. Nothing here has been near a live show.

## Roadmap

Phase 2 is the original idea: a virtual audio device that wraps a CoreAudio/ASIO/WDM device,
passes its I/O through, and adds summable virtual ports. That covers what a plugin
structurally cannot — SuperRack **SoundGrid** (no VST3), and routing between separate
applications.

## Licence

MIT. See [LICENSE](LICENSE).
