#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

/** One octave of notes, each a real parameter. Click goes Tune, Remove, Bypass. The note being hit lights coral. */
class KeyboardView final : public juce::Component
{
public:
    explicit KeyboardView (PluginProcessor&);

    /** One display frame, from the editor. Repaints only keys that changed. */
    void advance();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Tune to Remove to Bypass to Tune, through the real parameter. */
    void cycleNote (int pitchClass);
    void setNote (int pitchClass, int mode);
    int noteAt (juce::Point<float>) const noexcept;
    juce::Rectangle<float> keyBounds (int pitchClass) const noexcept;

    /** Window scale, for type sizes. */
    void setTypeScale (float s)  { typeScale = s; repaint(); }

    std::function<void (const juce::String&)> onHint;

private:
    int modeOf (int pitchClass) const noexcept;
    void openMenu (int pitchClass);
    void setHovered (int);
    void paintKey (juce::Graphics&, int pitchClass);

    PluginProcessor& processor;
    std::array<std::atomic<float>*, 12> noteParams {};
    std::atomic<float>* bypassParam = nullptr;
    float typeScale = 1.0f;
    int hovered = -1, litShown = -1;
    int modesShown[12] {};
    std::uint16_t scaleShown = 0x0FFF;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyboardView)
};
