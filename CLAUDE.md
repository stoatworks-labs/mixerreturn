# mixerreturn (MixerReturn)

An interface for Waves SuperRack Performer: a shared summing bus across plugin instances, so
a Dugan Automixer can run on a console's direct outs and return the automixed result as a
single stereo pair to a group's External Input. VST3/AU/Standalone, JUCE/C++, CMake. v0.3.0.
Phase 1 of two; Phase 2 is the virtual audio device (see AGENTS.md §7).

**Lead with what it doesn't cost:** working on direct outs rather than inserts leaves the
desk's insert slots free for normal channel-strip plugins, puts the automixer post-fader
without moving any insert points, and uses one channel strip per mic.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Test: `./build/mrtest_artefacts/Release/mrtest`
- Test through the real VST3: `./build/mrhost_artefacts/Release/mrhost build/MixerReturn_artefacts/Release/VST3/MixerReturn.vst3`
- Measure inside a real host (macOS, hand-built — see the comment in `tools/mrio.cpp`):
  `clang++ -std=c++20 -O2 -o mrio tools/mrio.cpp -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework Accelerate`
- SuperRack scans `/Library/Audio/Plug-Ins/VST3/` only, and only at launch — installing means
  quitting it, copying the bundle there, ad-hoc signing it, and relaunching.
- Built plugins land under `build/` as VST3/AU bundles.

## Notes
- JUCE plugin — DSP is real-time/allocation-sensitive; keep the audio thread lock-free and
  allocation-free. Bus membership changes go through `AsyncUpdater` onto the message thread.
- `Source/DSP/SummingBus.{h,cpp}` has no JUCE dependency. Keep it that way; it makes the bus
  testable on its own and portable into Phase 2's driver.
- PUBLIC at `stoatworks-labs/mixerreturn`, and listed at
  stoatworks-labs.com/software/mixerreturn/. Ships the AI disclaimer.
  "Commit" = commit **and** push.

## The rule that matters
**One block of delay, uniform across every sender, whatever order the host uses.** Uniform
beats short: an uneven delay across senders comb-filters the sum. Never "optimise" the
master into reading the current block.

Load-bearing details that look like they can be simplified but cannot:
- **No lock in `processBlock` — not even a try-lock.** The first commit guarded the bus
  lookup with a process-wide SpinLock and skipped the block (including `arrive()`) on a
  failed try. Sequential tests always win an uncontended try-lock, so it looked fine until
  instances ran on separate threads and silently dropped out of the sum. Bus and slot are
  packed in one atomic instead.
- `processBlockBypassed` must still call `arrive()` or the barrier wedges forever.
- A muted member must `clearSlot()`, not skip writing, or its last block sticks in the sum.
- `arrive()` uses `>=` not `==` so a member leaving mid-block can't wedge the barrier.
- A slot's buffers are sized only while that slot is inactive, and only that slot — a
  joining instance must not reallocate underneath instances already streaming.
- **Report the block the host actually processes, not the one it prepared.** `samplesPerBlock`
  is a maximum; SuperRack prepares 2048 and processes 256, so reporting the prepared value
  claimed 42.7 ms for a 5.3 ms bus and the host displayed it. And never clear `observedBlock`
  in `prepareToPlay` — a latency report makes the host re-prepare, which turns that into a
  10 Hz feedback loop. Both traps are written up in AGENTS.md §4.

## Verifying changes
`mrtest` pushes real audio through the actual shipped `MixerReturnAudioProcessor` and checks
the sum numerically. It covers the summing instance processed first *and* last, 24 senders
with the order reshuffled every block, and 400 blocks with 17 instances racing on their own
threads — because a barrier bug is inaudible right up until it comb-filters, and listening
will never catch it.

**Sequential tests are not enough here, and that is not a theoretical point** — the
concurrent test is what found the try-lock bug above, after every sequential test passed.
Run the TSan build too (recipe in AGENTS.md §8); it is currently clean.

`mrhost` covers what `mrtest` structurally cannot: it loads the built bundle and asks it for
instances, confirming they share one registry across the plugin-format boundary. Note VST3
hashes string param IDs to integers, so a host addresses parameters by **name**, not by the
IDs in PluginParameters.h.

Use **relative** tolerances when comparing sums: accumulating N floats in a different order
than the reference legitimately differs by a couple of ULP.

## The topology, corrected
**One rack cannot do it.** SuperRack's Dugan is a rack *output-stage* module (slot 8, after
all eight user slots), there is no Dugan plugin to insert, and nothing can sit downstream of
it — so a MixerReturn in the same rack sends the **pre**-automix signal. Measured: rack
outputs Dugan-attenuated to −4.39 dB while the bus sum carried the raw inputs at +0.02 dB,
last slot included. It needs two racks per channel with a loopback between them. Full
measurements and the working topology are in AGENTS.md §1a.

## Reality check
Verified with real audio in SuperRack Performer v15.15.12 on 2026-08-04 (48 kHz/256, over a
loopback virtual device): bit-exact sum, uniform 256-sample delay, trim/mute/bypass/bus
isolation all exact. Never run against a real SQ or any console, and never used on a show —
all console behaviour still comes from the SQ Reference Guide V1.6.0. Don't call it
field-proven.
