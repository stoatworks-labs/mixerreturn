# MixerReturn

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The summing bus has been verified
> numerically against the actual shipped processor class — a one-block delay with the
> summing instance processed both first and last, 24 senders with the host's processing
> order reshuffled on every block, 400 blocks with 17 instances racing on their own
> threads, plus trim, mute, bypass participation and bus isolation (`mrtest`), and it is
> clean under ThreadSanitizer. `pluginval` passes clean at strictness 8 on VST3, and on AU
> with one known benign wrapper warning. It has **not** been loaded in SuperRack Performer, **not** been
> run against a real console, and **not** been used on a show. Every claim about SQ
> behaviour comes from the reference guide, not from hardware. Review before use on live gear.

Bolt a Dugan Automixer onto a console that hasn't got one — post-fader, without spending an
insert slot or a second channel strip per mic.

## What it's for

MixerReturn is an interface for **Waves SuperRack Performer**. It lets the Dugan Automixer
run across a console's channel **direct outs**, and returns the automixed result summed down
to a **single stereo pair**, which comes back into a **group's External Input**.

The point is what it *doesn't* cost you. Because the automixer works on the direct outs
rather than on channel inserts:

- **The insert slots stay free.** You can still insert plugins on the channel strips exactly
  as normal — the automixer isn't competing for them.
- **The automixer sits post-fader**, without relocating any insert points.
- **No channel is used twice.** One channel strip per mic, as usual.

In short, it adds an automixer to a desk without taking anything away from it.

| A channel instance | The return instance |
| --- | --- |
| ![MixerReturn on a channel: Bus 1 selected, Send enabled, Mute off, send trim at 0.0 dB, the send meter showing programme level, Output set to Input so the rack passes its own audio through, and a readout of 25 members on this bus](docs/screenshots/send.png) | ![MixerReturn as the return: Bus 1 selected, Send disabled so the send meter is empty, Output set to Bus Sum with the output meter showing the summed level, and a readout of 25 members on this bus alongside 64 samples latency](docs/screenshots/sum.png) |

*Both rendered from the real editor by `mrshot`, with a full 24-channel rig registered on
the bus — which is why the member count and the latency readout are the actual values, not
a mock-up.*

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
summing bus, and `mrshot` regenerates the screenshots above:

```bash
./build/mrtest_artefacts/Release/mrtest
```

## Status

v0.1.0. The summing bus is verified numerically — including 24 senders with the processing
order reshuffled on every block, and 17 instances processing concurrently on their own
threads — it is clean under ThreadSanitizer, and `pluginval` passes at strictness 8. But it
**has not been run inside SuperRack Performer, or against a real SQ**. Nothing here has
been near a live show.

That concurrent test earned its place: it found a real bug that every sequential test
passed straight through. The audio path used to take a process-wide try-lock and skip the
block when it lost, which is invisible on one thread and drops instances out of the sum on
several. [docs/DESIGN.md](docs/DESIGN.md) has the detail.

## Roadmap

Phase 2 is the original idea: a virtual audio device that wraps a CoreAudio/ASIO/WDM device,
passes its I/O through, and adds summable virtual ports. That covers what a plugin
structurally cannot — SuperRack **SoundGrid** (no VST3), and routing between separate
applications.

## Licence

MIT. See [LICENSE](LICENSE).
