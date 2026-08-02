// mrtest — headless numerical verification of the summing bus.
//
// Pushes real audio through the actual MixerReturnAudioProcessor and checks the sum
// numerically. The property under test is the one the whole design exists for:
// the bus sum must be delayed by exactly one block, identically for every sender,
// no matter what order the host processes the instances in.

#include "../Source/PluginProcessor.h"

#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    if (! ok)
    {
        ++failures;
        std::printf ("  FAIL  %s\n", what.toRawUTF8());
    }
    else
    {
        std::printf ("  ok    %s\n", what.toRawUTF8());
    }
}

void checkClose (float actual, float expected, const juce::String& what, float tol = 1.0e-6f)
{
    const bool ok = std::abs (actual - expected) <= tol;
    if (! ok)
        std::printf ("  FAIL  %s (got %.9f, expected %.9f)\n", what.toRawUTF8(), actual, expected);
    else
        std::printf ("  ok    %s (%.6f)\n", what.toRawUTF8(), actual);

    if (! ok)
        ++failures;
}

void setChoice (MixerReturnAudioProcessor& p, const char* id, int index, int numChoices)
{
    p.getState().getParameter (id)->setValueNotifyingHost ((float) index / (float) (numChoices - 1));
}

void setBool (MixerReturnAudioProcessor& p, const char* id, bool v)
{
    p.getState().getParameter (id)->setValueNotifyingHost (v ? 1.0f : 0.0f);
}

void setTrimDb (MixerReturnAudioProcessor& p, const char* id, float db)
{
    auto* param = p.getState().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (db));
}

constexpr int blockSize = 64;
constexpr double sampleRate = 96000.0; // the SQ core rate

struct Rig
{
    std::vector<std::unique_ptr<MixerReturnAudioProcessor>> senders;
    std::unique_ptr<MixerReturnAudioProcessor> master;
    std::vector<juce::AudioBuffer<float>> senderBuffers;
    juce::AudioBuffer<float> masterBuffer;
    juce::MidiBuffer midi;

    explicit Rig (int numSenders, int busIndex = 0)
    {
        for (int i = 0; i < numSenders; ++i)
        {
            auto p = std::make_unique<MixerReturnAudioProcessor>();
            setChoice (*p, mr::params::busSelect, busIndex, mr::numBuses);
            setChoice (*p, mr::params::outputMode, (int) mr::params::OutputMode::input, 3);
            setBool (*p, mr::params::sendEnable, true);
            setBool (*p, mr::params::sendMute, false);
            p->setPlayConfigDetails (2, 2, sampleRate, blockSize);
            p->prepareToPlay (sampleRate, blockSize);
            senders.push_back (std::move (p));

            senderBuffers.emplace_back (2, blockSize);
        }

        master = std::make_unique<MixerReturnAudioProcessor>();
        setChoice (*master, mr::params::busSelect, busIndex, mr::numBuses);
        setChoice (*master, mr::params::outputMode, (int) mr::params::OutputMode::busSum, 3);
        setBool (*master, mr::params::sendEnable, false);
        master->setPlayConfigDetails (2, 2, sampleRate, blockSize);
        master->prepareToPlay (sampleRate, blockSize);

        masterBuffer.setSize (2, blockSize);
    }

    ~Rig()
    {
        for (auto& s : senders)
            s->releaseResources();
        master->releaseResources();
    }

    /** Runs one block. `masterFirst` decides whether the summing instance is processed
        before or after its senders — the thing a host gets to choose and we must not
        depend on. Returns the master's first output sample. */
    float runBlock (int blockIndex, bool masterFirst)
    {
        for (size_t i = 0; i < senders.size(); ++i)
        {
            const auto value = (float) (blockIndex + 1) * (float) (i + 1) * 0.01f;
            senderBuffers[i].clear();
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (senderBuffers[i].getWritePointer (ch), value, blockSize);
        }

        masterBuffer.clear();

        auto processSenders = [&]
        {
            for (size_t i = 0; i < senders.size(); ++i)
                senders[i]->processBlock (senderBuffers[i], midi);
        };

        if (masterFirst)
        {
            master->processBlock (masterBuffer, midi);
            processSenders();
        }
        else
        {
            processSenders();
            master->processBlock (masterBuffer, midi);
        }

        return masterBuffer.getReadPointer (0)[0];
    }
};

/** Expected sum for a block of N senders: sum over i of (block+1)*(i+1)*0.01 */
float expectedSumForBlock (int blockIndex, int numSenders)
{
    float total = 0.0f;
    for (int i = 0; i < numSenders; ++i)
        total += (float) (blockIndex + 1) * (float) (i + 1) * 0.01f;
    return total;
}

void testOrderIndependence (bool masterFirst)
{
    constexpr int numSenders = 3;
    Rig rig (numSenders, masterFirst ? 0 : 1);

    std::printf ("\n-- one-block delay, master processed %s --\n", masterFirst ? "FIRST" : "LAST");

    for (int block = 0; block < 6; ++block)
    {
        const auto got = rig.runBlock (block, masterFirst);
        const auto expected = block == 0 ? 0.0f : expectedSumForBlock (block - 1, numSenders);
        checkClose (got, expected, "block " + juce::String (block));
    }
}

void testLatencyReporting()
{
    std::printf ("\n-- reported latency --\n");
    Rig rig (2, 2);
    check (rig.senders[0]->getLatencySamples() == 0, "a passthrough sender reports 0 samples");
    check (rig.master->getLatencySamples() == blockSize,
           "the summing instance reports one block (" + juce::String (blockSize) + " samples)");
}

void testTrimAndMute()
{
    std::printf ("\n-- send trim and mute --\n");
    Rig rig (2, 3);

    // The trim parameter has a 0.1 dB step, so the expectation has to be built from the
    // value the parameter can actually hold, not from a convenient round number.
    setTrimDb (*rig.senders[0], mr::params::sendTrim, -6.0f);
    rig.runBlock (0, false);
    const auto withTrim = rig.runBlock (1, false);
    // Block 0 values: sender0 = 0.01 (trimmed), sender1 = 0.02.
    const auto trimGain = juce::Decibels::decibelsToGain (-6.0f);
    checkClose (withTrim, 0.01f * trimGain + 0.02f, "-6 dB trim scales that sender's contribution");

    setBool (*rig.senders[0], mr::params::sendMute, true);
    rig.runBlock (2, false);
    const auto muted = rig.runBlock (3, false);
    // Block 2 values: sender0 = 0.03 (muted), sender1 = 0.06.
    checkClose (muted, 0.06f, "muting removes that sender and leaves nothing stale behind");
}

void testBypassStillArrives()
{
    std::printf ("\n-- a bypassed instance must still complete the barrier --\n");
    constexpr int numSenders = 3;
    Rig rig (numSenders, 4);

    rig.runBlock (0, false);

    // Block 1, but sender 1 is bypassed rather than processed normally.
    for (size_t i = 0; i < rig.senders.size(); ++i)
    {
        const auto value = 2.0f * (float) (i + 1) * 0.01f;
        rig.senderBuffers[i].clear();
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (rig.senderBuffers[i].getWritePointer (ch), value, blockSize);
    }

    rig.senders[0]->processBlock (rig.senderBuffers[0], rig.midi);
    rig.senders[1]->processBlockBypassed (rig.senderBuffers[1], rig.midi);
    rig.senders[2]->processBlock (rig.senderBuffers[2], rig.midi);
    rig.masterBuffer.clear();
    rig.master->processBlock (rig.masterBuffer, rig.midi);

    // Still reading block 0: 0.01 + 0.02 + 0.03
    checkClose (rig.masterBuffer.getReadPointer (0)[0], 0.06f, "block 1 still sees block 0's complete sum");

    // Block 2 must show block 1, with the bypassed sender contributing nothing:
    // 0.02 + (bypassed) + 0.06
    const auto next = rig.runBlock (2, false);
    checkClose (next, 0.02f + 0.06f, "the bypassed sender contributes silence, not a stale block");
}

void testBusIsolation()
{
    std::printf ("\n-- buses are independent --\n");
    Rig busA (2, 5);
    Rig busB (2, 6);

    busA.runBlock (0, false);
    busB.runBlock (0, false);
    const auto a = busA.runBlock (1, false);
    const auto b = busB.runBlock (1, false);

    checkClose (a, 0.03f, "bus 6 sums only its own members");
    checkClose (b, 0.03f, "bus 7 sums only its own members");
}
/** The real deployment shape: 24 channels of automixed direct outs summed to one return,
    with the host free to reorder the instances on every single block. */
void testTwentyFourChannelsShuffled()
{
    std::printf ("\n-- 24 senders, processing order reshuffled every block --\n");

    constexpr int numSenders = 24;
    Rig rig (numSenders, 7);

    std::vector<int> order (numSenders + 1);
    std::iota (order.begin(), order.end(), 0); // index numSenders == the master
    juce::Random rng (20260802);

    bool allGood = true;

    for (int block = 0; block < 24; ++block)
    {
        for (int i = (int) order.size(); --i > 0;)
            std::swap (order[(size_t) i], order[(size_t) rng.nextInt (i + 1)]);

        for (int i = 0; i < numSenders; ++i)
        {
            const auto value = (float) (block + 1) * (float) (i + 1) * 0.01f;
            rig.senderBuffers[(size_t) i].clear();
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (
                    rig.senderBuffers[(size_t) i].getWritePointer (ch), value, blockSize);
        }
        rig.masterBuffer.clear();

        for (auto index : order)
        {
            if (index == numSenders)
                rig.master->processBlock (rig.masterBuffer, rig.midi);
            else
                rig.senders[(size_t) index]->processBlock (rig.senderBuffers[(size_t) index], rig.midi);
        }

        const auto expected = block == 0 ? 0.0f : expectedSumForBlock (block - 1, numSenders);

        // Relative, because summing 24 floats in a different order than the reference
        // legitimately differs by a couple of ULP and these test values run very hot.
        const auto tolerance = juce::jmax (1.0e-6f, std::abs (expected) * 2.0e-6f);

        // Every sample in the block must carry the same sum, not just the first.
        for (int s = 0; s < blockSize; ++s)
        {
            if (std::abs (rig.masterBuffer.getReadPointer (0)[s] - expected) > tolerance)
            {
                std::printf ("  FAIL  block %d sample %d: got %.9f, expected %.9f\n",
                             block, s, rig.masterBuffer.getReadPointer (0)[s], expected);
                allGood = false;
                ++failures;
                break;
            }
        }
    }

    check (allGood, "24 blocks x 64 samples, order reshuffled each block, delay stayed uniform");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("MixerReturn summing verification (%d Hz, %d-sample blocks)\n",
                 (int) sampleRate, blockSize);

    testOrderIndependence (false);
    testOrderIndependence (true);
    testLatencyReporting();
    testTrimAndMute();
    testBypassStillArrives();
    testBusIsolation();
    testTwentyFourChannelsShuffled();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
