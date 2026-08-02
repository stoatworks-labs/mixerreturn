#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "GUI/LevelBar.h"
#include "PluginProcessor.h"
#include "StoatworksAboutPanel.h"

class MixerReturnAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit MixerReturnAudioProcessorEditor (MixerReturnAudioProcessor&);
    ~MixerReturnAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    MixerReturnAudioProcessor& proc;

    juce::ComboBox   busBox;
    juce::ToggleButton sendButton { "Send" };
    juce::ToggleButton muteButton { "Mute" };
    juce::Slider     sendTrimSlider;
    juce::ComboBox   outputModeBox;
    juce::Slider     outputTrimSlider;

    LevelBar sendMeter, outputMeter;
    juce::Label statusLabel;

    /* Vendored from stoatworks-backend/about - see StoatworksAboutPanel.h.
       A child of the editor rather than a window of its own: a plugin must not
       put a second top-level window on a host's screen. */
    juce::TextButton aboutButton { "i" };
    stoatworks::AboutPanel aboutPanel;

    std::unique_ptr<ComboAttachment>  busAttachment, outputModeAttachment;
    std::unique_ptr<ButtonAttachment> sendAttachment, muteAttachment;
    std::unique_ptr<SliderAttachment> sendTrimAttachment, outputTrimAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerReturnAudioProcessorEditor)
};
