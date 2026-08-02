#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/SummingBus.h"
#include "PluginParameters.h"

class MixerReturnAudioProcessor : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener,
                                  private juce::AsyncUpdater
{
public:
    MixerReturnAudioProcessor();
    ~MixerReturnAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }

    /** Members currently on the bus this instance has selected, for the editor. */
    int getBusMemberCount() const noexcept;

    /** Post-trim peak of what this instance sent, and of what it output. Editor only. */
    float getSendPeak() const noexcept   { return sendPeak.load (std::memory_order_relaxed); }
    float getOutputPeak() const noexcept { return outputPeak.load (std::memory_order_relaxed); }

private:
    void parameterChanged (const juce::String& id, float newValue) override;
    void handleAsyncUpdate() override;

    void moveToBus (int newBusIndex);
    void leaveBus();

    juce::AudioProcessorValueTreeState apvts;

    int currentBus  = -1;
    int currentSlot = -1;

    int reportedLatency = 0;
    int preparedBlock   = 0;

    juce::AudioBuffer<float> scratch;

    std::atomic<float> sendPeak   { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerReturnAudioProcessor)
};
