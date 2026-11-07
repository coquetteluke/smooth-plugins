#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. Deep brown, with amber for the sound coming off the tape, cream for the sound going on and for anything you
// can set, and rust only for what Age has worn away.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base  { 0xff2c1c12 };   // the window
        inline const juce::Colour well  { 0xff1d120b };   // the displays
        inline const juce::Colour text  { 0xfff6ead8 };   // cream
        inline const juce::Colour muted { 0xffc6b199 };   // labels
        inline const juce::Colour faint { 0xff937d67 };   // scales and things at rest
        inline const juce::Colour amber { 0xfff2a93b };
        inline const juce::Colour rust  { 0xffd2602f };
        inline const juce::Colour menu  { 0xff3d2a1d };
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
