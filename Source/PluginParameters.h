#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/SummingBus.h"

namespace mr::params
{

// Parameter IDs. Changing one of these breaks every saved show, so don't.
inline constexpr const char* busSelect  = "bus";
inline constexpr const char* sendEnable = "sendEnable";
inline constexpr const char* sendMute   = "sendMute";
inline constexpr const char* sendTrim   = "sendTrim";
inline constexpr const char* outputMode = "outputMode";
inline constexpr const char* outputTrim = "outputTrim";

/** What this instance puts on its own output.

    An instance is a sender, a receiver, or both, and the two are independent — which is
    why they are separate parameters rather than one "role" switch. On the reference SQ
    rig, twenty-four instances send and pass their input through, and a twenty-fifth
    outputs the sum. */
enum class OutputMode
{
    input = 0,    // pass the rack's own input through untouched
    busSum,       // replace it with the bus sum
    inputPlusSum  // both, for when the return shares a rack with a source
};

/** Trim range, chosen to mirror the SQ's own direct-out trim so the two read the same.
    The bottom of the range is treated as -inf. */
inline constexpr float trimMinDb = -60.0f;
inline constexpr float trimMaxDb = 10.0f;

inline float trimToGain (float db) noexcept
{
    return db <= trimMinDb ? 0.0f : juce::Decibels::decibelsToGain (db);
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    StringArray busNames;
    for (int i = 1; i <= mr::numBuses; ++i)
        busNames.add ("Bus " + String (i));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { busSelect, 1 }, "Bus", busNames, 0));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { sendEnable, 1 }, "Send", true));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { sendMute, 1 }, "Send Mute", false));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { sendTrim, 1 }, "Send Trim",
        NormalisableRange<float> (trimMinDb, trimMaxDb, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return v <= trimMinDb ? String ("-inf") : String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { outputMode, 1 }, "Output",
        StringArray { "Input", "Bus Sum", "Input + Bus Sum" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { outputTrim, 1 }, "Output Trim",
        NormalisableRange<float> (trimMinDb, trimMaxDb, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return v <= trimMinDb ? String ("-inf") : String (v, 1) + " dB"; })));

    return layout;
}

} // namespace mr::params
