#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. A real deep green, not a black with a green tint. Acid yellow only ever means the alter ego: the dot, the
// right half of the mirror, the chosen Character, and the sliders that change the voice. Your own voice is always the pale green.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base  { 0xff0e4534 };   // the window
        inline const juce::Colour well  { 0xff08291f };   // the pad and the mirror
        inline const juce::Colour text  { 0xfff2f7e6 };
        inline const juce::Colour muted { 0xffa8c9b6 };   // labels
        inline const juce::Colour faint { 0xff75a08b };   // scales and things at rest
        inline const juce::Colour you   { 0xffcfe6d3 };   // the voice as it came in
        inline const juce::Colour acid  { 0xfff4f832 };   // the voice as it goes out
        inline const juce::Colour menu  { 0xff14553f };
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

    /** With a real minus sign, and always a sign unless it is zero. */
    juce::String signedText (float value, int places, const char* unit);

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
