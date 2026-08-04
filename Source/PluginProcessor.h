#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/SummingBus.h"
#include "PluginParameters.h"

class MixerReturnAudioProcessor : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener,
                                  private juce::AsyncUpdater,
                                  private juce::Timer
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
    void timerCallback() override;

    void moveToBus (int newBusIndex);
    void leaveBus();

    /** Latency this instance should report, in samples, for the current output mode. */
    int latencyForCurrentMode() const noexcept;

    /** Bus and slot packed into one word so the audio thread can read the pair atomically.

        This must not be a lock. An earlier version guarded it with a try-lock, which was
        fine until instances ran on different threads at once: the lock was process-wide,
        so contending instances lost the try and skipped their whole block — including
        arrive(), which desynchronised the barrier for everyone. Sequential tests cannot
        see that, because a single thread always wins an uncontended try-lock. */
    static constexpr uint32_t unassigned = 0;
    static uint32_t packAssignment (int bus, int slot) noexcept
    {
        return ((uint32_t) (bus + 1) << 16) | (uint32_t) (slot + 1);
    }
    static int busOf (uint32_t a) noexcept  { return (int) (a >> 16) - 1; }
    static int slotOf (uint32_t a) noexcept { return (int) (a & 0xffffu) - 1; }

    juce::AudioProcessorValueTreeState apvts;

    std::atomic<uint32_t> assignment { unassigned };
    int requestedBus = -1;

    int reportedLatency = 0;
    int preparedBlock   = 0;

    /** The block size the host actually calls us with, which is not necessarily the one it
        prepared us for.

        The delay through the two-page bus is one `processBlock` call, so it is this number
        and not `preparedBlock` that must be reported. SuperRack Performer prepares with
        2048 and then processes 256, so reporting `preparedBlock` overstated the latency by
        a factor of eight — 42.7 ms instead of 5.3 ms — and the host believed it.

        Written by the audio thread as a plain relaxed store (no branch, no allocation, no
        message post) and read by the timer on the message thread, which is the only place
        allowed to call setLatencySamples. */
    std::atomic<int> observedBlock { 0 };

    juce::AudioBuffer<float> scratch;

    std::atomic<float> sendPeak   { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerReturnAudioProcessor)
};
