#include "PluginProcessor.h"

#include "Diag/Diag.h"
#include "PluginEditor.h"

#include <mutex>

namespace
{
/** Bus membership changes are message-thread work: acquiring a slot may allocate its
    page buffers, and telling the host about a latency change must not happen from the
    audio callback. The audio thread only ever reads the resulting assignment. */
juce::SpinLock& busAssignmentLock()
{
    static juce::SpinLock lock;
    return lock;
}
} // namespace

MixerReturnAudioProcessor::MixerReturnAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "state", mr::params::createLayout())
{
    // Once per process, not once per instance: diag::init resets the log state and writes
    // a banner, and this plugin is deliberately loaded twenty-five times over.
    static std::once_flag diagOnce;
    std::call_once (diagOnce, []
    {
        cp::diag::init ({ "MixerReturn", "MIXERRETURN", JucePlugin_VersionString,
                          cp::diag::Level::info,
                          // Never true here: this runs inside someone else's process and a
                          // process-wide handler would intercept faults that are not ours.
                          false });
    });

    apvts.addParameterListener (mr::params::busSelect, this);
    apvts.addParameterListener (mr::params::outputMode, this);
}

MixerReturnAudioProcessor::~MixerReturnAudioProcessor()
{
    apvts.removeParameterListener (mr::params::busSelect, this);
    apvts.removeParameterListener (mr::params::outputMode, this);

    cancelPendingUpdate();
    leaveBus();
}

void MixerReturnAudioProcessor::parameterChanged (const juce::String&, float)
{
    triggerAsyncUpdate();
}

void MixerReturnAudioProcessor::handleAsyncUpdate()
{
    const auto wanted = (int) *apvts.getRawParameterValue (mr::params::busSelect);

    if (wanted != currentBus)
        moveToBus (wanted);

    const auto mode = (mr::params::OutputMode) (int) *apvts.getRawParameterValue (mr::params::outputMode);
    const auto wantedLatency = (mode == mr::params::OutputMode::input) ? 0 : preparedBlock;

    if (wantedLatency != reportedLatency)
    {
        reportedLatency = wantedLatency;
        setLatencySamples (reportedLatency);
    }
}

void MixerReturnAudioProcessor::moveToBus (int newBusIndex)
{
    const juce::SpinLock::ScopedLockType lock (busAssignmentLock());

    if (currentBus >= 0 && currentSlot >= 0)
        mr::BusRegistry::get().bus (currentBus).releaseSlot (currentSlot);

    currentBus  = newBusIndex;
    currentSlot = -1;

    if (currentBus < 0)
        return;

    auto& bus = mr::BusRegistry::get().bus (currentBus);
    bus.prepare (juce::jmax (preparedBlock, 1), mr::maxChannels);
    currentSlot = bus.acquireSlot();

    if (currentSlot < 0)
        CP_LOG_ERROR ("Bus " + juce::String (currentBus + 1) + " is full; this instance is not connected");
    else
        CP_LOG_INFO ("Joined bus " + juce::String (currentBus + 1) + " in slot " + juce::String (currentSlot));
}

void MixerReturnAudioProcessor::leaveBus()
{
    const juce::SpinLock::ScopedLockType lock (busAssignmentLock());

    if (currentBus >= 0 && currentSlot >= 0)
        mr::BusRegistry::get().bus (currentBus).releaseSlot (currentSlot);

    currentBus  = -1;
    currentSlot = -1;
}

void MixerReturnAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate);

    preparedBlock = samplesPerBlock;
    scratch.setSize (mr::maxChannels, samplesPerBlock, false, true, false);

    moveToBus ((int) *apvts.getRawParameterValue (mr::params::busSelect));

    const auto mode = (mr::params::OutputMode) (int) *apvts.getRawParameterValue (mr::params::outputMode);
    reportedLatency = (mode == mr::params::OutputMode::input) ? 0 : preparedBlock;
    setLatencySamples (reportedLatency);
}

void MixerReturnAudioProcessor::releaseResources()
{
    leaveBus();
}

bool MixerReturnAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void MixerReturnAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto numSamples  = buffer.getNumSamples();
    const auto numChannels = juce::jmin (buffer.getNumChannels(), mr::maxChannels);

    for (int ch = buffer.getNumChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const bool sending = *apvts.getRawParameterValue (mr::params::sendEnable) > 0.5f
                      && *apvts.getRawParameterValue (mr::params::sendMute) < 0.5f;

    const auto sendGain = mr::params::trimToGain (*apvts.getRawParameterValue (mr::params::sendTrim));
    const auto outGain  = mr::params::trimToGain (*apvts.getRawParameterValue (mr::params::outputTrim));
    const auto mode     = (mr::params::OutputMode) (int) *apvts.getRawParameterValue (mr::params::outputMode);

    float sendMagnitude = 0.0f;

    {
        const juce::SpinLock::ScopedTryLockType lock (busAssignmentLock());

        if (lock.isLocked() && currentSlot >= 0 && currentBus >= 0)
        {
            auto& bus = mr::BusRegistry::get().bus (currentBus);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                if (sending)
                {
                    bus.writeSlot (currentSlot, ch, buffer.getReadPointer (ch), numSamples, sendGain);
                    sendMagnitude = juce::jmax (sendMagnitude,
                                                buffer.getMagnitude (ch, 0, numSamples) * sendGain);
                }
                else
                {
                    // Not simply "skip writing": a muted member's previous block would
                    // otherwise stay in the sum forever.
                    bus.clearSlot (currentSlot, ch, numSamples);
                }
            }

            if (mode != mr::params::OutputMode::input)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* sum = scratch.getWritePointer (ch);
                    bus.readSum (sum, ch, numSamples);

                    if (mode == mr::params::OutputMode::busSum)
                        buffer.copyFrom (ch, 0, sum, numSamples);
                    else
                        buffer.addFrom (ch, 0, sum, numSamples, 1.0f);
                }
            }

            // Last thing in the block, always: this is what flips the pages.
            bus.arrive();
        }
        else if (mode == mr::params::OutputMode::busSum)
        {
            // Unable to reach the bus this block — emit silence rather than leaking this
            // rack's own input to a destination expecting only the sum.
            buffer.clear();
        }
    }

    buffer.applyGain (outGain);

    sendPeak.store (sendMagnitude, std::memory_order_relaxed);
    outputPeak.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);
}

void MixerReturnAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // A bypassed instance still holds a slot, so it must still arrive or the barrier
    // never completes and every other member freezes one block behind forever.
    const auto numSamples  = buffer.getNumSamples();
    const auto numChannels = juce::jmin (buffer.getNumChannels(), mr::maxChannels);

    const juce::SpinLock::ScopedTryLockType lock (busAssignmentLock());

    if (lock.isLocked() && currentSlot >= 0 && currentBus >= 0)
    {
        auto& bus = mr::BusRegistry::get().bus (currentBus);

        for (int ch = 0; ch < numChannels; ++ch)
            bus.clearSlot (currentSlot, ch, numSamples);

        bus.arrive();
    }

    sendPeak.store (0.0f, std::memory_order_relaxed);
    outputPeak.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);
}

int MixerReturnAudioProcessor::getBusMemberCount() const noexcept
{
    if (currentBus < 0)
        return 0;

    return mr::BusRegistry::get().bus (currentBus).getMemberCount();
}

juce::AudioProcessorEditor* MixerReturnAudioProcessor::createEditor()
{
    return new MixerReturnAudioProcessorEditor (*this);
}

void MixerReturnAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MixerReturnAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

    triggerAsyncUpdate();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MixerReturnAudioProcessor();
}
