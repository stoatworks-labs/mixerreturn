# AGENTS.md — bringing an LLM up to speed on MixerReturn

Orientation for an AI assistant (or a new human) picking this project up cold. Read this
before proposing changes. `CLAUDE.md` holds the short command reference; this file explains
the *why*, and the traps.

---

## 1. What this is

A **shared summing bus across plugin instances**, shipping as VST3, AU and Standalone,
built with JUCE in C++.

It is an **interface for Waves SuperRack Performer**. It exists for one job: let a Dugan
Automixer run across a console's channel direct outs and return the automixed result summed
to a single stereo pair, back into a group's External Input. The desk's direct outs feed one
rack per channel, each rack runs a Dugan instance followed by a MixerReturn instance, and one
further instance emits the sum.

**The product claim is what it doesn't cost.** Working on direct outs rather than inserts
means the desk's **insert slots stay free for normal plugin inserts on the channel strips**,
the automixer sits post-fader without relocating any insert points, and no channel is used
twice. It adds an automixer to a desk without taking anything away from it. Lead with that
when describing the project — it is the reason anyone would choose this over inserting the
automixer per channel.

Phase 1 of a two-phase project. Phase 2 is the virtual audio device that was the original
idea — see §7.

## 2. The one rule that matters most

**The bus sum is delayed by exactly one block, identically for every sender, regardless of
the order the host processes instances in.**

Uniformity is the guarantee, not speed. A shorter but *uneven* delay across channels does
not sound like lower latency, it sounds like a comb filter, because all the senders land in
the same sum. If you find yourself optimising the delay away — "the master could just read
the current block when it runs last" — stop. That is the bug this design exists to prevent,
and it will pass every listening test until the host happens to reorder its threads.

`tests/mrtest.cpp` exists to catch exactly that regression. Run it.

## 2a. The bug that shipped in the first commit, so it isn't reintroduced

The first version guarded the audio path's bus lookup with a **process-wide `SpinLock`**,
taken with `tryEnter`. On failure it skipped the block — including `arrive()`.

That is invisible to any sequential test, because a single thread always wins an
uncontended try-lock, and every test at the time drove `processBlock` from one thread. The
moment instances ran on different threads at once — which is exactly what a host does with
racks — they contended on that one shared lock, losers silently dropped out of the sum, and
their missing arrivals desynchronised the barrier for everyone.

The fix was to delete the lock from the audio path entirely and pack bus and slot into one
atomic. **If you find yourself adding any lock, allocation or blocking call to
`processBlock`, this is the failure you are recreating.** `testConcurrentProcessing` in
`mrtest` is the regression guard; it caught this, and a sequential test never would have.

## 3. Layout

```
Source/
  PluginProcessor.{h,cpp}    Host-facing entry point; owns bus membership and the block work
  PluginEditor.{h,cpp}       Top-level GUI
  PluginParameters.h         Every parameter ID, range and default
  DSP/
    SummingBus.{h,cpp}       The shared bus, the two-page buffers and the barrier.
                             Deliberately free of any JUCE dependency.
  GUI/
    LevelBar.h               Peak bar with slow decay
  Diag/                      Vendored diagnostics module, copied unchanged across the fleet

tests/mrtest.cpp             Headless numerical verification of the bus
tests/mrhost.cpp             The same check, but run through the built .vst3 as a host
tools/mrshot.cpp             Renders the editor to PNG for the documentation
docs/DESIGN.md               Why it works this way, plus the console-side findings
```

`mrtest` and `mrhost` answer different questions and you want both. `mrtest` constructs the
processor directly, so it can drive concurrency and ordering precisely. `mrhost` loads the
real bundle and asks it for instances, which is the only way to confirm the assumption the
whole product rests on: that instances a host creates from one bundle **share one loaded
copy of the registry**. If that were ever false — per-instance sandboxing, a bundle loaded
twice — every instance would report a bus of one and the plugin would output silence while
every control and meter carried on working.

**VST3 parameter IDs are not the string IDs.** VST3 IDs are 32-bit integers, so JUCE hashes
them on the way out: a host sees `bus` as `97920`. Names survive intact, which is why
`mrhost` addresses parameters by name. Tell anyone trying to automate this by ID.

## 4. How it actually works

Each bus keeps two **pages** of per-slot buffers. Senders always write the write page.
Readers always read the *other* page, which is by definition complete. Every member calls
`arrive()` as the last thing it does in its block, and whichever member arrives last flips
the pages. That is the entire mechanism.

Things that follow from it, each of which is load-bearing:

- **`processBlockBypassed` must still call `arrive()`.** A bypassed instance still holds a
  slot. If it stopped arriving, the barrier would never complete and every member would
  freeze one block behind, permanently.
- **A muted or non-sending member calls `clearSlot()`**, it does not merely skip writing.
  Skipping leaves its last block in the sum forever.
- **`arrive()` uses `>=` not `==`.** A member disappearing mid-block would otherwise wedge
  the barrier for good. The cost is that a reconfiguration can flip twice in one block —
  an audible tick at worst, self-correcting on the next.
- **The audio path must never take a lock. Not even a try-lock.** Bus and slot live packed
  in one `std::atomic<uint32_t>` on the processor for exactly this reason. Bus membership
  still changes on the message thread via `AsyncUpdater` — acquiring a slot may allocate,
  and `setLatencySamples` notifies the host — but the audio thread only ever does an
  acquire-load of the assignment.
- **A slot's buffers are only sized while the slot is inactive**, and only that slot's:
  a new instance joining must not reallocate underneath instances already streaming. The
  handoff to readers is the release store of `active`, which is why `Slot` has both
  `claimed` and `active`.
- **Latency is reported per output mode**: one block if this instance emits the sum, zero if
  it merely passes its input through.

## 5. The bus is per-process

Instances in two separate hosts cannot see each other. That is a deliberate constraint, not
an oversight — crossing process boundaries means shared memory and a cross-process barrier,
and the target use case keeps every rack inside one SuperRack. If someone asks for
cross-application summing, that is Phase 2's job, not a patch to this.

## 6. Console-side facts (verified, don't re-derive)

From the SQ Reference Guide V1.6.0. Full detail and quotes in `docs/DESIGN.md`.

- **Mix External In exists on any SQ mix**, with Source Select, Trim and Polarity. Mono mix
  takes mono, stereo takes stereo. That's the return path and it works.
- **There is no post-fader direct out tap point.** Tap points are Post-Preamp, Post-HPF,
  Post-Gate, Insert Return, Post-PEQ, Post-Comp, Post-Delay. Post-fader comes from a
  separate "Follow Fader" switch which is **global across all 48 channels** — so enabling it
  makes any direct-out multitrack post-fader too. The fix is to route multitrack over **tie
  lines** instead. Do not present the post-fader arrangement without this caveat.
- **The SQ core runs at 96 kHz.** SLink gigaACE carries 128×128 at that rate, so channel
  count is not a constraint.
- **Latency on the return is uniform, so it does not comb** — but only if the automixed
  channels are unrouted from the main mix. Any channel reaching the mix both directly and
  via the return will comb.

## 7. Phase 2, and why it was deferred

The original concept was a **virtual audio device** wrapping a CoreAudio/ASIO/WDM device,
passing its I/O through and adding summable virtual ports. It is still the more general
product and still worth building.

It was deferred because SuperRack Performer gained third-party VST3 hosting in v14, which
made a plugin sufficient for the actual use case at a fraction of the cost — no
AudioServerPlugIn to notarize, no nested-ASIO fragility, no Steinberg SDK redistribution
constraint, no EV certificate and Microsoft attestation for a WDM kernel driver.

**The target host is SuperRack Performer, not SoundGrid.** An earlier version of this
document claimed Phase 2 would cover SuperRack SoundGrid; that was wrong. SoundGrid racks
take their I/O from SoundGrid network hardware rather than from a CoreAudio or ASIO device,
so a virtual audio device is not visible to SuperRack SoundGrid as rack I/O at all. The
SoundGrid *driver* is a different matter — it presents CoreAudio/ASIO to the computer like
any other interface, so it could in principle be the device being wrapped. That is reasoning
from how the pieces fit, not something tested.

What the plugin cannot do and Phase 2 can: take the plugin out of the chain altogether, and
route between separate applications.

## 8. What has and hasn't been verified

**Verified:** the summing bus, numerically, by `mrtest` — one-block delay with the summing
instance processed first and last, 24 senders with the order reshuffled every block, 400
blocks with 17 instances each on their own thread racing concurrently, trim, mute, bypass
participation, and bus isolation. Clean under **ThreadSanitizer**, and `pluginval` at
strictness 8 passes clean on VST3 and passes on AU with one benign JUCE-wrapper warning
("Current program is -1").

To rebuild the TSan variant:

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" && cmake --build build-tsan --target mrtest
```

**Never done:** loaded in SuperRack Performer. Run against a real SQ or any console. Used on
a show. Every claim about console behaviour comes from the reference guide, not from
hardware. Do not describe this as field-proven.
