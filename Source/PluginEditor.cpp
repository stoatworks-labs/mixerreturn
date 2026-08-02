#include "PluginEditor.h"

#include "PluginParameters.h"

namespace
{
void styleTrim (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
}
} // namespace

MixerReturnAudioProcessorEditor::MixerReturnAudioProcessorEditor (MixerReturnAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p)
{
    auto& state = proc.getState();

    for (int i = 1; i <= mr::numBuses; ++i)
        busBox.addItem ("Bus " + juce::String (i), i);

    outputModeBox.addItem ("Input", 1);
    outputModeBox.addItem ("Bus Sum", 2);
    outputModeBox.addItem ("Input + Bus Sum", 3);

    styleTrim (sendTrimSlider);
    styleTrim (outputTrimSlider);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (juce::FontOptions (12.0f));

    for (auto* c : std::initializer_list<juce::Component*> {
             &busBox, &sendButton, &muteButton, &sendTrimSlider,
             &outputModeBox, &outputTrimSlider, &sendMeter, &outputMeter, &statusLabel })
        addAndMakeVisible (c);

    busAttachment        = std::make_unique<ComboAttachment> (state, mr::params::busSelect, busBox);
    sendAttachment       = std::make_unique<ButtonAttachment> (state, mr::params::sendEnable, sendButton);
    muteAttachment       = std::make_unique<ButtonAttachment> (state, mr::params::sendMute, muteButton);
    sendTrimAttachment   = std::make_unique<SliderAttachment> (state, mr::params::sendTrim, sendTrimSlider);
    outputModeAttachment = std::make_unique<ComboAttachment> (state, mr::params::outputMode, outputModeBox);
    outputTrimAttachment = std::make_unique<SliderAttachment> (state, mr::params::outputTrim, outputTrimSlider);

    setSize (420, 260);
    startTimerHz (24);
}

MixerReturnAudioProcessorEditor::~MixerReturnAudioProcessorEditor() = default;

void MixerReturnAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1f24));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("MixerReturn", 16, 12, getWidth() - 32, 24, juce::Justification::centredLeft);

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("SEND", 16, 78, 200, 16, juce::Justification::centredLeft);
    g.drawText ("OUTPUT", 16, 154, 200, 16, juce::Justification::centredLeft);

    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.drawHorizontalLine (72, 16.0f, (float) getWidth() - 16.0f);
    g.drawHorizontalLine (148, 16.0f, (float) getWidth() - 16.0f);
}

void MixerReturnAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (28);

    busBox.setBounds (area.removeFromTop (26));

    area.removeFromTop (30); // "SEND" heading and rule
    auto sendRow = area.removeFromTop (24);
    sendButton.setBounds (sendRow.removeFromLeft (70));
    muteButton.setBounds (sendRow.removeFromLeft (70));
    sendMeter.setBounds (sendRow.reduced (4, 7));

    sendTrimSlider.setBounds (area.removeFromTop (24));

    area.removeFromTop (30); // "OUTPUT" heading and rule
    outputModeBox.setBounds (area.removeFromTop (26));

    auto outRow = area.removeFromTop (24);
    outputTrimSlider.setBounds (outRow.removeFromLeft (outRow.getWidth() - 90));
    outputMeter.setBounds (outRow.reduced (6, 7));

    statusLabel.setBounds (area.removeFromBottom (18));
}

void MixerReturnAudioProcessorEditor::timerCallback()
{
    sendMeter.setLevel (proc.getSendPeak());
    outputMeter.setLevel (proc.getOutputPeak());

    const auto members = proc.getBusMemberCount();
    const auto latency = proc.getLatencySamples();

    juce::String status;
    status << members << (members == 1 ? " member" : " members") << " on this bus";

    if (latency > 0)
        status << "  |  " << latency << " samples latency";

    statusLabel.setText (status, juce::dontSendNotification);
}
