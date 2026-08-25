/*  mixerreturnd — the helper daemon.
 *
 *  An AudioServerPlugIn runs inside coreaudiod and cannot open another CoreAudio device, so
 *  wrapping a real interface needs a second process. This is it. It owns the hardware, runs
 *  its IOProc, and exchanges audio with the driver through the shared region defined in
 *  ../src/mr_shared.h.
 *
 *  Division of labour, which is not arbitrary:
 *
 *    - the DRIVER sums, because it receives every Sum port for a cycle in one callback and
 *      can therefore mix with no added delay
 *    - the HELPER moves bytes and owns the clock, because it is the only side that can see
 *      the hardware
 *
 *  THE HARDWARE IS THE CLOCK. Every cycle this publishes the device's own timestamp into
 *  the shared region, and the driver anchors GetZeroTimeStamp to it. Two free-running clocks
 *  would drift and the wrapper would slowly slip against the interface it is wrapping — the
 *  part ../README.md calls the hard problem of this project.
 *
 *  Realtime discipline inside RenderProc: no allocation, no locks, no syslog, no CoreAudio
 *  calls. It is on the hardware's IO thread, and a stall here is a dropout in every
 *  application on the machine, not just ours.
 */

#include "../src/mr_shared.h"

#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

MRShared*             gShm      = nullptr;
AudioDeviceID         gDevice   = kAudioObjectUnknown;
AudioDeviceIOProcID   gIOProc   = nullptr;
std::atomic<bool>     gRunning { true };

// The UID of the device being wrapped, kept for recovery. Recovery must re-resolve by UID
// and not reuse the AudioDeviceID: coreaudiod hands out fresh IDs when it restarts, and the
// old one refers to nothing.
std::string           gWrappedUID;

// Channel counts of the wrapped hardware, cached at open. Read on the realtime thread, so
// they are fixed for the lifetime of the run: a device that changes its stream configuration
// underneath us is handled by exiting, not by reallocating on the audio thread.
UInt32 gNumInputs  = 0;
UInt32 gNumOutputs = 0;

// -------------------------------------------------------------------------------------
// Property helpers. All of these are control-thread only.
// -------------------------------------------------------------------------------------

std::string DeviceUID(AudioDeviceID dev)
{
    CFStringRef uid = nullptr;
    UInt32 size = sizeof(uid);
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &uid) != noErr || !uid) {
        return {};
    }
    char buf[512] {};
    CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(uid);
    return buf;
}

std::string DeviceName(AudioDeviceID dev)
{
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    AudioObjectPropertyAddress addr {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &name) != noErr || !name) {
        return {};
    }
    char buf[512] {};
    CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(name);
    return buf;
}

// Channel count for one scope. Sums across buffers because a device may present its channels
// as one interleaved buffer or as many mono ones, and both are normal.
UInt32 ChannelCount(AudioDeviceID dev, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    std::vector<uint8_t> storage(size);
    auto* abl = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, abl) != noErr) {
        return 0;
    }
    UInt32 total = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) {
        total += abl->mBuffers[i].mNumberChannels;
    }
    return total;
}

double NominalSampleRate(AudioDeviceID dev)
{
    Float64 sr = 0;
    UInt32 size = sizeof(sr);
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &sr) != noErr) {
        return 0;
    }
    return sr;
}

std::vector<AudioDeviceID> AllDevices()
{
    AudioObjectPropertyAddress addr {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr) {
        return {};
    }
    std::vector<AudioDeviceID> devs(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devs.data()) != noErr) {
        return {};
    }
    return devs;
}

// Refuse to wrap ourselves. A helper that opens MixerReturn feeds the driver its own output
// and the whole thing becomes a feedback loop inside coreaudiod, which is not a mistake that
// announces itself politely.
bool IsOurOwnDevice(const std::string& uid)
{
    return uid == "com.allansargeant.mixerreturn.device";
}

// -------------------------------------------------------------------------------------
// The ring buffers. Single producer, single consumer, no waiting on either side.
// -------------------------------------------------------------------------------------

// A writer that finds no room drops the block and counts it. Dropping is correct: the
// alternative is blocking an audio thread, and a dropout you can count is better than a
// stall you cannot.
inline bool RingWrite(MRRing& r, const float* src, uint32_t frames, uint32_t stride)
{
    const uint64_t w = atomic_load_explicit(&r.write, std::memory_order_relaxed);
    const uint64_t rd = atomic_load_explicit(&r.read,  std::memory_order_acquire);

    if (w - rd + frames > MR_RING_FRAMES) {
        return false;
    }
    for (uint32_t f = 0; f < frames; ++f) {
        r.data[(w + f) & MR_RING_MASK] = src[f * stride];
    }
    atomic_store_explicit(&r.write, w + frames, std::memory_order_release);
    return true;
}

// A reader that finds nothing emits silence rather than repeating its last block. A device
// that repeats sounds like it is working, which is the worse failure.
inline bool RingRead(MRRing& r, float* dst, uint32_t frames, uint32_t stride)
{
    const uint64_t rd = atomic_load_explicit(&r.read,  std::memory_order_relaxed);
    const uint64_t w  = atomic_load_explicit(&r.write, std::memory_order_acquire);

    if (w - rd < frames) {
        for (uint32_t f = 0; f < frames; ++f) {
            dst[f * stride] = 0.0f;
        }
        return false;
    }
    for (uint32_t f = 0; f < frames; ++f) {
        dst[f * stride] = r.data[(rd + f) & MR_RING_MASK];
    }
    atomic_store_explicit(&r.read, rd + frames, std::memory_order_release);
    return true;
}

// -------------------------------------------------------------------------------------
// The IOProc. Realtime thread — see the discipline note at the top of the file.
// -------------------------------------------------------------------------------------

OSStatus RenderProc(AudioObjectID,
                    const AudioTimeStamp*  /*now*/,
                    const AudioBufferList* inInput,
                    const AudioTimeStamp*  inInputTime,
                    AudioBufferList*       outOutput,
                    const AudioTimeStamp*  /*outOutputTime*/,
                    void*)
{
    MRShared* shm = gShm;
    if (shm == nullptr) {
        return noErr;
    }

    uint32_t frames = 0;

    // ---- hardware input -> capture rings -------------------------------------------
    // Walk buffers rather than assuming a layout: an interface may hand over one buffer of
    // N interleaved channels or N buffers of one channel, and both are normal.
    if (inInput != nullptr) {
        uint32_t ch = 0;
        for (UInt32 b = 0; b < inInput->mNumberBuffers && ch < MR_MAX_CHANNELS; ++b) {
            const auto& buf = inInput->mBuffers[b];
            const auto* data = static_cast<const float*>(buf.mData);
            if (data == nullptr || buf.mNumberChannels == 0) {
                continue;
            }
            const uint32_t n = buf.mDataByteSize / sizeof(float) / buf.mNumberChannels;
            frames = n;
            for (UInt32 c = 0; c < buf.mNumberChannels && ch < MR_MAX_CHANNELS; ++c, ++ch) {
                if (!RingWrite(shm->captureRing[ch], data + c, n, buf.mNumberChannels)) {
                    atomic_fetch_add_explicit(&shm->captureOverruns, 1, std::memory_order_relaxed);
                }
            }
        }
    }

    // ---- playback rings -> hardware output ------------------------------------------
    // Already carries the summed buses mixed into the mirrored physical outputs, because the
    // summing happens driver-side where every Sum port for a cycle is in hand at once.
    if (outOutput != nullptr) {
        uint32_t ch = 0;
        for (UInt32 b = 0; b < outOutput->mNumberBuffers && ch < MR_MAX_CHANNELS; ++b) {
            auto& buf = outOutput->mBuffers[b];
            auto* data = static_cast<float*>(buf.mData);
            if (data == nullptr || buf.mNumberChannels == 0) {
                continue;
            }
            const uint32_t n = buf.mDataByteSize / sizeof(float) / buf.mNumberChannels;
            frames = n;
            for (UInt32 c = 0; c < buf.mNumberChannels && ch < MR_MAX_CHANNELS; ++c, ++ch) {
                if (!RingRead(shm->playbackRing[ch], data + c, n, buf.mNumberChannels)) {
                    atomic_fetch_add_explicit(&shm->playbackUnderruns, 1, std::memory_order_relaxed);
                }
            }
        }
    }

    // ---- publish the hardware's clock ------------------------------------------------
    // The reason this daemon exists at all, as far as the driver is concerned. Sample time
    // is stored before host time: the driver reads host time first and re-reads sample time
    // after, so a torn pair is detectable rather than silently wrong.
    if (inInputTime != nullptr && (inInputTime->mFlags & kAudioTimeStampSampleTimeValid)) {
        atomic_store_explicit(&shm->anchorSampleTime,
            (uint64_t) inInputTime->mSampleTime, std::memory_order_relaxed);
        atomic_store_explicit(&shm->anchorHostTime,
            (inInputTime->mFlags & kAudioTimeStampHostTimeValid)
                ? (uint64_t) inInputTime->mHostTime
                : mach_absolute_time(),
            std::memory_order_release);
    }
    if (frames != 0) {
        atomic_store_explicit(&shm->cycleFrames, frames, std::memory_order_relaxed);
    }

    return noErr;
}

// -------------------------------------------------------------------------------------

// -------------------------------------------------------------------------------------
// Recovery from a coreaudiod restart.
//
// Found 2026-08-25 the first time this ran in a VM: `killall coreaudiod` leaves the helper
// reporting RUNNING with a FROZEN anchor. The IOProc is never called again, no error is
// delivered, and nothing in the process notices. That is the exact failure helperState was
// added to prevent — a wrapper that still claims to be alive while nothing is behind it —
// and it is not an edge case: every driver install restarts coreaudiod.
//
// The watchdog is the anchor itself rather than a CoreAudio notification. There IS a
// documented signal for this one cause (kAudioHardwarePropertyServiceRestarted), but a
// frozen clock is the symptom that matters and it also covers the device being unplugged,
// the IOProc being stopped underneath us, and whatever else has the same effect. Watch the
// thing you actually care about.
// -------------------------------------------------------------------------------------

bool StartOnDevice(AudioDeviceID dev)
{
    OSStatus err = AudioDeviceCreateIOProcID(dev, RenderProc, nullptr, &gIOProc);
    if (err != noErr || gIOProc == nullptr) {
        return false;
    }
    err = AudioDeviceStart(dev, gIOProc);
    if (err != noErr) {
        AudioDeviceDestroyIOProcID(dev, gIOProc);
        gIOProc = nullptr;
        return false;
    }
    gDevice = dev;
    return true;
}

void StopCurrent()
{
    if (gIOProc != nullptr && gDevice != kAudioObjectUnknown) {
        AudioDeviceStop(gDevice, gIOProc);
        AudioDeviceDestroyIOProcID(gDevice, gIOProc);
    }
    gIOProc = nullptr;
}

// Re-resolve by UID and restart. Ring pointers are deliberately left alone: they are
// monotonic counters, so a reader that survived the outage simply continues, and resetting
// them would hand a live driver a discontinuity it has no way to interpret.
bool Recover()
{
    std::printf("  clock frozen — coreaudiod restart or device loss; recovering\n");
    std::fflush(stdout);

    atomic_store_explicit(&gShm->helperState, (uint32_t) MR_HELPER_STARTING,
        std::memory_order_release);
    StopCurrent();

    for (auto d : AllDevices()) {
        if (DeviceUID(d) == gWrappedUID) {
            if (StartOnDevice(d)) {
                atomic_store_explicit(&gShm->helperState, (uint32_t) MR_HELPER_RUNNING,
                    std::memory_order_release);
                std::printf("  recovered on device id %u\n", (unsigned) d);
                std::fflush(stdout);
                return true;
            }
            break;
        }
    }

    atomic_store_explicit(&gShm->helperState, (uint32_t) MR_HELPER_FAILED,
        std::memory_order_release);
    std::printf("  could not reopen '%s' — state FAILED\n", gWrappedUID.c_str());
    std::fflush(stdout);
    return false;
}

void OnSignal(int)
{
    gRunning.store(false);
}

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s --list\n"
        "       %s --device <uid-or-name>\n"
        "       %s --dump\n"
        "\n"
        "Wraps a physical CoreAudio device and exchanges audio with the MixerReturn\n"
        "driver through %s.  --dump inspects that region from outside.\n",
        argv0, argv0, argv0, MR_SHM_NAME);
}

const char* StateName(uint32_t s)
{
    switch (s) {
        case MR_HELPER_ABSENT:   return "ABSENT";
        case MR_HELPER_STARTING: return "STARTING";
        case MR_HELPER_RUNNING:  return "RUNNING";
        case MR_HELPER_FAILED:   return "FAILED";
        default:                 return "?";
    }
}

/*  Attach read-only and report. This is how you find out whether the helper is actually
 *  driving the region without having to instrument either side, and it is the same view the
 *  driver gets — so a disagreement between what this prints and what the driver believes is
 *  a real bug rather than a reporting artefact.
 *
 *  Samples twice a second apart: the counters that matter are the ones that MOVE. A
 *  populated-looking region with a frozen anchor is a helper that opened the hardware and
 *  then stopped being called, which reads identically to a healthy one in a single sample. */
int Dump()
{
    const int fd = shm_open(MR_SHM_NAME, O_RDONLY, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "no region at %s (%s) — is the helper running?\n",
            MR_SHM_NAME, strerror(errno));
        return 1;
    }
    void* p = mmap(nullptr, sizeof(MRShared), PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    const auto* shm = static_cast<const MRShared*>(p);

    if (shm->version != MR_SHM_VERSION) {
        std::fprintf(stderr, "version %u, expected %d — do not trust anything below\n",
            shm->version, MR_SHM_VERSION);
    }

    const uint32_t state = atomic_load_explicit(&shm->helperState, std::memory_order_acquire);
    const uint32_t nIn   = atomic_load_explicit(&shm->numInputs,   std::memory_order_relaxed);
    const uint32_t nOut  = atomic_load_explicit(&shm->numOutputs,  std::memory_order_relaxed);

    std::printf("region  : %s (version %u)\n", MR_SHM_NAME, shm->version);
    std::printf("helper  : %s\n", StateName(state));
    std::printf("io      : %u in / %u out, %u sums @ %.0f Hz, cycle %u frames\n",
        nIn, nOut, atomic_load_explicit(&shm->numSums, std::memory_order_relaxed),
        atomic_load_explicit(&shm->sampleRate, std::memory_order_relaxed),
        atomic_load_explicit(&shm->cycleFrames, std::memory_order_relaxed));

    const uint64_t host0   = atomic_load_explicit(&shm->anchorHostTime,   std::memory_order_acquire);
    const uint64_t sample0 = atomic_load_explicit(&shm->anchorSampleTime, std::memory_order_relaxed);
    const uint64_t cw0     = (nIn  > 0) ? atomic_load_explicit(&shm->captureRing[0].write,  std::memory_order_relaxed) : 0;
    const uint64_t pr0     = (nOut > 0) ? atomic_load_explicit(&shm->playbackRing[0].read,  std::memory_order_relaxed) : 0;

    sleep(1);

    const uint64_t host1   = atomic_load_explicit(&shm->anchorHostTime,   std::memory_order_acquire);
    const uint64_t sample1 = atomic_load_explicit(&shm->anchorSampleTime, std::memory_order_relaxed);
    const uint64_t cw1     = (nIn  > 0) ? atomic_load_explicit(&shm->captureRing[0].write,  std::memory_order_relaxed) : 0;
    const uint64_t pr1     = (nOut > 0) ? atomic_load_explicit(&shm->playbackRing[0].read,  std::memory_order_relaxed) : 0;

    std::printf("anchor  : host %llu -> %llu   sample %llu -> %llu  (+%llu frames in 1s)\n",
        (unsigned long long) host0, (unsigned long long) host1,
        (unsigned long long) sample0, (unsigned long long) sample1,
        (unsigned long long) (sample1 - sample0));
    std::printf("ring[0] : capture.write %llu -> %llu   playback.read %llu -> %llu\n",
        (unsigned long long) cw0, (unsigned long long) cw1,
        (unsigned long long) pr0, (unsigned long long) pr1);
    std::printf("errors  : overruns=%llu underruns=%llu\n",
        (unsigned long long) atomic_load_explicit(&shm->captureOverruns,   std::memory_order_relaxed),
        (unsigned long long) atomic_load_explicit(&shm->playbackUnderruns, std::memory_order_relaxed));

    // The verdict, so this does not need interpreting at 2am.
    const bool moving = (sample1 != sample0);
    if (state == MR_HELPER_RUNNING && moving) {
        std::printf("VERDICT : LIVE — the hardware clock is advancing\n");
    } else if (state == MR_HELPER_RUNNING) {
        std::printf("VERDICT : STALLED — helper says RUNNING but the anchor is frozen\n");
    } else {
        std::printf("VERDICT : helper is %s\n", StateName(state));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string want;
    bool list = false;
    bool dump = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a == "--dump") {
            dump = true;
        } else if (a == "--device" && i + 1 < argc) {
            want = argv[++i];
        } else {
            Usage(argv[0]);
            return 2;
        }
    }

    if (list) {
        std::printf("%-44s %4s %5s %9s  %s\n", "NAME", "IN", "OUT", "RATE", "UID");
        std::printf("%s\n", std::string(110, '-').c_str());
        for (auto d : AllDevices()) {
            std::printf("%-44s %4u %5u %9.0f  %s\n",
                DeviceName(d).c_str(), ChannelCount(d, kAudioObjectPropertyScopeInput),
                ChannelCount(d, kAudioObjectPropertyScopeOutput),
                NominalSampleRate(d), DeviceUID(d).c_str());
        }
        return 0;
    }

    if (dump) {
        return Dump();
    }

    if (want.empty()) {
        Usage(argv[0]);
        return 2;
    }

    // ---- find the device ---------------------------------------------------------------
    for (auto d : AllDevices()) {
        if (DeviceUID(d) == want || DeviceName(d) == want) {
            gDevice = d;
            break;
        }
    }
    if (gDevice == kAudioObjectUnknown) {
        std::fprintf(stderr, "no device matching '%s' — try --list\n", want.c_str());
        return 1;
    }

    const std::string uid = DeviceUID(gDevice);
    if (IsOurOwnDevice(uid)) {
        std::fprintf(stderr,
            "refusing to wrap MixerReturn itself — that is a feedback loop, not a wrapper\n");
        return 1;
    }

    gNumInputs  = ChannelCount(gDevice, kAudioObjectPropertyScopeInput);
    gNumOutputs = ChannelCount(gDevice, kAudioObjectPropertyScopeOutput);
    const double rate = NominalSampleRate(gDevice);

    if (gNumInputs > MR_MAX_CHANNELS || gNumOutputs > MR_MAX_CHANNELS) {
        std::fprintf(stderr, "device has %u in / %u out, over the %d ceiling in mr_shared.h\n",
            gNumInputs, gNumOutputs, MR_MAX_CHANNELS);
        return 1;
    }

    std::printf("wrapping: %s\n  uid  : %s\n  io   : %u in / %u out @ %.0f Hz\n",
        DeviceName(gDevice).c_str(), uid.c_str(), gNumInputs, gNumOutputs, rate);

    // ---- shared region -------------------------------------------------------------------
    // The helper creates it. It is the side that knows the hardware's shape, and the driver
    // must be able to come and go — coreaudiod restarts far more often than this daemon.
    const int fd = shm_open(MR_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        std::perror("shm_open");
        return 1;
    }
    if (ftruncate(fd, sizeof(MRShared)) != 0 && errno != EINVAL) {
        std::perror("ftruncate");
        return 1;
    }
    // shm_open honours umask, so the mode above is not necessarily what landed — and the
    // driver runs as _coreaudiod, not as whoever started this.
    fchmod(fd, 0666);

    void* mapped = mmap(nullptr, sizeof(MRShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    gShm = static_cast<MRShared*>(mapped);

    // Zero before publishing anything. A stale region from a previous run has stale ring
    // pointers, and a driver that attaches to those reads audio that is not there.
    std::memset(gShm, 0, sizeof(MRShared));
    gShm->version = MR_SHM_VERSION;
    atomic_store_explicit(&gShm->helperState, (uint32_t) MR_HELPER_STARTING, std::memory_order_release);

    atomic_store_explicit(&gShm->numInputs,  gNumInputs,  std::memory_order_relaxed);
    atomic_store_explicit(&gShm->numOutputs, gNumOutputs, std::memory_order_relaxed);
    atomic_store_explicit(&gShm->numSums,    gNumInputs,  std::memory_order_relaxed);
    atomic_store_explicit(&gShm->sampleRate, rate,        std::memory_order_relaxed);

    // ---- run --------------------------------------------------------------------------
    gWrappedUID = uid;
    if (!StartOnDevice(gDevice)) {
        std::fprintf(stderr, "could not start an IOProc on '%s'\n", uid.c_str());
        atomic_store_explicit(&gShm->helperState, (uint32_t) MR_HELPER_FAILED, std::memory_order_release);
        return 1;
    }

    atomic_store_explicit(&gShm->helperState, (uint32_t) MR_HELPER_RUNNING, std::memory_order_release);
    std::printf("running — ^C to stop\n");
    std::fflush(stdout);

    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    // Report from the control thread, not the IOProc. The counters are the only evidence a
    // dropout leaves, and a dropout nobody can point at after a show is one nobody can argue
    // about.
    uint64_t lastOver = 0, lastUnder = 0;
    uint64_t lastAnchor = 0;
    int      stalledTicks = 0;
    int      failedRecoveries = 0;

    while (gRunning.load()) {
        sleep(1);

        // ---- watchdog ------------------------------------------------------------------
        // Two ticks before acting: one is normal at very large buffer sizes, where a cycle
        // can straddle a sample. Two seconds of a frozen clock is not.
        const uint64_t anchor = atomic_load_explicit(&gShm->anchorSampleTime,
            std::memory_order_acquire);
        const uint32_t state = atomic_load_explicit(&gShm->helperState,
            std::memory_order_relaxed);

        if (state == MR_HELPER_RUNNING && anchor == lastAnchor) {
            if (++stalledTicks >= 2) {
                stalledTicks = 0;
                if (!Recover() && ++failedRecoveries >= 10) {
                    std::fprintf(stderr, "giving up after %d failed recoveries\n",
                        failedRecoveries);
                    break;
                }
            }
        } else {
            stalledTicks = 0;
            if (state == MR_HELPER_RUNNING) {
                failedRecoveries = 0;
            }
        }
        // A FAILED helper keeps trying: the device may come back, and the alternative is a
        // daemon that has to be restarted by hand after every coreaudiod bounce.
        if (state == MR_HELPER_FAILED) {
            Recover();
        }
        lastAnchor = anchor;

        // ---- counters ------------------------------------------------------------------
        const uint64_t over  = atomic_load_explicit(&gShm->captureOverruns,   std::memory_order_relaxed);
        const uint64_t under = atomic_load_explicit(&gShm->playbackUnderruns, std::memory_order_relaxed);
        if (over != lastOver || under != lastUnder) {
            std::printf("  overruns=%llu underruns=%llu  (frames=%u)\n",
                (unsigned long long) over, (unsigned long long) under,
                atomic_load_explicit(&gShm->cycleFrames, std::memory_order_relaxed));
            std::fflush(stdout);
            lastOver = over;
            lastUnder = under;
        }
    }

    std::printf("\nstopping\n");
    StopCurrent();

    // Tell the driver we are gone before the memory goes away. A virtual device that keeps
    // accepting IO with nothing behind it silently eats a show's audio.
    atomic_store_explicit(&gShm->helperState, (uint32_t) MR_HELPER_ABSENT, std::memory_order_release);
    munmap(mapped, sizeof(MRShared));
    close(fd);
    return 0;
}
