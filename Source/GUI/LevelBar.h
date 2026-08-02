#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** A horizontal peak bar with a slow decay, so a transient stays readable long enough
    to see across a room. Fed from the message thread; holds no audio state. */
class LevelBar : public juce::Component
{
public:
    void setLevel (float linearPeak)
    {
        const auto db = juce::Decibels::gainToDecibels (linearPeak, floorDb);
        displayed = db > displayed ? db : displayed + (db - displayed) * 0.25f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r, 2.0f);

        const auto proportion = juce::jlimit (0.0f, 1.0f, (displayed - floorDb) / (0.0f - floorDb));

        if (proportion > 0.001f)
        {
            g.setColour (displayed > -3.0f ? juce::Colours::orangered
                                           : juce::Colour (0xff4fc3f7));
            g.fillRoundedRectangle (r.withWidth (r.getWidth() * proportion), 2.0f);
        }

        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }

private:
    static constexpr float floorDb = -60.0f;
    float displayed = floorDb;
};
