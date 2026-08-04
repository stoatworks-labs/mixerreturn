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
#include <cstring>
#include <vector>

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

        float mix[BusChannels][MaxFrames];
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
    }
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
    params.Name         = "MixerReturn";
    params.Manufacturer = "Stoatworks Labs";
    params.DeviceUID    = "com.allansargeant.mixerreturn.device";
    params.ModelUID     = "com.allansargeant.mixerreturn.model";
    params.SampleRate   = SampleRate;
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
