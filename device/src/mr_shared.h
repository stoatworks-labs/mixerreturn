/*  The contract between the driver and the helper daemon.
 *
 *  The driver lives inside coreaudiod and cannot open a CoreAudio device itself, so the
 *  helper owns the real interface and the two exchange audio through one shared memory
 *  region. This header is the only thing they agree on; changing it means rebuilding both,
 *  which is what MR_SHM_VERSION is for.
 *
 *  Nothing in here may be touched by anything but plain loads and stores from the audio
 *  side. No locks, no allocation, no syscalls — the driver's IO operations run on
 *  coreaudiod's realtime threads and the helper's run on the hardware IOProc thread, and
 *  a blocked audio thread is a dropout in both processes at once.
 */

#ifndef MR_SHARED_H
#define MR_SHARED_H

#include <stdint.h>
#include <stdatomic.h>

#define MR_SHM_NAME      "/mixerreturn.shm"
#define MR_SHM_VERSION   1

/*  Hard ceilings, so the region is a fixed size and can be mapped once. Generous against
 *  the reference rig (24 mics on an SQ) without being absurd: a 64-in interface with a Sum
 *  port for every input is already past what SuperRack will host racks for. */
#define MR_MAX_CHANNELS  64      /* physical inputs, and physical outputs, each          */
#define MR_MAX_SUMS      64      /* one Sum port per input                               */
#define MR_MAX_BUSES     8       /* stereo, matching the control surface                 */
#define MR_RING_FRAMES   8192    /* per channel; power of two so wrapping is a mask       */

#define MR_RING_MASK     (MR_RING_FRAMES - 1)

/*  Which buses a Sum port feeds. A bitmask rather than a list because the crosspoint matrix
 *  is genuinely many-to-many — a Sum port may feed several buses at once, which is how one
 *  automixed channel lands in both a main return and a separate record feed. */
typedef struct {
    _Atomic uint32_t busMask;    /* bit b set = this Sum port feeds bus b                 */
    _Atomic float    gain;       /* linear, applied on the way into the sum               */
} MRSumAssign;

/*  Where a bus comes out. The pair is expressed as physical output channel indices so the
 *  helper does not have to know anything about stereo pairing rules. -1 = not landed. */
typedef struct {
    _Atomic int32_t  outLeft;
    _Atomic int32_t  outRight;
    _Atomic float    gain;
} MRBusOutput;

/*  One producer, one consumer, per channel. `write` is only ever advanced by the producer
 *  and `read` only by the consumer, so a single atomic each is enough and neither side ever
 *  waits: a reader that finds nothing emits silence and a writer that finds no room drops,
 *  which is the correct behaviour when the alternative is blocking an audio thread.
 *
 *  Deliberately not one ring for all channels: the driver writes Sum ports in whatever
 *  order the host hands them over, and interleaving would force it to buffer a whole cycle
 *  before it could write anything. */
typedef struct {
    _Atomic uint64_t write;
    _Atomic uint64_t read;
    float            data[MR_RING_FRAMES];
} MRRing;

/*  Set by the helper when it has the hardware open, cleared when it lets go. The driver
 *  reads it to decide whether it can claim to be alive: a virtual device that accepts IO
 *  while nothing is behind it is a device that silently eats a show's audio. */
typedef enum {
    MR_HELPER_ABSENT  = 0,
    MR_HELPER_STARTING= 1,
    MR_HELPER_RUNNING = 2,
    MR_HELPER_FAILED  = 3,
} MRHelperState;

typedef struct {
    uint32_t          version;          /* MR_SHM_VERSION; mismatch means do not touch    */
    _Atomic uint32_t  helperState;      /* MRHelperState                                  */

    /*  What the helper found when it opened the hardware. The driver mirrors these into
     *  its own stream formats, which is what makes this a wrapper rather than a fixed
     *  virtual device. */
    _Atomic uint32_t  numInputs;
    _Atomic uint32_t  numOutputs;
    _Atomic uint32_t  numSums;
    _Atomic double    sampleRate;

    /*  The hardware's clock, published by the helper's IOProc every cycle. The driver
     *  anchors GetZeroTimeStamp to this so the virtual device runs on the hardware's
     *  timeline rather than a free-running one of its own — two clocks would drift and
     *  the wrapper would slowly slip against the interface it is wrapping. */
    _Atomic uint64_t  anchorHostTime;   /* mach_absolute_time at the sample below         */
    _Atomic uint64_t  anchorSampleTime;
    _Atomic uint32_t  cycleFrames;      /* the hardware's buffer size                     */

    MRSumAssign       sums[MR_MAX_SUMS];
    MRBusOutput       buses[MR_MAX_BUSES];

    /*  hw input -> driver (appears to the host as the mirrored physical inputs)          */
    MRRing            captureRing[MR_MAX_CHANNELS];
    /*  driver -> hw output. Already carries the mirrored physical outputs with the summed
     *  buses mixed in, because the summing happens driver-side where every Sum port for a
     *  cycle is in hand at once. */
    MRRing            playbackRing[MR_MAX_CHANNELS];

    /*  Diagnostics. Counters rather than logs: an audio thread cannot log, and a dropout
     *  that leaves no trace is one nobody can argue about after the show. */
    _Atomic uint64_t  captureOverruns;
    _Atomic uint64_t  playbackUnderruns;
} MRShared;

#endif /* MR_SHARED_H */
