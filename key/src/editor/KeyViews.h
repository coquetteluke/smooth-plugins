#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

#include <array>
#include <functional>

namespace keyui
{
    // Plain text that only repaints itself when it changes, and never the panel behind it.
    class Readout final : public juce::Component,
                          public Hinted
    {
    public:
        Readout (int weightToUse, float sizeToUse, juce::Colour colourToUse, Align alignToUse = Align::left);

        // True if anything changed.
        bool set (const juce::String& newText, juce::Colour newColour);
        bool setText (const juce::String& newText)  { return set (newText, colour); }
        void setDot (bool shouldShow);

        const juce::String& getText() const noexcept  { return text; }
        void paint (juce::Graphics&) override;

    private:
        juce::String text;
        const int weight;
        const float size;
        juce::Colour colour;
        const Align align;
        bool dot = false;
    };

    // The reading itself: the key, then the scale under it, as large as they fit, then the relative key beside the scale.
    class HeroView final : public juce::Component,
                           public Hinted,
                           private juce::Timer
    {
    public:
        struct Content
        {
            juce::String key, scale, otherName, relative;
            bool hasKey = false, dimmed = true, manual = false;
        };

        std::function<void (const juce::String&)> onTyped;
        std::function<void()> onReset;
        juce::String listenHint, manualHint;

        HeroView();
        ~HeroView() override;

        bool set (const Content&);
        juce::Rectangle<int> swapBounds() const;    // where the swap button goes, in this view
        juce::Rectangle<int> keyLetterBounds() const;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        void startTyping();

    private:
        void timerCallback() override;

        Content content;
        float keySize = 100.0f, scaleSize = 30.0f, keyBaseline = 70.0f, scaleBaseline = 100.0f, tallest = 0.7f;
        TypeBox typeBox;
    };

    // The circle of fifths. Each key's ink is how likely it looks right now. The key on show is red.
    // Drawn into one image, and only the keys that changed are redrawn.
    class CircleView final : public juce::Component,
                             public Hinted
    {
    public:
        static constexpr int levels = 10;

        std::function<void (int key, bool minor)> onPick;
        std::function<void()> onReset;
        juce::String listenHint, manualHint;

        CircleView();

        // level is 0 to levels for each of 24 keys, 0 to 11 major from C, then minor. shown is the red one, or -1.
        bool set (const std::array<juce::uint8, 24>& level, int shown, bool shownDimmed, bool manual);

        juce::Point<float> centre() const noexcept;
        float holeRadius() const noexcept;

        // Which key is at a point, as 0 to 23, or -1.
        int keyAt (juce::Point<float>) const noexcept;
        juce::Point<float> pointForKey (int key) const noexcept;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

    private:
        static int spokeOf (int key) noexcept;
        void drawKey (juce::Graphics&, int key);
        void markDirty (int key);
        void setHovered (int key);

        std::array<juce::uint8, 24> shownLevels {};
        std::array<bool, 24> dirty {};
        std::array<juce::Path, 24> wedges, clears;
        std::array<juce::Rectangle<int>, 24> wedgeBounds;
        std::array<juce::Point<float>, 24> labelSpots;
        int redKey = -1, hoveredKey = -1;
        bool redDimmed = false, manualMode = false;
        juce::Image layer;
        float layerScale = 0.0f;
        bool layerStale = true;
    };

    // One octave, labelled, with the scale marked and the home note red. Dots show the notes sounding now.
    class KeyboardView final : public juce::Component,
                               public Hinted
    {
    public:
        std::function<void (int note)> onPick;
        juce::String listenHint, manualHint;

        KeyboardView();

        bool setScale (int key, bool minor, bool dimmed, bool manual);
        bool setNotes (const std::array<float, 12>& levels);

        int noteAt (juce::Point<float>) const noexcept;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        bool inScale (int note) const noexcept;
        juce::Rectangle<float> keyBounds (int note) const noexcept;
        juce::Point<float> dotSpot (int note) const noexcept;
        void rebuild (float scale);

        int rootKey = -1;
        bool minorScale = false, dim = false, manualMode = false;
        std::array<juce::uint8, 12> dots {};
        juce::Image layer;
        float layerScale = 0.0f;
        bool layerStale = true;
    };
}
