/*  MixerReturn — the AudioServerPlugIn.
 *
 *  Presents one virtual device: the wrapped interface's inputs and outputs mirrored
 *  straight through, plus a Sum port for every input. Sum ports are outputs as far as the
 *  host is concerned — a rack patches its output to one, and this driver collapses them
 *  into stereo buses before they reach the hardware.
 *
 *  Read `../README.md` first; the port model is the whole point and it is explained there
 *  rather than repeated here.
 *
 *  ---------------------------------------------------------------------------------
 *  Two rules this file lives under
 *  ---------------------------------------------------------------------------------
 *
 *  1. Everything from BeginIOOperation to EndIOOperation runs on a realtime thread inside
 *     coreaudiod. No allocation, no locks, no logging, no syscalls. A stall here is not a
 *     glitch in one app, it is a glitch in every audio client on the machine.
 *
 *  2. The device's timeline belongs to the wrapped hardware, not to this driver. The
 *     helper publishes the hardware's anchor every cycle and GetZeroTimeStamp reports it.
 *     A free-running virtual clock would drift against the interface being wrapped, and
 *     the drift shows up as a slow slide rather than an obvious failure.
 */

#include <CoreAudio/AudioServerPlugIn.h>
/*  AudioServerPlugIn.h does not pull in the selectors a driver still has to answer —
 *  kAudioDevicePropertyStreamConfiguration among them — so the client-side header comes
 *  along for the constants. None of its functions are called from in here. */
#include <CoreAudio/AudioHardware.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "mr_shared.h"

#pragma mark - Object model

/*  Fixed IDs. The HAL only ever asks about objects this driver told it exist, so a static
 *  set is simpler and cheaper than a table, and the device layout is fixed once the helper
 *  has reported what it wrapped. */
enum {
    kObjectID_PlugIn        = kAudioObjectPlugInObject,
    kObjectID_Device        = 2,
    kObjectID_Stream_Input  = 3,
    kObjectID_Stream_Output = 4,
};

#define kDeviceUID          "com.allansargeant.mixerreturn.device"
#define kDeviceModelUID     "com.allansargeant.mixerreturn.model"
#define kBoxUID             "com.allansargeant.mixerreturn.box"

static pthread_mutex_t  gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32           gRefCount   = 0;
static AudioServerPlugInHostRef gHost = NULL;

/*  The shared region. Mapped once at Initialize; the helper may not exist yet, in which
 *  case gShm stays NULL, the device falls back to the layout in RefreshFromHelper and its
 *  IO path produces silence. */
static MRShared*        gShm    = NULL;
static int              gShmFd  = -1;

static Float64          gSampleRate = 48000.0;
static UInt32           gNumInputs  = 0;
static UInt32           gNumOutputs = 0;
static UInt32           gNumSums    = 0;

/*  IO cycle bookkeeping. Advanced by GetZeroTimeStamp, which the HAL calls once per cycle
 *  to ask where the device's timeline has got to. */
static UInt64           gAnchorHostTime   = 0;
static UInt64           gAnchorSampleTime = 0;

/*  Host-visible output channel count: the mirrored physical outputs come first so a rack
 *  patched to "output 3" means the same thing it always did, and the Sum ports follow. */
static inline UInt32 TotalOutputChannels(void)  { return gNumOutputs + gNumSums; }
static inline UInt32 TotalInputChannels(void)   { return gNumInputs; }

#pragma mark - Shared memory

/*  Pulls the helper's published description into the driver's own view. Called from the
 *  message thread only — the audio path reads the cached copies above, so that a helper
 *  restarting mid-cycle cannot change the channel count underneath a running IO operation.
 */
static void RefreshFromHelper(void)
{
    /*  The fallback layout, used until the helper reports what it actually wrapped.
     *
     *  This started out as "report zero channels when no helper is running", on the
     *  reasoning that a wrapper with nothing behind it should not look like a working
     *  interface. That was wrong in a way that cost an install cycle to find: a device
     *  with no streams is one the HAL does not publish at all, so the driver loaded
     *  correctly and simply never appeared, which looks identical to not loading.
     *
     *  A virtual device exists whether or not hardware is behind it. Honesty about the
     *  helper belongs in the control surface and in what the IO path produces — silence,
     *  below — not in pretending the device is absent. */
    if (gShm == NULL || gShm->version != MR_SHM_VERSION
        || atomic_load(&gShm->helperState) != MR_HELPER_RUNNING) {
        gNumInputs  = 8;
        gNumOutputs = 8;
        gNumSums    = 8;
        gSampleRate = 48000.0;
        return;
    }

    gNumInputs  = atomic_load(&gShm->numInputs);
    gNumOutputs = atomic_load(&gShm->numOutputs);
    gNumSums    = atomic_load(&gShm->numSums);
    gSampleRate = atomic_load(&gShm->sampleRate);

    if (gNumInputs  > MR_MAX_CHANNELS) gNumInputs  = MR_MAX_CHANNELS;
    if (gNumOutputs > MR_MAX_CHANNELS) gNumOutputs = MR_MAX_CHANNELS;
    if (gNumSums    > MR_MAX_SUMS)     gNumSums    = MR_MAX_SUMS;
}

static void MapShared(void)
{
    if (gShm != NULL) return;

    gShmFd = shm_open(MR_SHM_NAME, O_RDWR, 0666);
    if (gShmFd < 0) return;                     /* helper not running yet; not an error   */

    void* p = mmap(NULL, sizeof(MRShared), PROT_READ | PROT_WRITE, MAP_SHARED, gShmFd, 0);
    if (p == MAP_FAILED) { close(gShmFd); gShmFd = -1; return; }

    gShm = (MRShared*) p;
    RefreshFromHelper();
}

#pragma mark - Ring helpers

/*  Both of these are deliberately lossy. A ring that blocks when it is full or empty would
 *  stall a realtime thread, so a full ring drops and an empty one reads silence, and both
 *  bump a counter the control surface can show. Dropping audibly is recoverable; blocking
 *  coreaudiod is not. */

static inline void RingWrite(MRRing* ring, const float* src, UInt32 frames, UInt64* dropped)
{
    const uint64_t w = atomic_load_explicit(&ring->write, memory_order_relaxed);
    const uint64_t r = atomic_load_explicit(&ring->read,  memory_order_acquire);

    if (w - r + frames > MR_RING_FRAMES) { if (dropped) (*dropped)++; return; }

    for (UInt32 i = 0; i < frames; ++i)
        ring->data[(w + i) & MR_RING_MASK] = src[i];

    atomic_store_explicit(&ring->write, w + frames, memory_order_release);
}

static inline void RingRead(MRRing* ring, float* dst, UInt32 frames, UInt64* starved)
{
    const uint64_t r = atomic_load_explicit(&ring->read,  memory_order_relaxed);
    const uint64_t w = atomic_load_explicit(&ring->write, memory_order_acquire);

    if (w - r < frames) {
        memset(dst, 0, frames * sizeof(float));
        if (starved) (*starved)++;
        return;
    }

    for (UInt32 i = 0; i < frames; ++i)
        dst[i] = ring->data[(r + i) & MR_RING_MASK];

    atomic_store_explicit(&ring->read, r + frames, memory_order_release);
}

#pragma mark - Driver interface

static HRESULT     MR_QueryInterface(void* drv, REFIID iid, LPVOID* out);
static ULONG       MR_AddRef(void* drv);
static ULONG       MR_Release(void* drv);
static OSStatus    MR_Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host);
static OSStatus    MR_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus    MR_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus    MR_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus    MR_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus    MR_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus    MR_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean     MR_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus    MR_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus    MR_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus    MR_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus    MR_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static OSStatus    MR_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus    MR_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus    MR_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
static OSStatus    MR_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus    MR_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus    MR_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus    MR_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    MR_QueryInterface,
    MR_AddRef,
    MR_Release,
    MR_Initialize,
    MR_CreateDevice,
    MR_DestroyDevice,
    MR_AddDeviceClient,
    MR_RemoveDeviceClient,
    MR_PerformDeviceConfigurationChange,
    MR_AbortDeviceConfigurationChange,
    MR_HasProperty,
    MR_IsPropertySettable,
    MR_GetPropertyDataSize,
    MR_GetPropertyData,
    MR_SetPropertyData,
    MR_StartIO,
    MR_StopIO,
    MR_GetZeroTimeStamp,
    MR_WillDoIOOperation,
    MR_BeginIOOperation,
    MR_DoIOOperation,
    MR_EndIOOperation,
};

static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef        gDriverRef    = &gInterfacePtr;

#pragma mark - COM plumbing

static HRESULT MR_QueryInterface(void* drv, REFIID iid, LPVOID* out)
{
    if (drv != gDriverRef || out == NULL) return kAudioHardwareBadObjectError;

    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    const Boolean wanted =
        CFEqual(req, IUnknownUUID) ||
        CFEqual(req, CFUUIDGetConstantUUIDWithBytes(NULL,
            0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, 0x49, 0xF1,
            0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17));   /* kAudioServerPlugInDriverInterfaceUUID */
    CFRelease(req);

    if (!wanted) return E_NOINTERFACE;

    pthread_mutex_lock(&gStateMutex);
    ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);

    *out = gDriverRef;
    return S_OK;
}

static ULONG MR_AddRef(void* drv)
{
    if (drv != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    const ULONG n = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return n;
}

static ULONG MR_Release(void* drv)
{
    if (drv != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    const ULONG n = (gRefCount > 0) ? --gRefCount : 0;
    pthread_mutex_unlock(&gStateMutex);
    return n;
}

#pragma mark - Lifecycle

static OSStatus MR_Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host)
{
    if (drv != gDriverRef) return kAudioHardwareBadObjectError;

    gHost = host;
    MapShared();
    return 0;
}

/*  This driver publishes one fixed device rather than letting the host create them, so the
 *  creation calls are refused outright. */
static OSStatus MR_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                                const AudioServerPlugInClientInfo* client, AudioObjectID* out)
{ (void)d; (void)desc; (void)client; (void)out; return kAudioHardwareUnsupportedOperationError; }

static OSStatus MR_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{ (void)d; (void)id; return kAudioHardwareUnsupportedOperationError; }

static OSStatus MR_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                   const AudioServerPlugInClientInfo* c)
{ (void)d; (void)id; (void)c; return 0; }

static OSStatus MR_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                      const AudioServerPlugInClientInfo* c)
{ (void)d; (void)id; (void)c; return 0; }

static OSStatus MR_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                    UInt64 action, void* info)
{ (void)d; (void)id; (void)action; (void)info; return 0; }

static OSStatus MR_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                  UInt64 action, void* info)
{ (void)d; (void)id; (void)action; (void)info; return 0; }

#pragma mark - IO

static OSStatus MR_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void)d; (void)client;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    RefreshFromHelper();
    pthread_mutex_unlock(&gStateMutex);

    /*  Starts regardless of the helper. Refusing here was the same mistake as reporting
     *  zero channels: a device that cannot be started cannot be selected, inspected or
     *  debugged, and "not selectable" is indistinguishable from "not installed". With no
     *  helper the IO path simply produces silence. */

    gAnchorHostTime   = mach_absolute_time();
    gAnchorSampleTime = 0;
    return 0;
}

static OSStatus MR_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{ (void)d; (void)client; return (id == kObjectID_Device) ? 0 : kAudioHardwareBadObjectError; }

/*  Where the device's timeline has got to. Anchored to the hardware, per rule 2 at the top:
 *  the helper republishes these every hardware cycle and this just relays them. */
static OSStatus MR_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                    Float64* sampleTime, UInt64* hostTime, UInt64* seed)
{
    (void)d; (void)client;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (gShm == NULL) return kAudioHardwareNotRunningError;

    const uint64_t hwHost   = atomic_load(&gShm->anchorHostTime);
    const uint64_t hwSample = atomic_load(&gShm->anchorSampleTime);

    if (hwHost != 0) { gAnchorHostTime = hwHost; gAnchorSampleTime = hwSample; }

    if (sampleTime) *sampleTime = (Float64) gAnchorSampleTime;
    if (hostTime)   *hostTime   = gAnchorHostTime;
    if (seed)       *seed       = 1;
    return 0;
}

static OSStatus MR_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                     UInt32 operation, Boolean* willDo, Boolean* willDoInPlace)
{
    (void)d; (void)client;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;

    Boolean doIt = false;
    switch (operation) {
        case kAudioServerPlugInIOOperationReadInput:
        case kAudioServerPlugInIOOperationWriteMix:
            doIt = true;
            break;
        default:
            break;
    }

    if (willDo)        *willDo = doIt;
    if (willDoInPlace) *willDoInPlace = true;
    return 0;
}

static OSStatus MR_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                    UInt32 operation, UInt32 frames,
                                    const AudioServerPlugInIOCycleInfo* cycle)
{ (void)d; (void)id; (void)client; (void)operation; (void)frames; (void)cycle; return 0; }

static OSStatus MR_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                  UInt32 operation, UInt32 frames,
                                  const AudioServerPlugInIOCycleInfo* cycle)
{ (void)d; (void)id; (void)client; (void)operation; (void)frames; (void)cycle; return 0; }

/*  The audio path.
 *
 *  ReadInput hands the host the wrapped interface's inputs. WriteMix takes everything the
 *  host produced for this cycle — the mirrored physical outputs and every Sum port — and
 *  this is the moment the whole design turns on: they all arrive together, so the summing
 *  costs no delay. The plugin's two-page barrier exists precisely because plugin instances
 *  never get this moment.
 */
static OSStatus MR_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                                 AudioObjectID stream, UInt32 client, UInt32 operation,
                                 UInt32 frames, const AudioServerPlugInIOCycleInfo* cycle,
                                 void* mainBuffer, void* secondaryBuffer)
{
    (void)d; (void)client; (void)cycle; (void)secondaryBuffer;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (mainBuffer == NULL) return 0;

    /*  No helper: hand back silence rather than whatever was in the buffer. An
     *  uninitialised input buffer is not silence, it is the previous cycle's audio. */
    if (gShm == NULL) {
        if (operation == kAudioServerPlugInIOOperationReadInput)
            memset(mainBuffer, 0, (size_t) frames * TotalInputChannels() * sizeof(float));
        return 0;
    }

    float* buf = (float*) mainBuffer;

    if (operation == kAudioServerPlugInIOOperationReadInput) {
        const UInt32 n = TotalInputChannels();
        for (UInt32 ch = 0; ch < n; ++ch) {
            float scratch[MR_RING_FRAMES];
            const UInt32 want = (frames <= MR_RING_FRAMES) ? frames : MR_RING_FRAMES;
            RingRead(&gShm->captureRing[ch], scratch, want, NULL);
            for (UInt32 f = 0; f < want; ++f) buf[f * n + ch] = scratch[f];   /* interleaved */
        }
        return 0;
    }

    if (operation == kAudioServerPlugInIOOperationWriteMix) {
        (void)stream;
        const UInt32 nOut  = gNumOutputs;
        const UInt32 nSum  = gNumSums;
        const UInt32 total = nOut + nSum;
        const UInt32 want  = (frames <= MR_RING_FRAMES) ? frames : MR_RING_FRAMES;

        /*  Start from the mirrored physical outputs: whatever the host wrote for a
         *  passed-through output goes out unchanged, which is what makes a rack patched to
         *  a physical output behave as an ordinary insert. */
        float mix[MR_MAX_CHANNELS][MR_RING_FRAMES];
        for (UInt32 ch = 0; ch < nOut; ++ch)
            for (UInt32 f = 0; f < want; ++f)
                mix[ch][f] = buf[f * total + ch];

        /*  Then fold every Sum port into the buses it is assigned to, and each bus onto the
         *  physical pair it lands on. */
        for (UInt32 s = 0; s < nSum; ++s) {
            const uint32_t mask = atomic_load_explicit(&gShm->sums[s].busMask, memory_order_relaxed);
            if (mask == 0) continue;
            const float g = atomic_load_explicit(&gShm->sums[s].gain, memory_order_relaxed);

            for (UInt32 b = 0; b < MR_MAX_BUSES; ++b) {
                if ((mask & (1u << b)) == 0) continue;

                const int32_t l  = atomic_load_explicit(&gShm->buses[b].outLeft,  memory_order_relaxed);
                const int32_t r  = atomic_load_explicit(&gShm->buses[b].outRight, memory_order_relaxed);
                const float   bg = atomic_load_explicit(&gShm->buses[b].gain,     memory_order_relaxed);
                const float   k  = g * bg;

                for (UInt32 f = 0; f < want; ++f) {
                    const float v = buf[f * total + nOut + s] * k;
                    if (l >= 0 && (UInt32) l < nOut) mix[l][f] += v;
                    if (r >= 0 && (UInt32) r < nOut) mix[r][f] += v;
                }
            }
        }

        for (UInt32 ch = 0; ch < nOut; ++ch)
            RingWrite(&gShm->playbackRing[ch], mix[ch], want, NULL);

        return 0;
    }

    return 0;
}

#pragma mark - Properties

/*  The HAL discovers everything about this driver by asking for properties one at a time,
 *  so this section is most of the file. Only the properties the HAL actually requires to
 *  publish a usable device are answered; anything else falls through to
 *  kAudioHardwareUnknownPropertyError, which is a legal answer and the HAL copes.
 */

static Boolean MR_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                              const AudioObjectPropertyAddress* addr)
{
    (void)d; (void)pid;
    if (addr == NULL) return false;

    switch (id) {
        case kObjectID_PlugIn:
            switch (addr->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    return true;
            }
            return false;

        case kObjectID_Device:
            switch (addr->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioDevicePropertyStreamConfiguration:
                    return true;
            }
            return false;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            switch (addr->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
            }
            return false;
    }
    return false;
}

static OSStatus MR_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                      const AudioObjectPropertyAddress* addr, Boolean* settable)
{
    (void)d; (void)pid;
    if (addr == NULL || settable == NULL) return kAudioHardwareIllegalOperationError;

    /*  Nothing here is settable yet. The sample rate follows the wrapped hardware, so
     *  letting a host set it would be a lie until the helper can retune the interface. */
    (void)id;
    *settable = false;
    return 0;
}

static OSStatus MR_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                       const AudioObjectPropertyAddress* addr,
                                       UInt32 qualDataSize, const void* qualData, UInt32* outSize)
{
    (void)d; (void)pid; (void)qualDataSize; (void)qualData;
    if (addr == NULL || outSize == NULL) return kAudioHardwareIllegalOperationError;

    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:            *outSize = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner:            *outSize = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:         *outSize = sizeof(CFStringRef); return 0;
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: *outSize = sizeof(UInt32); return 0;
        case kAudioDevicePropertyNominalSampleRate: *outSize = sizeof(Float64); return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outSize = sizeof(AudioValueRange); return 0;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outSize = sizeof(AudioStreamBasicDescription); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outSize = sizeof(AudioStreamRangedDescription); return 0;

        case kAudioObjectPropertyOwnedObjects:
            *outSize = (id == kObjectID_Device) ? (2 * sizeof(AudioObjectID))
                     : (id == kObjectID_PlugIn) ? (1 * sizeof(AudioObjectID))
                     : 0;
            return 0;
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outSize = sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyStreams: {
            const UInt32 n = (addr->mScope == kAudioObjectPropertyScopeGlobal) ? 2 : 1;
            *outSize = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioDevicePropertyStreamConfiguration: {
            /*  One buffer describing every channel in that direction. The Sum ports sit in
             *  the output list after the mirrored physical outputs, which is what makes
             *  "output 3" still mean output 3 to anyone already patched. */
            *outSize = (UInt32) (sizeof(AudioBufferList) - sizeof(AudioBuffer) + sizeof(AudioBuffer));
            return 0;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

/*  The device's stream format. Float32 interleaved, following the wrapped hardware's rate.
 *  `channels` differs between the input stream and the output stream, which is where the
 *  Sum ports come from. */
static void FillFormat(AudioStreamBasicDescription* f, UInt32 channels)
{
    memset(f, 0, sizeof(*f));
    f->mSampleRate       = gSampleRate;
    f->mFormatID         = kAudioFormatLinearPCM;
    f->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
                         | kAudioFormatFlagIsPacked;
    f->mBytesPerPacket   = channels * sizeof(Float32);
    f->mFramesPerPacket  = 1;
    f->mBytesPerFrame    = channels * sizeof(Float32);
    f->mChannelsPerFrame = channels;
    f->mBitsPerChannel   = 32;
}

static OSStatus MR_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                   const AudioObjectPropertyAddress* addr,
                                   UInt32 qualDataSize, const void* qualData,
                                   UInt32 dataSize, UInt32* outSize, void* outData)
{
    (void)d; (void)pid; (void)qualDataSize; (void)qualData;
    if (addr == NULL || outSize == NULL || outData == NULL)
        return kAudioHardwareIllegalOperationError;

    const Boolean isInputStream = (id == kObjectID_Stream_Input);

    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (dataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *(AudioClassID*)outData = (id == kObjectID_PlugIn) ? kAudioObjectClassID
                                    : (id == kObjectID_Device) ? kAudioObjectClassID
                                    : kAudioObjectClassID;
            *outSize = sizeof(AudioClassID); return 0;

        case kAudioObjectPropertyClass:
            if (dataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *(AudioClassID*)outData = (id == kObjectID_PlugIn) ? kAudioPlugInClassID
                                    : (id == kObjectID_Device) ? kAudioDeviceClassID
                                    : kAudioStreamClassID;
            *outSize = sizeof(AudioClassID); return 0;

        case kAudioObjectPropertyOwner:
            if (dataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *(AudioObjectID*)outData = (id == kObjectID_PlugIn) ? kAudioObjectUnknown
                                                               : kObjectID_Device;
            *outSize = sizeof(AudioObjectID); return 0;

        case kAudioObjectPropertyManufacturer:
            if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *(CFStringRef*)outData = CFSTR("Stoatworks Labs");
            *outSize = sizeof(CFStringRef); return 0;

        case kAudioObjectPropertyName:
            if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *(CFStringRef*)outData = CFSTR("MixerReturn");
            *outSize = sizeof(CFStringRef); return 0;

        case kAudioDevicePropertyDeviceUID:
            if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *(CFStringRef*)outData = CFSTR(kDeviceUID);
            *outSize = sizeof(CFStringRef); return 0;

        case kAudioDevicePropertyModelUID:
            if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *(CFStringRef*)outData = CFSTR(kDeviceModelUID);
            *outSize = sizeof(CFStringRef); return 0;

        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
            if (dataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *(AudioObjectID*)outData = kObjectID_Device;
            *outSize = sizeof(AudioObjectID); return 0;

        case kAudioObjectPropertyOwnedObjects: {
            const UInt32 room = dataSize / sizeof(AudioObjectID);
            AudioObjectID* list = (AudioObjectID*) outData;
            UInt32 n = 0;

            /*  The plug-in owns the device. Returning nothing here is not a harmless
             *  omission: the HAL walks a plug-in's owned objects to find its devices, so a
             *  plug-in that owns nothing publishes nothing, and the driver loads perfectly
             *  while never appearing in the device list. */
            if (id == kObjectID_PlugIn) {
                if (room > n) list[n++] = kObjectID_Device;
            } else if (id == kObjectID_Device) {
                if (room > n) list[n++] = kObjectID_Stream_Input;
                if (room > n) list[n++] = kObjectID_Stream_Output;
            }
            *outSize = n * sizeof(AudioObjectID); return 0;
        }

        case kAudioDevicePropertyStreams: {
            const UInt32 room = dataSize / sizeof(AudioObjectID);
            AudioObjectID* list = (AudioObjectID*) outData;
            UInt32 n = 0;
            if (addr->mScope != kAudioObjectPropertyScopeOutput && room > n)
                list[n++] = kObjectID_Stream_Input;
            if (addr->mScope != kAudioObjectPropertyScopeInput && room > n)
                list[n++] = kObjectID_Stream_Output;
            *outSize = n * sizeof(AudioObjectID); return 0;
        }

        case kAudioDevicePropertyTransportType:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = kAudioDeviceTransportTypeVirtual;
            *outSize = sizeof(UInt32); return 0;

        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyIsHidden:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = 0;
            *outSize = sizeof(UInt32); return 0;

        case kAudioDevicePropertyDeviceIsAlive:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            /*  Always alive. IsAlive answers "does this device object exist", not "is
             *  hardware attached" -- a device that reports itself dead is one the HAL
             *  hides, which is not the message intended. */
            *(UInt32*)outData = 1;
            *outSize = sizeof(UInt32); return 0;

        case kAudioDevicePropertyDeviceIsRunning:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = (gAnchorHostTime != 0) ? 1 : 0;
            *outSize = sizeof(UInt32); return 0;

        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            /*  Not a default-device candidate. Somebody's system alerts landing in a
             *  summing bus mid-show is not a failure mode worth allowing. */
            *(UInt32*)outData = 0;
            *outSize = sizeof(UInt32); return 0;

        case kAudioDevicePropertySafetyOffset:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = 0;
            *outSize = sizeof(UInt32); return 0;

        case kAudioDevicePropertyZeroTimeStampPeriod:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = (gShm != NULL) ? atomic_load(&gShm->cycleFrames) : 512;
            *outSize = sizeof(UInt32); return 0;

        case kAudioDevicePropertyNominalSampleRate:
            if (dataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            *(Float64*)outData = gSampleRate;
            *outSize = sizeof(Float64); return 0;

        case kAudioDevicePropertyAvailableNominalSampleRates: {
            if (dataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
            AudioValueRange* r = (AudioValueRange*) outData;
            r->mMinimum = gSampleRate;      /* follows the hardware, so exactly one rate   */
            r->mMaximum = gSampleRate;
            *outSize = sizeof(AudioValueRange); return 0;
        }

        case kAudioDevicePropertyStreamConfiguration: {
            AudioBufferList* bl = (AudioBufferList*) outData;
            const UInt32 channels = (addr->mScope == kAudioObjectPropertyScopeInput)
                                  ? TotalInputChannels() : TotalOutputChannels();
            bl->mNumberBuffers = 1;
            bl->mBuffers[0].mNumberChannels = channels;
            bl->mBuffers[0].mDataByteSize   = 0;
            bl->mBuffers[0].mData           = NULL;
            *outSize = (UInt32)(sizeof(AudioBufferList) - sizeof(AudioBuffer) + sizeof(AudioBuffer));
            return 0;
        }

        case kAudioStreamPropertyIsActive:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = 1;
            *outSize = sizeof(UInt32); return 0;

        case kAudioStreamPropertyDirection:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = isInputStream ? 1u : 0u;   /* 1 = input                    */
            *outSize = sizeof(UInt32); return 0;

        case kAudioStreamPropertyTerminalType:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = isInputStream ? kAudioStreamTerminalTypeMicrophone
                                              : kAudioStreamTerminalTypeSpeaker;
            *outSize = sizeof(UInt32); return 0;

        case kAudioStreamPropertyStartingChannel:
            if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *(UInt32*)outData = 1;
            *outSize = sizeof(UInt32); return 0;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            if (dataSize < sizeof(AudioStreamBasicDescription))
                return kAudioHardwareBadPropertySizeError;
            FillFormat((AudioStreamBasicDescription*) outData,
                       isInputStream ? TotalInputChannels() : TotalOutputChannels());
            *outSize = sizeof(AudioStreamBasicDescription); return 0;
        }

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            if (dataSize < sizeof(AudioStreamRangedDescription))
                return kAudioHardwareBadPropertySizeError;
            AudioStreamRangedDescription* r = (AudioStreamRangedDescription*) outData;
            FillFormat(&r->mFormat, isInputStream ? TotalInputChannels() : TotalOutputChannels());
            r->mSampleRateRange.mMinimum = gSampleRate;
            r->mSampleRateRange.mMaximum = gSampleRate;
            *outSize = sizeof(AudioStreamRangedDescription); return 0;
        }
    }

    return kAudioHardwareUnknownPropertyError;
}

static OSStatus MR_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                   const AudioObjectPropertyAddress* addr,
                                   UInt32 qualDataSize, const void* qualData,
                                   UInt32 dataSize, const void* data)
{
    (void)d; (void)id; (void)pid; (void)addr;
    (void)qualDataSize; (void)qualData; (void)dataSize; (void)data;
    /*  Nothing settable yet — see MR_IsPropertySettable. */
    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - Factory

/*  Named in Info.plist's CFPlugInFactories. The HAL calls this to get the driver interface;
 *  everything else follows from the vtable above. */
void* MixerReturnCreate(CFAllocatorRef allocator, CFUUIDRef typeUUID);
void* MixerReturnCreate(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    (void)allocator;
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriverRef;
}
