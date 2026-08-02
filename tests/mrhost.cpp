// mrhost — runs the summing test through the real VST3 bundle, as a host would.
//
// mrtest constructs MixerReturnAudioProcessor directly, which proves the bus logic but
// never crosses the plugin-format boundary. The whole design rests on instances finding
// each other through a process-wide registry, and that assumption only holds if every
// instance a host creates from one bundle shares the same loaded copy of it. Nothing in a
// direct-construction test can confirm that, and if it were false — a host sandboxing each
// instance, or a bundle somehow loaded twice — the product would silently do nothing at
// all, with every instance reporting a bus of one.
//
// So this loads the built .vst3, asks it for several instances, and checks the sum.
//
// Usage: mrhost <path-to-MixerReturn.vst3>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

constexpr int    blockSize  = 64;
constexpr double sampleRate = 96000.0;

/** Matches on the displayed name, not the parameter ID.

    VST3 parameter IDs are 32-bit integers, so JUCE hashes our string IDs on the way out:
    a host sees "bus" as "97920". That is normal VST3 behaviour rather than a fault, but it
    does mean the string IDs in PluginParameters.h are not what a host addresses, and the
    name is the only stable handle from this side. Worth knowing before anyone tries to
    automate this plugin by ID. */
juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, juce::StringRef name)
{
    for (auto* param : p.getParameters())
        if (param->getName (64) == name)
            return param;

    return nullptr;
}

bool setParam (juce::AudioPluginInstance& p, juce::StringRef name, float normalised)
{
    if (auto* param = findParam (p, name))
    {
        param->setValueNotifyingHost (normalised);
        return true;
    }

    return false;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc < 2)
    {
        std::printf ("usage: mrhost <path-to-MixerReturn.vst3>\n");
        return 2;
    }

    const juce::File bundle { juce::String (argv[1]) };
    std::printf ("MixerReturn VST3 host check\n  bundle: %s\n", bundle.getFullPathName().toRawUTF8());

    if (! bundle.exists())
    {
        std::printf ("  FAIL  bundle does not exist\n");
        return 1;
    }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, bundle.getFullPathName());

    check (! found.isEmpty(), "the bundle advertises at least one plugin type");

    if (found.isEmpty())
        return 1;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (new juce::VST3PluginFormat());

    constexpr int numSenders = 8;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> senders;
    std::unique_ptr<juce::AudioPluginInstance> master;

    auto make = [&] () -> std::unique_ptr<juce::AudioPluginInstance>
    {
        juce::String error;
        auto instance = formats.createPluginInstance (*found[0], sampleRate, blockSize, error);

        if (instance == nullptr)
            std::printf ("  FAIL  could not instantiate: %s\n", error.toRawUTF8());

        return instance;
    };

    for (int i = 0; i < numSenders; ++i)
    {
        auto p = make();
        if (p == nullptr) return 1;

        setParam (*p, "Bus", 0.0f);        // Bus 1
        setParam (*p, "Output", 0.0f);     // pass the input through
        setParam (*p, "Send", 1.0f);
        setParam (*p, "Send Mute", 0.0f);

        p->prepareToPlay (sampleRate, blockSize);
        senders.push_back (std::move (p));
    }

    master = make();
    if (master == nullptr) return 1;

    check (setParam (*master, "Bus", 0.0f), "the host can address the plugin's parameters");
    setParam (*master, "Output", 0.5f);  // Bus Sum, the middle of three choices
    setParam (*master, "Send", 0.0f);
    master->prepareToPlay (sampleRate, blockSize);

    const auto reported = master->getLatencySamples();
    check (reported == blockSize,
           "the summing instance reports one block of latency through the wrapper ("
               + juce::String (reported) + " samples)");

    std::vector<juce::AudioBuffer<float>> buffers;
    for (int i = 0; i < numSenders; ++i)
        buffers.emplace_back (2, blockSize);

    juce::AudioBuffer<float> masterBuffer (2, blockSize);
    juce::MidiBuffer midi;

    bool allGood = true;

    for (int block = 0; block < 8 && allGood; ++block)
    {
        for (int i = 0; i < numSenders; ++i)
        {
            const auto value = (float) (block + 1) * (float) (i + 1) * 0.01f;
            buffers[(size_t) i].clear();
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (buffers[(size_t) i].getWritePointer (ch),
                                                   value, blockSize);
            senders[(size_t) i]->processBlock (buffers[(size_t) i], midi);
        }

        masterBuffer.clear();
        master->processBlock (masterBuffer, midi);

        float expected = 0.0f;
        if (block > 0)
            for (int i = 0; i < numSenders; ++i)
                expected += (float) block * (float) (i + 1) * 0.01f;

        const auto got = masterBuffer.getReadPointer (0)[0];
        const auto tolerance = juce::jmax (1.0e-6f, std::abs (expected) * 2.0e-6f);

        if (std::abs (got - expected) > tolerance)
        {
            std::printf ("  FAIL  block %d: got %.9f, expected %.9f\n", block, got, expected);
            allGood = false;
            ++failures;
        }
    }

    // The claim under test. If each instance had its own copy of the registry the sum
    // would be silence, and the plugin would be useless in the way that is hardest to
    // notice: every control working, every meter moving, and no audio arriving.
    check (allGood, juce::String (numSenders)
                        + " senders and a return, all created from the one bundle, summed correctly"
                          " -- so instances really do share one registry");

    for (auto& s : senders)
        s->releaseResources();
    master->releaseResources();

    senders.clear();
    master.reset();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
