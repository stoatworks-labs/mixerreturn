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
to a single stereo pair, back into a group's External Input.

**The topology is two racks per channel, not one — see §1a.** An earlier version of this
document said each rack runs "a Dugan instance followed by a MixerReturn instance". That is
not possible, and the correction matters more than any other fact in this file.

**The product claim is what it doesn't cost.** Working on direct outs rather than inserts
means the desk's **insert slots stay free for normal plugin inserts on the channel strips**,
the automixer sits post-fader without relocating any insert points, and no channel is used
twice. It adds an automixer to a desk without taking anything away from it. Lead with that
when describing the project — it is the reason anyone would choose this over inserting the
automixer per channel.

That claim is about the **console**, and it survives §1a intact — no channel strip is used
twice and no insert slot is taken. What §1a costs is on the **SuperRack** side: two racks and
a loopback channel per mic. Don't blur the two when describing the project.

Phase 1 of a two-phase project. Phase 2 is the virtual audio device that was the original
idea — see §7.

## 1a. The Dugan is not a plugin, and nothing can be inserted after it

Measured in SuperRack Performer v15.15.12 on 2026-08-04, with real audio.

**SuperRack's Dugan Automixer is part of the rack's output stage, not a plugin slot.** A
rack has eight user plugin slots, indexed 0–7 in the session database. Enabling the Dugan
on a rack writes a `Dugan Speech` module at **slot 8** with `plug_role = 16`, and the rack's
OUTPUT panel gains a "Dugan Automixer" label. There is no slot after it, and there is **no
Dugan available as an insertable plugin** — searching the plugin browser for "dugan" or
"automix" returns nothing.

Two consequences, both verified rather than reasoned:

- **The gain sharing senses post-plugin-chain.** With two racks fed equal noise, both
  settled at −4.4 dB. Raising one rack's level by 10 dB *inside its plugin chain* moved the
  two outputs to +8.9 dB and −11.1 dB — a difference of **19.99 dB**, i.e. the 10 dB of
  plugin gain plus a 10 dB shift in the shared gain. If the detector tapped the rack input,
  the other rack would not have moved at all.
- **A MixerReturn in the same rack therefore sends the *pre*-automix signal.** Measured: the
  rack outputs were Dugan-attenuated to −4.39 dB while the bus sum carried the raw inputs at
  **+0.02 dB**. Putting MixerReturn in the *last* user slot (7) changes nothing — still
  +0.00 dB. The sum contained no automixing whatsoever.

**The topology that works, and is verified end to end:**

```
direct out -> Rack A: [ ...plugins... ] -> Dugan (output stage) -> output N
                                                                     |
                                                                     v  (loopback)
              Rack B: input N -> MixerReturn (send to bus) -> unused output
              ...
              Rack Z: MixerReturn in Bus Sum mode -> the return to the desk
```

Rack B's input must be fed from Rack A's output, which needs a loopback path — a spare
physical I/O pair, or a virtual audio device. Verified on a 32-channel loopback device: the
bus sum came out bit-exact against the sum of the two Dugan-processed feeds, residual
−178 dBFS, one 256-sample block of delay, uniform.

The cost is a second rack and a loopback channel per mic. **This is the strongest argument
yet for Phase 2** (§7): a driver that owns the I/O makes the loopback free, and the product
claim in §1 stops depending on burning console I/O to get the automixed signal back.

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
tools/mrio.cpp               CoreAudio rig that measures the plugin inside the real host
tools/resid.py               Checks a captured sum against the captured senders, exactly
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
- **"One block" means the block the host actually calls, not the one it prepared for.**
  `prepareToPlay`'s `samplesPerBlock` is a *maximum*. SuperRack prepares with 2048 and then
  processes 256, so reporting `preparedBlock` claimed 42.7 ms for a bus that delays 5.3 ms —
  and the host believed it, showing 42.7 ms in the rack's LTNC readout. The audio thread
  records the real block size in `observedBlock` (a plain relaxed store, nothing more) and a
  10 Hz `Timer` on the message thread turns it into `setLatencySamples`. Every test that
  prepares and processes at the same size is blind to this; `testLatencyFollowsActualBlockSize`
  deliberately does not.
- **Do not clear `observedBlock` in `prepareToPlay`.** Reporting a latency change makes the
  host re-prepare, so clearing it produced a feedback loop with SuperRack — prepare 2048,
  report 2048, process 256, timer reports 256, host re-prepares, forget, report 2048 — ten
  times a second, releasing and reacquiring a bus slot on every pass. Carrying the previous
  run's value over means the re-prepare reports the same number, `setLatencySamples` sees no
  change, and the host is told nothing. This one is invisible to `mrtest` and was caught only
  by watching the plugin's own log while SuperRack ran.

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
looked like it made a plugin sufficient for the actual use case at a fraction of the cost —
no AudioServerPlugIn to notarize, no nested-ASIO fragility, no Steinberg SDK redistribution
constraint, no EV certificate and Microsoft attestation for a WDM kernel driver.

**§1a retired that reasoning.** A plugin is *not* sufficient for the automixer use case,
because no plugin slot can sit downstream of the Dugan — the plugin needs a second rack and
a loopback channel per mic to see the automixed signal at all. The measurement in §1a is
therefore positive evidence for the Phase 2 architecture rather than merely an argument for
it: a `Sum` port is fed by the rack's **output patch**, which is downstream of the Dugan
output stage, so the driver picks up the automixed signal at exactly the point a plugin
cannot reach. The virtual output port is not a convenience over the plugin here — it is the
thing that makes the product work as described in §1.

None of which makes the plugin useless. It stays situationally useful for the cases in the
"what the plugin still gives you" paragraph below, and for summing that does not involve an
automixer at all, where a single rack is enough and the Dugan constraint never applies.

**The target host is SuperRack Performer, not SoundGrid.** An earlier version of this
document claimed Phase 2 would cover SuperRack SoundGrid; that was wrong. SoundGrid racks
take their I/O from SoundGrid network hardware rather than from a CoreAudio or ASIO device,
so a virtual audio device is not visible to SuperRack SoundGrid as rack I/O at all. The
SoundGrid *driver* is a different matter — it presents CoreAudio/ASIO to the computer like
any other interface, so it could in principle be the device being wrapped. That is reasoning
from how the pieces fit, not something tested.

**The device is the product; the plugin is the version that works without installing one.**

Once the wrapper exists, a rack's output patch *is* the routing decision — physical output to
behave as an insert, or a `Sum` port to feed the buses — with no plugin in the chain at all.
The wrapper is also strictly better technically: a driver receives every Sum port in one
callback, so it can sum with **no added delay**, where the plugin must cost a block. The
entire two-page barrier in this plugin exists only because plugin instances cannot see each
other's timing; a driver has no such problem.

What the plugin still gives you, honestly and completely: it exists today; it needs no driver
install, no admin rights, no notarized system extension and no signed kernel driver; it does
not depend on successfully wrapping any particular interface; and it therefore runs on locked
down or rented machines where a system audio device is not an option. That is "easier to
deploy", not "does something the device cannot". The one capability gap runs the other way —
routing between separate applications, which a per-process plugin bus can never do.

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

**Done in the real host (2026-08-03):** loaded in Waves SuperRack Performer v15.15.12 on
macOS, two instances in separate racks, both reading "2 members on this bus" — so the
cross-instance registry works through the real plugin-format boundary in the real host, not
just in `mrhost`. No audio device was attached, so no audio was passed.

**Real audio through the real host (2026-08-04):** SuperRack Performer v15.15.12, 48 kHz,
256-sample buffer, I/O on a 32-channel loopback virtual device (Pro Tools Audio Bridge 32 —
verified bit-transparent 1:1 first, and it shares happily with SuperRack). Independent white
noise per channel, recovered by cross-correlation, so every gain and delay below is measured
rather than inferred:

- Pass-through (`Output = Input`) is **bit-exact** and adds no delay: gain +0.00 dB,
  correlation 1.000, residual at float-rounding level.
- The bus sum equals the sum of the senders **delayed by exactly 256 samples**, uniform
  across all senders — 0 of 143744 samples and 0 of 561 blocks in error, residual −163 dBFS.
- −6 dB of send trim came back as 0.501187 (−6.00 dB). A muted send came back as **exactly
  zero**, not a stale block.
- A bypassed instance contributed exactly zero **and the barrier did not wedge** — so
  SuperRack does keep calling a bypassed plugin, and `processBlockBypassed`'s `arrive()`
  earns its keep in the real host.
- Moving one instance to another bus removed it from the sum and left the rest bit-exact;
  the instance reported "1 member on this bus", the others "4".
- Latency now reads 5.3 ms in the rack's LTNC readout, matching the measured 256 samples.

`tools/mrio.cpp` and `tools/resid.py` reproduce all of it; the build line and the usual
invocation are in the comment at the top of `mrio.cpp`. Note that measuring latency this way
needs a device that is genuinely bit-transparent — `mrio probe` checks that before you trust
anything else.

**Still never done:** run against a real SQ or any console, or use it on a show. Every claim
about console behaviour still comes from the reference guide, not from hardware. The audio
above went through a virtual loopback device, not a desk. Do not describe this as
field-proven.

**Install location is not optional on macOS:** SuperRack scans only
`/Library/Audio/Plug-Ins/VST3/` and never `~/Library/Audio/Plug-Ins/VST3/`. A plugin in the
user folder is invisible to it with no error. Ad-hoc signing is fine — Developer ID and
notarization are *not* required to be hosted. SuperRack scans at launch only.

