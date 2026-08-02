// mrshot — renders the plugin editor straight to PNG for the documentation.
//
// Deliberately not a screen capture. Driving a window server to photograph a window is
// unreliable here (blank frames, and captures of whatever window happened to be in front),
// and it cannot show a populated bus anyway: the standalone build is a single instance, so
// the member count would always read 1. This registers a realistic rig, pushes audio
// through it, and renders the real editor component offscreen.
//
// Usage: mrshot <output-directory>

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <cstdio>

namespace
{
constexpr int    blockSize  = 64;
constexpr double sampleRate = 96000.0;

void setChoice (MixerReturnAudioProcessor& p, const char* id, int index, int numChoices)
{
    p.getState().getParameter (id)->setValueNotifyingHost ((float) index / (float) (numChoices - 1));
}

void setBool (MixerReturnAudioProcessor& p, const char* id, bool v)
{
    p.getState().getParameter (id)->setValueNotifyingHost (v ? 1.0f : 0.0f);
}

bool writeSnapshot (MixerReturnAudioProcessor& processor, const juce::File& target)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

    if (editor == nullptr)
        return false;

    editor->setBounds (0, 0, editor->getWidth(), editor->getHeight());

    // Let the editor's timer run so the meters and the member-count readout are populated
    // rather than showing their initial state.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (250);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);

    target.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (target.createOutputStream());

    if (stream == nullptr)
        return false;

    juce::PNGImageFormat png;
    return png.writeImageToStream (image, *stream);
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File outDir (argc > 1 ? juce::String (argv[1])
                                      : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();

    // A realistic rig: 24 automixed channels sending to bus 1, plus the instance that
    // returns their sum. The member count in the corner is therefore truthful.
    std::vector<std::unique_ptr<MixerReturnAudioProcessor>> senders;
    std::vector<juce::AudioBuffer<float>> buffers;

    for (int i = 0; i < 24; ++i)
    {
        auto p = std::make_unique<MixerReturnAudioProcessor>();
        setChoice (*p, mr::params::busSelect, 0, mr::numBuses);
        setChoice (*p, mr::params::outputMode, (int) mr::params::OutputMode::input, 3);
        setBool (*p, mr::params::sendEnable, true);
        p->setPlayConfigDetails (2, 2, sampleRate, blockSize);
        p->prepareToPlay (sampleRate, blockSize);
        senders.push_back (std::move (p));

        buffers.emplace_back (2, blockSize);
    }

    auto master = std::make_unique<MixerReturnAudioProcessor>();
    setChoice (*master, mr::params::busSelect, 0, mr::numBuses);
    setChoice (*master, mr::params::outputMode, (int) mr::params::OutputMode::busSum, 3);
    setBool (*master, mr::params::sendEnable, false);
    master->setPlayConfigDetails (2, 2, sampleRate, blockSize);
    master->prepareToPlay (sampleRate, blockSize);
    juce::AudioBuffer<float> masterBuffer (2, blockSize);

    // Give the meters something to show: programme-level pink-ish noise, not full scale.
    juce::Random rng (20260802);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        for (size_t i = 0; i < senders.size(); ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = buffers[i].getWritePointer (ch);
                for (int s = 0; s < blockSize; ++s)
                    d[s] = (rng.nextFloat() * 2.0f - 1.0f) * 0.03f;
            }
            senders[i]->processBlock (buffers[i], midi);
        }

        masterBuffer.clear();
        master->processBlock (masterBuffer, midi);
    }

    const auto sendShot = outDir.getChildFile ("send.png");
    const auto sumShot  = outDir.getChildFile ("sum.png");

    const bool ok = writeSnapshot (*senders.front(), sendShot)
                 && writeSnapshot (*master, sumShot);

    for (auto& s : senders)
        s->releaseResources();
    master->releaseResources();

    if (! ok)
    {
        std::printf ("failed to write snapshots\n");
        return 1;
    }

    std::printf ("wrote %s\nwrote %s\n",
                 sendShot.getFullPathName().toRawUTF8(),
                 sumShot.getFullPathName().toRawUTF8());
    return 0;
}
