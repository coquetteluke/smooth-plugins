#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// The window's look: slate, with one sky blue. White is the lead, sky blue is a double, and nothing else is coloured.
// Things are separated by space, not by lines.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base    { 0xff2a3440 };   // the window
        inline const juce::Colour floor   { 0xff35424f };   // the stage, seen from above
        inline const juce::Colour ring    { 0xff4a5a69 };   // the lines painted on the stage
        inline const juce::Colour well    { 0xff222b35 };   // behind a fader, and a box being typed in
        inline const juce::Colour lift    { 0xff3b4957 };   // a resting button
        inline const juce::Colour text    { 0xfff2f6f9 };   // type, and the lead
        inline const juce::Colour muted   { 0xffb4c1cc };   // labels
        inline const juce::Colour faint   { 0xff8f9fad };   // scales
        inline const juce::Colour ink     { 0xff17212b };   // type on a sky blue or white shape
        inline const juce::Colour sky     { 0xff7cc9f5 };   // a double
        inline const juce::Colour pool    { 0xff5ab6ef };   // the light under a double
        inline const juce::Colour skyDeep { 0xff4f9fd0 };   // a double's shoulders
        inline const juce::Colour skyPale { 0xffcdeafb };   // a double's head
        inline const juce::Colour leadDeep { 0xffc6d3dd };  // the lead's shoulders
    }

    enum class Weight { regular, medium };

    /** The embedded fonts, shared between open windows and freed with the last one. */
    struct Assets
    {
        Assets();
        juce::Typeface::Ptr regular, medium;
    };

    /** Size is the type size in pixels, not JUCE's taller line height. Large type gets a little tighter. */
    juce::Font font (Weight, float size);

    /** A control that can explain itself in the hint line. */
    struct Hinted
    {
        virtual ~Hinted() = default;
        juce::String hint;
    };

    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();

        juce::Font getPopupMenuFont() override;
        void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
        void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                                const juce::Drawable* icon, const juce::Colour* textColour) override;
        void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
        juce::Font getLabelFont (juce::Label&) override;
        void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
        void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    };
}
