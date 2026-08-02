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

    members.fetch_sub (1, std::memory_order_acq_rel);

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

void SummingBus::arrive() noexcept
{
    const auto count   = arrived.fetch_add (1, std::memory_order_acq_rel) + 1;
    const auto expected = members.load (std::memory_order_relaxed);

    // >= rather than == so that a member disappearing mid-block cannot wedge the
    // barrier permanently. The cost is that a reconfiguration can flip twice in one
    // block, which is an audible tick at worst and self-corrects on the next one.
    if (count >= expected)
    {
        writePage.fetch_xor (1, std::memory_order_acq_rel);
        arrived.store (0, std::memory_order_release);
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
