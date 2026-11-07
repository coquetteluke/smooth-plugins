#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. Black, with one colour. Hot orange only ever means the sound as it goes out, or the two controls that shape it.
// White means the sound as it came in, and anything to read.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour black { 0xff000000 };   // the window, with nothing behind it
        inline const juce::Colour well  { 0xff161616 };   // empty meter lanes and buttons
        inline const juce::Colour well2 { 0xff262626 };   // the same under the mouse
        inline const juce::Colour track { 0xff2e2e2e };   // the bar under a slider
        inline const juce::Colour track2{ 0xff444444 };   // the same under the mouse
        inline const juce::Colour lift  { 0xff141414 };   // the part of the display a control under the mouse works on
        inline const juce::Colour grid  { 0xff1a1a1a };   // scale lines
        inline const juce::Colour grid0 { 0xff3a3a3a };   // 0 dB
        inline const juce::Colour ghost { 0xff1f1f1f };   // the hit before this one
        inline const juce::Colour text  { 0xffffffff };
        inline const juce::Colour muted { 0xff9c9c9c };   // labels
        inline const juce::Colour faint { 0xff757575 };   // scales
        inline const juce::Colour hot   { 0xffff5a00 };   // the sound as it goes out
        inline const juce::Colour dimHot{ 0xffb24000 };   // the same, further back in time, or only an example
        inline const juce::Colour ember { 0xff4d1b00 };   // what the settings took away
        inline const juce::Colour menu  { 0xff1c1c1c };
    }

    enum class Weight { regular, medium };

    /** The embedded fonts, shared between open windows and freed with the last one. */
    struct Assets
    {
        Assets();
        juce::Typeface::Ptr regular, medium;
    };

    /** Size is the type size in pixels, not JUCE's taller line height. Tabular keeps digits the same width, so a live number does not shake. */
    juce::Font font (Weight, float size, bool tabular = false);

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
