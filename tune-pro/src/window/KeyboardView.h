#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

/** One octave of notes, each a real parameter. Click goes Tune, Remove, Bypass; double-click goes back to Tune. The note being hit lights
    coral. A click plays at once and reaches the host once the double-click time has passed, so a double-click writes only its Tune. */
class KeyboardView final : public juce::Component,
                           private juce::Timer
{
public:
    explicit KeyboardView (PluginProcessor&);
    ~KeyboardView() override;

    /** One display frame, from the editor. Repaints only keys that changed. */
    void advance();
    /** Set by the editor when no audio has arrived for a while: nothing is shown as being sung then. */
    void setStale (bool s) noexcept  { stale = s; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Tune to Remove to Bypass to Tune, through the real parameter. */
    void cycleNote (int pitchClass);
    void setNote (int pitchClass, int mode);
    /** Gives the host a click still held for the double-click time (the timer, the window's next look after that time, or closing). */
    void settleClick();
    int noteAt (juce::Point<float>) const noexcept;
    juce::Rectangle<float> keyBounds (int pitchClass) const noexcept;

    /** Window scale, for type sizes. */
    void setTypeScale (float s)  { typeScale = s; repaint(); }

    std::function<void (const juce::String&)> onHint;

    // Tests: the key lit coral (-1 for none) and which notes are drawn as in the scale.
    int getLitKey() const noexcept               { return litShown; }
    std::uint16_t getScaleShown() const noexcept { return scaleShown; }

private:
    int modeOf (int pitchClass) const noexcept;
    int hostModeOf (int pitchClass) const noexcept;
    void firstClick (int pitchClass);
    void resetNote (int pitchClass);
    void dropHold (int pitchClass);
    void timerCallback() override  { settleClick(); }
    static int holdMs() noexcept  { return juce::MouseEvent::getDoubleClickTimeout() + 100; }
    void openMenu (int pitchClass);
    void setHovered (int);
    void sayHint();
    void paintKey (juce::Graphics&, int pitchClass);

    PluginProcessor& processor;
    std::array<std::atomic<float>*, 12> noteParams {};
    std::atomic<float>* bypassParam = nullptr;
    float typeScale = 1.0f;
    int hovered = -1, litShown = -1;
    bool stale = false;
    int modesShown[12] {};
    std::uint16_t scaleShown = 0x0FFF;
    // The click held for the double-click time: its note, the mode the host has and the mode it plays, and when.
    int heldKey = -1, heldFrom = 0, heldTo = 0;
    double heldAtMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyboardView)
};
