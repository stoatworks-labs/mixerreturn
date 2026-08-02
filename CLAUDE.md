# mixerreturn (MixerReturn)

An interface for Waves SuperRack Performer: a shared summing bus across plugin instances, so
a Dugan Automixer can run on a console's direct outs and return the automixed result as a
single stereo pair to a group's External Input. VST3/AU/Standalone, JUCE/C++, CMake. v0.1.0.
Phase 1 of two; Phase 2 is the virtual audio device (see AGENTS.md §7).

**Lead with what it doesn't cost:** working on direct outs rather than inserts leaves the
desk's insert slots free for normal channel-strip plugins, puts the automixer post-fader
without moving any insert points, and uses one channel strip per mic.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Test: `./build/mrtest_artefacts/Release/mrtest`
- Test through the real VST3: `./build/mrhost_artefacts/Release/mrhost build/MixerReturn_artefacts/Release/VST3/MixerReturn.vst3`
- Built plugins land under `build/` as VST3/AU bundles.

## Notes
- JUCE plugin — DSP is real-time/allocation-sensitive; keep the audio thread lock-free and
  allocation-free. Bus membership changes go through `AsyncUpdater` onto the message thread.
- `Source/DSP/SummingBus.{h,cpp}` has no JUCE dependency. Keep it that way; it makes the bus
  testable on its own and portable into Phase 2's driver.
- PUBLIC at `stoatworks-labs/mixerreturn`, and listed at
  stoatworks-labs.com/software/mixerreturn/. Ships the AI disclaimer. Not yet tagged.
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

## Reality check
Never loaded in SuperRack Performer, never run against a real SQ, never used on a show. All
console behaviour comes from the SQ Reference Guide V1.6.0, not from hardware. Don't call it
field-proven.
