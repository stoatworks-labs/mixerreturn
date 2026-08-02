# MixerReturn — design notes

## The problem

The Dugan Automixer is the reference solution for gain-sharing across open mics, but most
digital consoles don't have one. Bolting a plugin-host Dugan onto a desk that lacks it
normally means inserting the automixer on every channel, which costs an insert slot per
channel and puts the automixer wherever the desk's insert point happens to sit — usually
somewhere pre-fader. The alternative, running each mic through two channel strips, doubles
the channel count.

MixerReturn exists to enable a third arrangement:

1. Tap every mic channel's **direct out**, set to follow the fader.
2. Send those to the plugin host, one rack per channel, each with a Dugan instance.
3. **Sum the automixed channels** down to one signal.
4. Return that sum to the desk as a mix's **External Input**.

The operator keeps working ordinary channel strips. Dugan sees post-fader signals. No
insert slots are consumed and no channel is used twice. Step 3 is the only piece that
neither the desk nor the plugin host provides — which is what this is.

## Why the host can't do the summing

Waves SuperRack's patcher is explicit that **only one rack can patch to a given output
I/O**. Patching twenty-four rack outputs onto one output to sum them is not available;
the last one simply wins. The input side is more permissive (one I/O can feed several rack
inputs) but that doesn't help here.

SuperRack Performer gained third-party VST3 hosting in v14, so the summing can be done by
a plugin instead of by a virtual audio driver. That is the whole reason this project starts
as a plugin — see [ROADMAP](#roadmap) for where the driver still fits.

## Why a shared bus, and not just a wide plugin

The Waves Dugan Automixer works as one instance per channel, exchanging gain information
across a group. SuperRack racks in this configuration are mono: one input, one output. So
there is no single place where all twenty-four post-Dugan signals exist in one buffer for a
wide plugin to sum.

MixerReturn therefore uses the same shape Dugan does: N instances, one shared bus, and one
designated instance that outputs the sum on its own rack output.

## The barrier, which is the entire design

A host processes plugin instances in an order it chooses, possibly across several threads,
and that order is not knowable from inside a plugin.

A naive shared accumulator breaks badly here. An instance summing its peers cannot assume
they have already run this block. Any sender scheduled *after* the summing instance would
contribute nothing that block; any sender scheduled *before* it would contribute this
block. Different channels would then arrive at the sum with different delays — and on an
automix bus that is not a latency problem, it is a comb filter.

So the bus never lets anyone read the block currently being written:

- Each bus keeps **two pages** of per-slot buffers.
- Senders always write the **write page**.
- Readers always read the **other** page, which is by definition complete.
- Every member calls `arrive()` as the last thing it does in its block. The member that
  arrives last **flips the pages**.

The result is one block of delay, *identical for every member*, regardless of the order the
host picked. Uniform beats short: a single constant delay on the return path is inaudible
in this application, whereas a per-channel delay spread of even a few samples is not.

### Consequences that must be respected

- **A bypassed instance must still arrive.** `processBlockBypassed` participates in the
  barrier and writes silence. If it didn't, the barrier would never complete and every
  member would freeze one block behind, permanently.
- **A muted or non-sending member must actively clear its slot**, not merely skip writing.
  Skipping would leave its last block in the sum forever.
- **The summing instance reports one block of latency**; a passthrough sender reports zero.
- **The audio path takes no lock at all**, and this is not a stylistic preference. Bus and
  slot are packed into one atomic word. The first version used a process-wide try-lock and
  skipped the block on failure; see "The lock that broke it" below. Bus membership changes
  still happen on the message thread, because acquiring a slot may allocate and telling the
  host about a latency change must not happen from the audio callback. The barrier uses
  `>=` rather than `==` so a member disappearing mid-block cannot wedge it.
- **A slot's buffers are sized only while that slot is inactive, and only that slot.** An
  instance joining an existing bus must not reallocate buffers underneath instances that
  are already streaming, which is why a slot is `claimed` before it is `active`.
- **The bus is per-process.** Instances in two separate hosts cannot see each other. This
  is a real constraint, not an oversight — crossing process boundaries would need shared
  memory and a cross-process barrier, and the target use case keeps every rack inside one
  SuperRack.

### The lock that broke it

Worth recording, because it is the failure this design is most likely to have reintroduced
if something ever goes wrong here again.

The first implementation looked up its bus and slot behind a **process-wide `SpinLock`**,
taken from the audio thread with `tryEnter`, skipping the block when the try failed. The
reasoning was that a skipped block during reconfiguration is harmless. The flaw is that the
lock was shared by *every* instance, so it was not only contended during reconfiguration —
it was contended whenever two instances processed at the same time, which is the normal
case for a host running racks across several audio threads. Losers skipped their entire
block, including `arrive()`, so they dropped out of the sum *and* left the barrier an
arrival short, desynchronising every other member.

Every sequential test passed throughout, and always would have: one thread always wins an
uncontended try-lock. It took a test that gives each instance its own thread to expose it,
on the second block. The fix was to remove the lock from the audio path entirely.

The general lesson is the ordinary real-time audio one, but it is worth stating in the form
it took here: **a try-lock is not a safe way to make a shared structure real-time — it just
converts contention into silently dropped work**, and dropped work in a barrier is worse
than a stall.

## Console-side notes (Allen & Heath SQ)

Verified against the SQ Reference Guide V1.6.0. These generalise to other desks in shape
but not in detail.

### The return lands cleanly

> "A 'Mix Ext In' can be used with any mix channel. This routes audio directly from a
> source to the mix without processing, or any routing and level control."

Available on *any* mix, with Source Select, Trim and Polarity. Mono mixes take a mono
external input; stereo mixes take a stereo one. That is the return path: no input channel
consumed, and no fader sitting in it waiting to be knocked.

### Post-fader direct outs are a global switch — this is the trap

The direct out tap points are Post-Preamp, Post-HPF, Post-Gate, Insert Return, Post-PEQ,
Post-Comp and Post-Delay. **There is no post-fader tap point.** Post-fader behaviour comes
from a separate setting, and the spec table is blunt about its scope:

> `Channel Direct Out — Follow Fader, Mute, Mute Group, DCA (global all ch)`

So "Follow Fader" is all-or-nothing across all 48 channels. If the same desk is
multitracking off direct outs — the normal SQ recording path — that recording becomes
post-fader too, which is almost never wanted.

**The fix is tie lines.** They patch an input socket straight to an output socket, bypassing
the processing path entirely, and the reference guide's own worked example is exactly this
collision: unprocessed audio for multitrack while direct outs carry processed audio
elsewhere. So multitrack goes out on tie lines post-preamp, and direct outs go post-fader
to the automixer.

Direct out **trim is per-channel** (-inf to +10 dB), which is why MixerReturn's own send
trim uses the same range and reads the same way.

### Other console facts worth having

- The SQ core runs at **96 kHz**, so the whole plugin rig must.
- Channel count is not a constraint: SLink gigaACE carries 128×128 at 96 kHz. Option cards
  cover Dante, Waves and MADI.
- Latency on the return path is a *uniform* delay, so it does not comb-filter — **provided
  the automixed channels are unrouted from the main mix**, leaving the summed return as
  their only path there. Any channel reaching the main mix both directly and through the
  return will comb. Pre-fader monitor sends stay inside the desk and are unaffected.

## Roadmap

**Phase 1 (this) — the plugin.** Solves the Dugan case on macOS and Windows with no driver
work: no AudioServerPlugIn to notarize, no nested-ASIO fragility, no Steinberg SDK
redistribution constraint, no EV certificate or Microsoft attestation for WDM.

**Phase 2 — the wrapper virtual audio device.** The general product, and still worth
building: a virtual device that passes a physical device's I/O through and adds virtual
ports that can be summed to chosen outputs. It covers what the plugin structurally cannot —
SuperRack **SoundGrid** (which hosts SoundGrid-format plugins only, no VST3), and routing
between separate applications. Three separate platform efforts: macOS AudioServerPlugIn,
Windows ASIO wrapper, Windows WDM.

## Sources

- [SuperRack routing and patching options](https://www.waves.com/support/superrack-routing-and-patching-options)
- [Third-party VST3 in SuperRack Performer](https://forum.waves.com/t/new-superrack-performer-now-runs-any-vst3-plugin/8509)
- [Allen & Heath SQ Reference Guide V1.6.0](https://www.allen-heath.com/content/uploads/2023/05/SQ_ReferenceGuide_V1_6_0_iss2.pdf)
