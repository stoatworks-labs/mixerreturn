#include "SummingBus.h"

#include <algorithm>
#include <cstring>

namespace mr
{

int SummingBus::acquireSlot (int blockSize, int numChannels)
{
    const auto channels = std::min (numChannels, maxChannels);

    for (int i = 0; i < maxSlots; ++i)
    {
        auto& s = slots[(size_t) i];

        // `claimed` rather than `active`: the buffers have to be sized and zeroed before
        // any reader is allowed to see this slot, so publishing `active` is the last step.
        bool expected = false;
        if (! s.claimed.compare_exchange_strong (expected, true))
            continue;

        for (auto& page : s.pages)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                auto& buffer = page[(size_t) ch];

                if ((int) buffer.size() < blockSize)
                    buffer.assign ((size_t) blockSize, 0.0f);
                else
                    std::fill (buffer.begin(), buffer.end(), 0.0f);
            }
        }

        s.counted.store (true, std::memory_order_release);
        s.arrivedThisRound.store (false, std::memory_order_release);
        members.fetch_add (1, std::memory_order_acq_rel);
        s.active.store (true, std::memory_order_release);
        return i;
    }

    return -1;
}

void SummingBus::releaseSlot (int slot)
{
    if (slot < 0 || slot >= maxSlots)
        return;

    auto& s = slots[(size_t) slot];

    if (! s.active.exchange (false, std::memory_order_acq_rel))
        return;

    // Only if it is still counted — the barrier may already have dropped it for
    // not arriving. See arrive().
    if (s.counted.exchange (false, std::memory_order_acq_rel))
        members.fetch_sub (1, std::memory_order_acq_rel);

    s.arrivedThisRound.store (false, std::memory_order_release);

    // A member leaving mid-block would otherwise strand the barrier one arrival short
    // for the rest of that block.
    arrived.store (0, std::memory_order_release);

    s.claimed.store (false, std::memory_order_release);
}

void SummingBus::writeSlot (int slot, int channel, const float* src, int numSamples, float gain) noexcept
{
    if (slot < 0 || slot >= maxSlots || channel < 0 || channel >= maxChannels)
        return;

    const auto page = (size_t) writePage.load (std::memory_order_acquire);
    auto& dst = slots[(size_t) slot].pages[page][(size_t) channel];

    const auto n = (size_t) std::min (numSamples, (int) dst.size());

    for (size_t i = 0; i < n; ++i)
        dst[i] = src[i] * gain;

    // Anything beyond this block must not survive from a previous, longer one.
    std::fill (dst.begin() + (long) n, dst.end(), 0.0f);
}

void SummingBus::clearSlot (int slot, int channel, int numSamples) noexcept
{
    if (slot < 0 || slot >= maxSlots || channel < 0 || channel >= maxChannels)
        return;

    (void) numSamples; // the whole page is cleared, not just this block's worth

    const auto page = (size_t) writePage.load (std::memory_order_acquire);
    auto& dst = slots[(size_t) slot].pages[page][(size_t) channel];
    std::fill (dst.begin(), dst.end(), 0.0f);
}

void SummingBus::readSum (float* dst, int channel, int numSamples) noexcept
{
    if (channel < 0 || channel >= maxChannels)
        return;

    // The completed page is always the one nobody is writing.
    const auto page = (size_t) (writePage.load (std::memory_order_acquire) ^ 1);

    std::memset (dst, 0, sizeof (float) * (size_t) numSamples);

    for (auto& slot : slots)
    {
        if (! slot.active.load (std::memory_order_acquire))
            continue;

        const auto& src = slot.pages[page][(size_t) channel];
        const auto n = (size_t) std::min (numSamples, (int) src.size());

        for (size_t i = 0; i < n; ++i)
            dst[i] += src[i];
    }
}

void SummingBus::arrive (int slot) noexcept
{
    if (slot < 0 || slot >= maxSlots)
        return;

    auto& self = slots[(size_t) slot];

    // A slot that was dropped for not arriving (below) rejoins as soon as it is
    // processed again.
    if (! self.counted.exchange (true, std::memory_order_acq_rel))
        members.fetch_add (1, std::memory_order_acq_rel);

    // The barrier counts SLOT HOLDERS, and a slot is taken in prepareToPlay on the
    // message thread — so an instance the host has not started processing, or has
    // stopped processing, is counted and never arrives. With M counted and K < M
    // actually processed the flip condition is never met, and the pages turn over
    // once every ceil(M/K) blocks: senders overwrite their own write page and
    // readers see the same page twice. That is a repeated block and a dropped
    // block, not the uniform one-block delay AGENTS.md §2 calls the rule that
    // matters most, and it persists for as long as the idle instance holds a slot.
    //
    // Nothing can tell the barrier that member has gone. But in a healthy round
    // every member arrives exactly once, so this member arriving a SECOND time
    // before the round closed means some other counted member is not arriving at
    // all. Flip on that rather than waiting for one that will never come, and drop
    // whoever missed the round so the count converges on who is really here. One
    // disturbed block, then correct — instead of alternating forever.
    const bool stalled = self.arrivedThisRound.exchange (true, std::memory_order_acq_rel);

    const auto count    = arrived.fetch_add (1, std::memory_order_acq_rel) + 1;
    const auto expected = members.load (std::memory_order_relaxed);

    // >= rather than == so that a member disappearing mid-block cannot wedge the
    // barrier permanently. The cost is that a reconfiguration can flip twice in one
    // block, which is an audible tick at worst and self-corrects on the next one.
    if (count >= expected || stalled)
    {
        if (stalled)
        {
            for (auto& s : slots)
                if (s.active.load (std::memory_order_acquire)
                    && s.counted.load (std::memory_order_acquire)
                    && ! s.arrivedThisRound.load (std::memory_order_acquire))
                {
                    s.counted.store (false, std::memory_order_release);
                    members.fetch_sub (1, std::memory_order_acq_rel);
                }
        }

        for (auto& s : slots)
            s.arrivedThisRound.store (false, std::memory_order_release);

        writePage.fetch_xor (1, std::memory_order_acq_rel);

        // On the stall path this arrival is not the END of the round that just
        // closed — that round is being abandoned — it is the FIRST arrival of the
        // new one. Counting it as zero leaves the new round permanently one short,
        // which just moves the fault along by a block instead of curing it.
        if (stalled)
        {
            self.arrivedThisRound.store (true, std::memory_order_release);
            arrived.store (1, std::memory_order_release);
        }
        else
        {
            arrived.store (0, std::memory_order_release);
        }
    }
}

BusRegistry& BusRegistry::get()
{
    static BusRegistry instance;
    return instance;
}

SummingBus& BusRegistry::bus (int index) noexcept
{
    return buses[(size_t) std::clamp (index, 0, numBuses - 1)];
}

} // namespace mr
