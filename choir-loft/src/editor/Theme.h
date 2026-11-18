#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// The window's look: plum, with warm white for the lead and one colour per voice. Colour only ever means "this singer".
// Everything that is not a singer stays neutral. Things are separated by space, not by lines.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base   { 0xff1d1021 };   // the window
        inline const juce::Colour well   { 0xff150a19 };   // behind the pitch lines
        inline const juce::Colour floor  { 0xff27152d };   // the stage
        inline const juce::Colour text   { 0xfffaf0e6 };   // warm white, and the lead
        inline const juce::Colour muted  { 0xffbcaabb };   // labels
        inline const juce::Colour faint  { 0xff8c7a90 };   // scales
        inline const juce::Colour ink    { 0xff1a0c1e };   // type on a coloured singer

        inline const juce::Colour voice[8] = { juce::Colour (0xffff7fa0),    // rose
                                               juce::Colour (0xffffba70),    // apricot
                                               juce::Colour (0xffc8a2ff),    // orchid
                                               juce::Colour (0xff76dcc7),    // sea glass
                                               juce::Colour (0xff7cc0ff),    // sky
                                               juce::Colour (0xffb9e36c),    // lime
                                               juce::Colour (0xfff6e27f),    // butter
                                               juce::Colour (0xffee8fe4) };  // magenta
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
