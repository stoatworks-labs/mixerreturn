# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*mixerreturn — shared summing bus letting a Dugan Automixer bolt onto a desk without one; phase 1 plugin done, phase 2 virtual audio device deferred*

**mixerreturn** (`~/Projects/mixerreturn`, JUCE/C++ VST3+AU+Standalone, MIT, v0.1.0).
Started 2026-08-02. **PUBLIC** at github.com/stoatworks-labs/mixerreturn, ships the AI
disclaimer. **LIVE** at stoatworks-labs.com/software/mixerreturn/. No video yet, not tagged.

`mrshot` renders the editor straight to PNG (send/sum/pair) — screen-capturing the
standalone was unreliable *and* could never show a populated bus, since one process = one
instance = "1 member". mrshot registers 24 senders so the readouts are real. See
[screenshot capture methods](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_screenshot_capture_methods.md).

An **interface for Waves SuperRack Performer**: runs the Dugan Automixer across a console's
direct outs and returns the automixed result summed to a **single stereo pair** into a
**group's External Input**.

**Lead with what it doesn't cost** (the user's own framing): working on direct outs rather
than inserts leaves the **insert slots free for normal channel-strip plugins**, puts the
automixer **post-fader** without moving insert points, and uses one channel strip per mic.

**ONE RACK CANNOT DO THE AUTOMIXER JOB** (measured 2026-08-04, AGENTS.md §1a). SuperRack's
Dugan is a rack **output-stage** module at slot 8, after all eight user plugin slots; there
is no Dugan available as an insertable plugin. So a MixerReturn in the same rack sends the
**pre**-automix signal — rack outputs were Dugan-attenuated to −4.39 dB while the bus sum
carried the raw inputs at +0.02 dB, last slot included. Gain sharing senses **post**-plugin
(10 dB of plugin gain moved the two outputs 19.99 dB apart). The working topology is **two
racks per channel with a loopback between them**, verified bit-exact.

**Originally conceived as a virtual audio device** wrapping CoreAudio/ASIO/WDM. Deferred to
phase 2 on the belief that VST3 hosting (v14+) made a plugin sufficient — **that reasoning is
retired**: the finding above means a Sum port, fed by the rack's output patch, picks up the
automixed signal at exactly the point a plugin cannot reach. The measurement is evidence
*for* the phase 2 architecture. The plugin stays situationally useful, especially for summing
with no automixer involved. It does **NOT** serve SuperRack SoundGrid — I claimed that and was
wrong; SoundGrid takes its I/O from SoundGrid hardware, not CoreAudio or ASIO. See
[superrack routing](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_superrack_routing.md).

**Phase 2 device: it LOADS as of 2026-08-05.** `MixerReturn` appears in the CoreAudio device
list in a clean VM, behind a passing control — after a history of never loading once. Built on
**libASPL** (MIT, `device/src/Driver.cpp`); the from-scratch AudioServerPlugIn was deleted.
Scope it: coreaudiod accepts the bundle, and that is all. **No audio has been through it** —
summing unverified, helper daemon unwritten, never run on the host or in a real host app.

Two things that did *not* turn out to be true, both worth not rediscovering: the "device
appears with Int16, vanishes with Float32" mystery **does not reproduce**, and the
`DeviceParameters::ChannelCount` theory built on it was **refuted by A/B** (device appears set
to 8 or left at 2). Both were artefacts of a polluted host — as was most of the first attempt's
evidence. Test in the throwaway VM (`device/tools/vmtest.sh`, `--control` first, always),
never on the working machine; a bad AudioServerPlugIn takes audio down for every app.
VM networking gotcha: [vm subnet shadowing](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_vm_subnet_shadowing.md).

**The one design rule:** the bus sum is delayed by exactly one block, *identically for every
sender*, whatever order the host processes instances in. Two pages per bus; senders write
one while readers read the other; the last member to arrive flips them. **Uniform beats
short** — an uneven delay across senders comb-filters the sum rather than just shortening it.

Verified numerically by `mrtest` (kept in the build, `octest`/`phtest` precedent — a barrier
bug is inaudible until it combs), and **with real audio in SuperRack Performer 2026-08-04**:
bit-exact sum, uniform 256-sample delay, trim/mute/bypass/bus-isolation all exact. Driven via
`tools/mrio.cpp` over a **loopback virtual device** — no sudo here, so a HAL driver cannot be
installed; Pro Tools Audio Bridge 32 is already present, is bit-transparent 1:1 at 48 kHz, and
shares fine with SuperRack. SuperRack's session is a readable **SQLite** DB at
`/Users/Shared/Waves/SuperRack Performer/Sessions/CurrentSPRKP.dat` — query it for ground
truth instead of reading screenshots.

**Latency trap fixed:** `samplesPerBlock` is a *maximum*; SuperRack prepares 2048 and
processes 256, so reporting the prepared value claimed 42.7 ms for a 5.3 ms bus and the host
displayed it. And never clear the observed block size in `prepareToPlay` — a latency report
makes the host re-prepare, which turns that into a 10 Hz feedback loop.

**Still never run against a real SQ or used on a show.** Console behaviour all comes from the
reference guide — see [sq direct out ext in](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_sq_direct_out_ext_in.md).
