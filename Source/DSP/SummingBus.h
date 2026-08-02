#pragma once

#include <array>
#include <atomic>
#include <vector>

namespace mr
{

/** Hard limits. Buses are cheap; slots cost a pair of block-sized buffers each. */
static constexpr int numBuses     = 8;
static constexpr int maxSlots     = 64;
static constexpr int maxChannels  = 2;

/**
    One summing bus, shared by every plugin instance in the process that selects it.

    The problem this solves
    ----------------------
    A host processes plugin instances in an order it chooses, possibly across several
    threads. An instance that sums its peers therefore cannot assume its peers have
    already run this block: whoever happens to be scheduled after the summing instance
    would contribute nothing, and whoever runs before would contribute this block —
    giving *different* channels different delays. On an automix bus that is not a
    latency problem, it is a comb filter.

    So the bus never lets anyone read the block currently being written. There are two
    pages. Senders always write the write page; readers always read the other one, which
    is by definition complete. Every member calls arrive() as the last thing it does in
    its block, and the member that arrives last flips the pages.

    The result is one block of delay, identical for every member regardless of the order
    the host picked. That is the whole point: uniform beats short.

    Bypassed instances must still call arrive(), or the barrier never completes — see
    the note on processBlockBypassed in PluginProcessor.cpp.
*/
class SummingBus
{
public:
    SummingBus() = default;

    /** Sizes the slot buffers. Safe to call repeatedly; only grows. Not real-time safe,
        so it belongs in prepareToPlay. */
    void prepare (int blockSize, int numChannels);

    /** Claims a slot for one plugin instance. Returns -1 if the bus is full.
        Message thread only. */
    int acquireSlot();

    /** Releases a slot and zeroes both its pages, so a removed member cannot leave a
        block of audio stuck in the sum. Message thread only. */
    void releaseSlot (int slot);

    /** Number of instances currently holding a slot. */
    int getMemberCount() const noexcept { return members.load (std::memory_order_relaxed); }

    // ---- audio thread ----

    /** Copies src into this slot's write page, scaled by gain. */
    void writeSlot (int slot, int channel, const float* src, int numSamples, float gain) noexcept;

    /** Zeroes this slot's write page. A muted or non-sending member must call this, not
        simply skip writing — its previous block would otherwise persist in the sum. */
    void clearSlot (int slot, int channel, int numSamples) noexcept;

    /** Sums every slot's read page into dst (replacing its contents). */
    void readSum (float* dst, int channel, int numSamples) noexcept;

    /** Signals that this member has finished the block. The last to arrive flips the
        pages. Must be called exactly once per member per block. */
    void arrive() noexcept;

private:
    struct Slot
    {
        std::atomic<bool> active { false };
        // [page][channel][sample], flattened per page/channel into one vector.
        std::array<std::array<std::vector<float>, maxChannels>, 2> pages;
    };

    std::array<Slot, maxSlots> slots;

    std::atomic<int> members   { 0 };
    std::atomic<int> arrived   { 0 };
    std::atomic<int> writePage { 0 };

    int preparedBlockSize = 0;
    int preparedChannels  = 0;
};

/** Process-wide bus registry.

    Deliberately per-process: instances in two separate hosts cannot see each other.
    That is a real constraint, not an oversight — crossing process boundaries would mean
    shared memory and a cross-process barrier, and the use case this was built for keeps
    every rack inside one SuperRack. */
class BusRegistry
{
public:
    static BusRegistry& get();

    SummingBus& bus (int index) noexcept;

private:
    BusRegistry() = default;
    std::array<SummingBus, numBuses> buses;
};

} // namespace mr
