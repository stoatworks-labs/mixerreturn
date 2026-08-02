# mixerreturn (MixerReturn)

Shared summing bus across plugin instances, so a Dugan Automixer in SuperRack Performer can
be bolted onto a console without one — post-fader, no insert slots, no doubled channel
strips. VST3/AU/Standalone, JUCE/C++, CMake. v0.1.0. Phase 1 of two; Phase 2 is the virtual
audio device (see AGENTS.md §7).

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Test: `./build/mrtest_artefacts/Release/mrtest`
- Built plugins land under `build/` as VST3/AU bundles.

## Notes
- JUCE plugin — DSP is real-time/allocation-sensitive; keep the audio thread lock-free and
  allocation-free. Bus membership changes go through `AsyncUpdater` onto the message thread.
- `Source/DSP/SummingBus.{h,cpp}` has no JUCE dependency. Keep it that way; it makes the bus
  testable on its own and portable into Phase 2's driver.
- Local git only so far — no remote yet, and public/private not yet decided.

## The rule that matters
**One block of delay, uniform across every sender, whatever order the host uses.** Uniform
beats short: an uneven delay across senders comb-filters the sum. Never "optimise" the
master into reading the current block.

Load-bearing details that look like they can be simplified but cannot:
- `processBlockBypassed` must still call `arrive()` or the barrier wedges forever.
- A muted member must `clearSlot()`, not skip writing, or its last block sticks in the sum.
- `arrive()` uses `>=` not `==` so a member leaving mid-block can't wedge the barrier.

## Verifying changes
`mrtest` pushes real audio through the actual shipped `MixerReturnAudioProcessor` and checks
the sum numerically. It covers the summing instance processed first *and* last, and 24
senders with the order reshuffled every block — because a barrier bug is inaudible right up
until it comb-filters, and listening will never catch it.

Use **relative** tolerances when comparing sums: accumulating N floats in a different order
than the reference legitimately differs by a couple of ULP.

## Reality check
Never loaded in SuperRack Performer, never run against a real SQ, never used on a show. All
console behaviour comes from the SQ Reference Guide V1.6.0, not from hardware. Don't call it
field-proven.
