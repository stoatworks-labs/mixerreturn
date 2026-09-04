#include "PluginProcessor.h"

#include "Diag/Diag.h"
#include "PluginEditor.h"

#include <mutex>

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
    stopTimer();

    apvts.removeParameterListener (mr::params::busSelect, this);
    apvts.removeParameterListener (mr::params::outputMode, this);

    cancelPendingUpdate();
    leaveBus();
}

void MixerReturnAudioProcessor::parameterChanged (const juce::String&, float)
{
    triggerAsyncUpdate();
}

int MixerReturnAudioProcessor::latencyForCurrentMode() const noexcept
{
    const auto mode = (mr::params::OutputMode) (int) *apvts.getRawParameterValue (mr::params::outputMode);

    if (mode == mr::params::OutputMode::input)
        return 0;

    // One processBlock call of delay — the actual call length, not the maximum the host
    // prepared for. Until a block has been seen, preparedBlock is the best guess available.
    const auto actual = observedBlock.load (std::memory_order_relaxed);
    return actual > 0 ? actual : preparedBlock;
}

void MixerReturnAudioProcessor::handleAsyncUpdate()
{
    const auto wanted = (int) *apvts.getRawParameterValue (mr::params::busSelect);

    if (wanted != requestedBus)
        moveToBus (wanted);

    const auto wantedLatency = latencyForCurrentMode();

    if (wantedLatency != reportedLatency)
    {
        reportedLatency = wantedLatency;
        setLatencySamples (reportedLatency);
    }
}

void MixerReturnAudioProcessor::timerCallback()
{
    // The host's actual block size is only knowable from inside processBlock, and
    // setLatencySamples must not be called from there. Poll instead: this settles one tick
    // after audio starts and then never changes again unless the host changes block size.
    if (latencyForCurrentMode() != reportedLatency)
        triggerAsyncUpdate();
}

void MixerReturnAudioProcessor::moveToBus (int newBusIndex)
{
    // Stop the audio thread using the old slot before releasing it. It may still be
    // part-way through a block that began before this store; that is harmless, because a
    // released slot keeps its buffers and readers skip it once it is inactive.
    leaveBus();

    requestedBus = newBusIndex;

    if (newBusIndex < 0)
        return;

    auto& bus = mr::BusRegistry::get().bus (newBusIndex);
    const auto slot = bus.acquireSlot (juce::jmax (preparedBlock, 1), mr::maxChannels);

    if (slot < 0)
    {
        CP_LOG_ERROR ("Bus " + juce::String (newBusIndex + 1) + " is full; this instance is not connected");
        return;
    }

    assignment.store (packAssignment (newBusIndex, slot), std::memory_order_release);
    CP_LOG_INFO ("Joined bus " + juce::String (newBusIndex + 1) + " in slot " + juce::String (slot));
}

void MixerReturnAudioProcessor::leaveBus()
{
    const auto previous = assignment.exchange (unassigned, std::memory_order_acq_rel);

    if (previous != unassigned)
        mr::BusRegistry::get().bus (busOf (previous)).releaseSlot (slotOf (previous));
}

void MixerReturnAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedBlock = samplesPerBlock;
    scratch.setSize (mr::maxChannels, samplesPerBlock, false, true, false);

    // 15 ms. Long enough that a mute is a fade rather than a click, short enough
    // that it is not a fade anyone would call one — an operator hitting mute
    // expects it gone, not ducked. Trim rides on the same constant so a fader
    // move and a mute behave the same way.
    const double rampSeconds = 0.015;
    sendSmoothed.reset (sampleRate, rampSeconds);
    outSmoothed.reset (sampleRate, rampSeconds);

    // Start settled at whatever the parameters already say, so loading a session
    // with the trim down does not fade up from silence on the first block.
    const bool sending = *apvts.getRawParameterValue (mr::params::sendEnable) > 0.5f
                      && *apvts.getRawParameterValue (mr::params::sendMute) < 0.5f;
    sendSmoothed.setCurrentAndTargetValue (
        sending ? mr::params::trimToGain (*apvts.getRawParameterValue (mr::params::sendTrim)) : 0.0f);
    outSmoothed.setCurrentAndTargetValue (
        mr::params::trimToGain (*apvts.getRawParameterValue (mr::params::outputTrim)));

    // Deliberately *not* clearing observedBlock here.
    //
    // Reporting a latency change makes the host re-prepare the plugin, so clearing it
    // created a feedback loop with SuperRack: prepare(2048) -> report 2048 -> process(256)
    // -> timer reports 256 -> host re-prepares -> forget -> report 2048 again, ten times a
    // second, churning a bus slot on every pass. Carrying the previous run's block size
    // over means the second prepare already reports the right number, setLatencySamples
    // sees no change, and the host stops being told anything.
    moveToBus ((int) *apvts.getRawParameterValue (mr::params::busSelect));

    reportedLatency = latencyForCurrentMode();
    setLatencySamples (reportedLatency);

    startTimerHz (10);
}

void MixerReturnAudioProcessor::releaseResources()
{
    stopTimer();
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

    // What the delay through the bus actually is. A plain relaxed store: the timer on the
    // message thread is what turns this into a latency report.
    observedBlock.store (numSamples, std::memory_order_relaxed);

    for (int ch = buffer.getNumChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const bool sending = *apvts.getRawParameterValue (mr::params::sendEnable) > 0.5f
                      && *apvts.getRawParameterValue (mr::params::sendMute) < 0.5f;

    // Mute folded into the send gain: a mute is a ramp to zero, not a switch
    // between writing and clearing at a block boundary.
    sendSmoothed.setTargetValue (
        sending ? mr::params::trimToGain (*apvts.getRawParameterValue (mr::params::sendTrim)) : 0.0f);
    outSmoothed.setTargetValue (
        mr::params::trimToGain (*apvts.getRawParameterValue (mr::params::outputTrim)));

    // Both ends of this block's ramp, taken once so every channel gets the same
    // one — advancing the smoother per channel would ramp the first channel and
    // hand the rest a settled value, which is a channel-dependent gain.
    const auto sendStart = sendSmoothed.getCurrentValue();
    sendSmoothed.skip (numSamples);
    const auto sendEnd = sendSmoothed.getCurrentValue();

    const auto outStart = outSmoothed.getCurrentValue();
    outSmoothed.skip (numSamples);
    const auto outEnd = outSmoothed.getCurrentValue();

    const auto mode = (mr::params::OutputMode) (int) *apvts.getRawParameterValue (mr::params::outputMode);

    float sendMagnitude = 0.0f;

    const auto current = assignment.load (std::memory_order_acquire);

    if (current != unassigned)
    {
        auto& bus = mr::BusRegistry::get().bus (busOf (current));
        const auto slot = slotOf (current);

        // Still writing while the ramp is running, even into a mute — writing at
        // a gain of zero IS clearing the slot, so the rule that a muted member
        // must not simply skip writing still holds. clearSlot is only taken once
        // the ramp has arrived at silence, where it is the cheaper way to say the
        // same thing.
        const bool silent = sendStart == 0.0f && sendEnd == 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (silent)
            {
                // Not simply "skip writing": a muted member's previous block would
                // otherwise stay in the sum forever.
                bus.clearSlot (slot, ch, numSamples);
            }
            else
            {
                bus.writeSlot (slot, ch, buffer.getReadPointer (ch), numSamples,
                               sendStart, sendEnd);
                sendMagnitude = juce::jmax (sendMagnitude,
                                            buffer.getMagnitude (ch, 0, numSamples)
                                                * juce::jmax (sendStart, sendEnd));
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
        bus.arrive (slotOf (current));
    }
    else if (mode == mr::params::OutputMode::busSum)
    {
        // Not on a bus — emit silence rather than leaking this rack's own input to a
        // destination expecting only the sum.
        buffer.clear();
    }

    buffer.applyGainRamp (0, numSamples, outStart, outEnd);

    sendPeak.store (sendMagnitude, std::memory_order_relaxed);
    outputPeak.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);
}

void MixerReturnAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // A bypassed instance still holds a slot, so it must still arrive or the barrier
    // never completes and every other member freezes one block behind forever.
    const auto numSamples  = buffer.getNumSamples();
    const auto numChannels = juce::jmin (buffer.getNumChannels(), mr::maxChannels);

    observedBlock.store (numSamples, std::memory_order_relaxed);

    const auto current = assignment.load (std::memory_order_acquire);

    if (current != unassigned)
    {
        auto& bus = mr::BusRegistry::get().bus (busOf (current));

        for (int ch = 0; ch < numChannels; ++ch)
            bus.clearSlot (slotOf (current), ch, numSamples);

        bus.arrive (slotOf (current));
    }

    sendPeak.store (0.0f, std::memory_order_relaxed);
    outputPeak.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);
}

int MixerReturnAudioProcessor::getBusMemberCount() const noexcept
{
    const auto current = assignment.load (std::memory_order_acquire);

    if (current == unassigned)
        return 0;

    return mr::BusRegistry::get().bus (busOf (current)).getMemberCount();
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
