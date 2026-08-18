// MixerReturn — the Phase 2 virtual audio device.
//
// Rewritten on libASPL after a from-scratch AudioServerPlugIn failed to load at all. The
// property dispatch a HAL plugin has to answer is large, and a single wrong answer makes
// coreaudiod drop the device with no log line and no device in the list — a failure that
// is indistinguishable from the bundle not loading. libASPL owns that layer; this file is
// only the part that is actually MixerReturn.
//
// ---------------------------------------------------------------------------------------
// The routing model, which is the whole point
// ---------------------------------------------------------------------------------------
//
// SuperRack allows only ONE rack to patch to any given output I/O. Twenty-four racks can
// therefore never share a summing output, which is the constraint this project exists to
// get around. Give every rack its own `Sum n` port and sum a layer down, inside the device,
// and the constraint evaporates: a rack's output patch becomes the routing decision.
//
// Sum ports are OUTPUTS as far as the host is concerned — destinations SuperRack writes
// into. The summed buses come back as device INPUTS, so the whole loop closes inside one
// device and needs no loopback cable and no second rack per mic.
//
// Measured on 2026-08-04 and the reason the device beats the plugin: SuperRack's Dugan
// Automixer lives in a rack's *output stage*, after all eight plugin slots, so nothing in a
// rack can sit downstream of it. A Sum port is fed by the rack's output patch — the one
// place that is downstream. The device reaches what a plugin structurally cannot.

#include <aspl/Driver.hpp>

#include <CoreAudio/AudioServerPlugIn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef MR_TRACE_REALTIME
#include <syslog.h>
// Diagnostic only. libASPL's realtime tracing proves the HAL is *driving* the device, but it
// says nothing about whether OUR handlers run or what they see — which is the difference
// between "no audio because nothing is called" and "no audio because the sum is wrong".
// Rate-limited because these are realtime callbacks; syslog here is not realtime-safe and
// this must never ship on.
#define MR_RT_LOG(...) do { syslog(LOG_NOTICE, "[mixerreturn] " __VA_ARGS__); } while (0)
#else
#define MR_RT_LOG(...) do { } while (0)
#endif

namespace {

constexpr UInt32 SampleRate  = 48000;

// One Sum port per rack you want to automix. Eight is the demonstrable rig; the ceiling is
// what SuperRack will host racks for, not anything here.
constexpr UInt32 SumPorts    = 8;

// Four stereo buses, matching the control surface in ../client/web-ui.
constexpr UInt32 BusCount    = 4;
constexpr UInt32 BusChannels = BusCount * 2;

constexpr UInt32 MaxFrames   = 4096;

// Which buses each Sum port feeds, as a bitmask, and at what gain. This is the crosspoint
// matrix the control surface edits. Defaults put every Sum port into bus 1, so the device
// does something useful the moment it appears rather than needing configuration to prove
// it works at all.
struct Assign
{
    std::atomic<uint32_t> BusMask { 1u };
    std::atomic<float>    Gain    { 1.0f };
};

Assign gAssign[SumPorts];

// The summed buses, produced when the host writes its output and consumed when it reads its
// input. One block of buffering, because the read and write callbacks are separate and
// their order within a cycle is not ours to choose.
//
// Note this block is a property of *returning the sum as a device input*, not of the
// summing itself. Once the helper daemon wraps a real interface, a bus lands straight on a
// physical output in the same cycle it was written, with no added delay — which is the
// technical argument for the device over the plugin, whose two-page barrier exists only
// because plugin instances cannot see each other's timing.
struct BusBuffer
{
    std::atomic<UInt32> Frames { 0 };
    float Samples[BusChannels][MaxFrames] {};
};

BusBuffer gBuses;

class MixerReturnHandler : public aspl::IORequestHandler
{
public:
    // Everything SuperRack wrote to the Sum ports this cycle, all of it in hand at once.
    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        const void* bytes,
        UInt32 bytesCount) override
    {
        const auto* samples = static_cast<const Float32*>(bytes);
        const UInt32 frames =
            std::min(bytesCount / sizeof(Float32) / SumPorts, size_t(MaxFrames));

        // A member, not a local. BusChannels * MaxFrames floats is 128 KB, and this runs on
        // a HAL realtime thread inside coreaudiod whose stack we do not own or size. A local
        // of that size is a stack overflow waiting for the first host that asks for large
        // blocks, and it would land as an audio-server crash rather than anything traceable
        // back to here.
        auto& mix = Mix;
        std::memset(mix, 0, sizeof(float) * BusChannels * frames);

        for (UInt32 port = 0; port < SumPorts; ++port) {
            const uint32_t mask = gAssign[port].BusMask.load(std::memory_order_relaxed);
            if (mask == 0) {
                continue;
            }
            const float gain = gAssign[port].Gain.load(std::memory_order_relaxed);

            for (UInt32 bus = 0; bus < BusCount; ++bus) {
                if ((mask & (1u << bus)) == 0) {
                    continue;
                }
                // A Sum port is one rack, so it is mono; it lands on both legs of the
                // stereo bus. Panning belongs to the control surface, not here.
                for (UInt32 f = 0; f < frames; ++f) {
                    const float v = samples[f * SumPorts + port] * gain;
                    mix[bus * 2 + 0][f] += v;
                    mix[bus * 2 + 1][f] += v;
                }
            }
        }

        for (UInt32 ch = 0; ch < BusChannels; ++ch) {
            std::memcpy(gBuses.Samples[ch], mix[ch], sizeof(float) * frames);
        }
        gBuses.Frames.store(frames, std::memory_order_release);

        if ((WriteCalls++ % 200) == 0) {
            float inPeak = 0.0f, outPeak = 0.0f;
            for (UInt32 f = 0; f < frames * SumPorts; ++f) {
                inPeak = std::max(inPeak, std::abs(samples[f]));
            }
            for (UInt32 f = 0; f < frames; ++f) {
                outPeak = std::max(outPeak, std::abs(mix[0][f]));
            }
            MR_RT_LOG("WriteMixedOutput #%llu bytes=%u frames=%u inPeak=%.6f bus1Peak=%.6f",
                (unsigned long long) WriteCalls, bytesCount, frames, inPeak, outPeak);
        }
    }

    // And the buses handed back as device inputs, which is what returns to the desk.
    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
        const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        void* bytes,
        UInt32 bytesCount) override
    {
        auto* samples = static_cast<Float32*>(bytes);
        const UInt32 frames =
            std::min(bytesCount / sizeof(Float32) / BusChannels, size_t(MaxFrames));

        const UInt32 have = gBuses.Frames.load(std::memory_order_acquire);

        for (UInt32 f = 0; f < frames; ++f) {
            for (UInt32 ch = 0; ch < BusChannels; ++ch) {
                // Silence rather than stale audio when nothing has been written yet: a
                // device that repeats its last block sounds like it is working.
                samples[f * BusChannels + ch] = (f < have) ? gBuses.Samples[ch][f] : 0.0f;
            }
        }

        if ((ReadCalls++ % 200) == 0) {
            float peak = 0.0f;
            for (UInt32 f = 0; f < frames * BusChannels; ++f) {
                peak = std::max(peak, std::abs(samples[f]));
            }
            MR_RT_LOG("ReadClientInput #%llu bytes=%u frames=%u have=%u outPeak=%.6f",
                (unsigned long long) ReadCalls, bytesCount, frames, have, peak);
        }
    }

#ifdef MR_TRACE_REALTIME
    // Every other callback libASPL can issue, logged once each. The device is being driven —
    // BeginIOOperation, EndIOOperation and GetZeroTimeStamp all tick ~289 times — yet neither
    // handler above ever runs. Overriding the whole set answers the only question left: which
    // operation IS the HAL asking for? If none of these fire either, the fault is upstream of
    // the handler entirely (stream inactive, or the IO handler never installed).
    void OnProcessClientInput(const std::shared_ptr<aspl::Client>&,
        const std::shared_ptr<aspl::Stream>&, Float64, Float64,
        Float32*, UInt32 frameCount, UInt32 channelCount) override
    {
        if (OtherCalls[0]++ == 0) {
            MR_RT_LOG("OnProcessClientInput frames=%u chans=%u", frameCount, channelCount);
        }
    }

    void OnProcessClientOutput(const std::shared_ptr<aspl::Client>&,
        const std::shared_ptr<aspl::Stream>&, Float64, Float64,
        Float32*, UInt32 frameCount, UInt32 channelCount) override
    {
        if (OtherCalls[1]++ == 0) {
            MR_RT_LOG("OnProcessClientOutput frames=%u chans=%u", frameCount, channelCount);
        }
    }

    void OnWriteClientOutput(const std::shared_ptr<aspl::Client>&,
        const std::shared_ptr<aspl::Stream>&, Float64, Float64,
        const Float32*, UInt32 frameCount, UInt32 channelCount) override
    {
        if (OtherCalls[2]++ == 0) {
            MR_RT_LOG("OnWriteClientOutput frames=%u chans=%u", frameCount, channelCount);
        }
    }

    void OnProcessMixedOutput(const std::shared_ptr<aspl::Stream>&, Float64, Float64,
        Float32*, UInt32 frameCount, UInt32 channelCount) override
    {
        if (OtherCalls[3]++ == 0) {
            MR_RT_LOG("OnProcessMixedOutput frames=%u chans=%u", frameCount, channelCount);
        }
    }

    unsigned long long OtherCalls[4] {};
#endif

private:
    unsigned long long WriteCalls = 0;
    unsigned long long ReadCalls  = 0;

    // Scratch for OnWriteMixedOutput. Only ever touched from the realtime IO thread, so no
    // synchronisation; see the comment at its use for why it does not live on the stack.
    float Mix[BusChannels][MaxFrames] {};
};

// Float32 interleaved, N channels. Every byte-count field is derived from the channel
// count rather than left at a default, because the HAL validates them against each other.
AudioStreamBasicDescription MakeFloat32Format(UInt32 channels)
{
    AudioStreamBasicDescription format {};

    format.mSampleRate       = SampleRate;
    format.mFormatID         = kAudioFormatLinearPCM;
    format.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
                             | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel   = 32;
    format.mChannelsPerFrame = channels;
    format.mFramesPerPacket  = 1;
    format.mBytesPerFrame    = channels * sizeof(Float32);
    format.mBytesPerPacket   = channels * sizeof(Float32);

    return format;
}

std::shared_ptr<aspl::Driver> CreateDriver()
{
    // Trace to syslog. A HAL plugin that coreaudiod rejects produces no device and no log
    // line of its own, which is the single hardest thing about this whole exercise — every
    // failure looks identical from outside. With this on, the property calls and their
    // answers land in `log stream --predicate 'sender == "MixerReturn"'`, so a rejected
    // device can be read rather than guessed at.
    auto tracer = std::make_shared<aspl::Tracer>(
        aspl::Tracer::Mode::Syslog, aspl::Tracer::Style::Hierarchical);

    auto context = std::make_shared<aspl::Context>(tracer);

    aspl::DeviceParameters params;
#ifdef MR_TRACE_REALTIME
    // Off by default and it must stay that way: the tracer is not realtime-safe and the IO
    // callbacks run thousands of times a second. This exists because "the device appears but
    // no audio comes back" is otherwise unfalsifiable — the property calls are traced, the
    // IO calls are not, so there is no way to tell a handler that never runs from one that
    // runs and produces silence. Build with -DMR_TRACE_REALTIME=ON to find out which.
    params.EnableRealtimeTracing = true;
#endif
    params.Name         = "MixerReturn";
    params.Manufacturer = "Stoatworks Labs";
    params.DeviceUID    = "com.allansargeant.mixerreturn.device";
    params.ModelUID     = "com.allansargeant.mixerreturn.model";
    params.SampleRate   = SampleRate;

    // libASPL derives kAudioDevicePropertyPreferredChannelLayout from this field, and left at
    // its default of 2 it publishes a two-channel layout (Left, Right) for a device whose
    // streams carry eight channels each. Set for consistency. Both directions are eight wide
    // (SumPorts outputs, BusCount*2 bus returns), so there is one right answer.
    //
    // **This is not what made the device load.** It was A/B'd in the VM on 2026-08-05 against
    // a passing control, and the device appears either way — set to 8 or left at 2. The
    // "device stopped appearing" story in AGENTS.md §4 that this was meant to explain was one
    // more artefact of the polluted host, and has been moved to §3 with the rest of them.
    static_assert(SumPorts == BusChannels,
        "PreferredChannelCount can only describe both directions while they are equal");
    params.ChannelCount = SumPorts;

    // Not a default-device candidate: somebody's system alerts landing in a summing bus
    // mid-show is not a failure mode worth allowing.
    params.CanBeDefault = false;
    params.CanBeDefaultForSystemSounds = false;

    auto device = std::make_shared<aspl::Device>(context, params);

    // Outputs are the Sum ports; inputs are the summed buses coming back.
    //
    // The whole format has to be rebuilt, not just the channel count. libASPL's default is
    // Int16 stereo with mBytesPerFrame = 4; setting only mChannelsPerFrame leaves a
    // description whose byte counts contradict its channel count, and the device then
    // opens but refuses to start — which reads as a broken device rather than a bad format.
    aspl::StreamParameters sums;
    sums.Direction = aspl::Direction::Output;
    sums.Format = MakeFloat32Format(SumPorts);
    device->AddStreamWithControlsAsync(sums);

    aspl::StreamParameters returns;
    returns.Direction = aspl::Direction::Input;
    returns.Format = MakeFloat32Format(BusChannels);
    device->AddStreamWithControlsAsync(returns);

    device->SetIOHandler(std::make_shared<MixerReturnHandler>());

    auto plugin = std::make_shared<aspl::Plugin>(context);
    plugin->AddDevice(device);

    return std::make_shared<aspl::Driver>(context, plugin);
}

} // namespace

extern "C" void* MixerReturnEntryPoint(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    // Static, so the driver outlives this call and owns the whole object tree.
    static std::shared_ptr<aspl::Driver> driver = CreateDriver();

    return driver->GetReference();
}
