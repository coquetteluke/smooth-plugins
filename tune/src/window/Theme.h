#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. Graphite, with one colour. Lime only ever means the corrected voice or the amount of correction,
// and the one control that sets it. Everything else is bone on graphite.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base  { 0xff2c2e32 };   // the window
        inline const juce::Colour well  { 0xff1d1f22 };   // the display
        inline const juce::Colour text  { 0xfff3f3ee };
        inline const juce::Colour muted { 0xffaeb1b5 };   // labels
        inline const juce::Colour faint { 0xff7f8388 };   // scales and things at rest
        inline const juce::Colour sung  { 0xffe6e5dc };   // the voice as it came in
        inline const juce::Colour lime  { 0xffc9f23a };   // the voice as it goes out
        inline const juce::Colour menu  { 0xff36393e };
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
