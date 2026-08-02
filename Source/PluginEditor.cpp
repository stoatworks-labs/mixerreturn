#include "PluginEditor.h"

#include "PluginParameters.h"

namespace
{
void styleTrim (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 68, 20);

    // The default track colours sit too close to the panel background to read as a groove
    // with a filled portion, which makes the travelled part of the range invisible.
    s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff0e1116));
    s.setColour (juce::Slider::trackColourId, juce::Colour (0xff4fc3f7).withAlpha (0.55f));
    s.setColour (juce::Slider::thumbColourId, juce::Colour (0xff4fc3f7));
}

// One layout table, so paint() and resized() cannot drift apart.
constexpr int margin      = 16;
constexpr int titleH      = 30;
constexpr int comboH      = 26;
constexpr int headingH    = 22;
constexpr int rowH        = 24;
constexpr int meterH      = 10;
constexpr int gap         = 6;
constexpr int trimLabelW  = 38;

struct Layout
{
    int busY, sendHeadingY, sendRowY, sendTrimY, sendMeterY;
    int outHeadingY, outComboY, outTrimY, outMeterY, statusY, totalHeight;

    Layout()
    {
        int y = margin + titleH;
        busY         = y;                    y += comboH + gap * 2;
        sendHeadingY = y;                    y += headingH;
        sendRowY     = y;                    y += rowH;
        sendTrimY    = y;                    y += rowH + 2;
        sendMeterY   = y;                    y += meterH + gap * 2;
        outHeadingY  = y;                    y += headingH;
        outComboY    = y;                    y += comboH + gap;
        outTrimY     = y;                    y += rowH + 2;
        outMeterY    = y;                    y += meterH + gap * 2;
        statusY      = y;                    y += 18;
        totalHeight  = y + margin;
    }
};

const Layout& layout()
{
    static const Layout l;
    return l;
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

    setSize (420, layout().totalHeight);
    startTimerHz (24);
}

MixerReturnAudioProcessorEditor::~MixerReturnAudioProcessorEditor() = default;

void MixerReturnAudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto& l = layout();
    const auto right = (float) getWidth() - margin;

    g.fillAll (juce::Colour (0xff1b1f24));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("MixerReturn", margin, margin, getWidth() - margin * 2, 24,
                juce::Justification::centredLeft);

    auto sectionHeading = [&] (const char* text, int y)
    {
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawHorizontalLine (y - gap, (float) margin, right);

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (text, margin, y, 200, headingH, juce::Justification::centredLeft);
    };

    sectionHeading ("SEND", l.sendHeadingY);
    sectionHeading ("OUTPUT", l.outHeadingY);

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("Trim", margin, l.sendTrimY, trimLabelW, rowH, juce::Justification::centredLeft);
    g.drawText ("Trim", margin, l.outTrimY, trimLabelW, rowH, juce::Justification::centredLeft);
}

void MixerReturnAudioProcessorEditor::resized()
{
    const auto& l = layout();
    const auto x = margin;
    const auto w = getWidth() - margin * 2;

    busBox.setBounds (x, l.busY, w, comboH);

    sendButton.setBounds (x, l.sendRowY, 78, rowH);
    muteButton.setBounds (x + 82, l.sendRowY, 78, rowH);

    sendTrimSlider.setBounds (x + trimLabelW, l.sendTrimY, w - trimLabelW, rowH);
    sendMeter.setBounds (x, l.sendMeterY, w, meterH);

    outputModeBox.setBounds (x, l.outComboY, w, comboH);
    outputTrimSlider.setBounds (x + trimLabelW, l.outTrimY, w - trimLabelW, rowH);
    outputMeter.setBounds (x, l.outMeterY, w, meterH);

    statusLabel.setBounds (x, l.statusY, w, 18);
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
