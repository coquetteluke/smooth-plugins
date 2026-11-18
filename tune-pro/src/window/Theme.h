#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. Ink for everything at rest, white for the voice as sung and for the keys, and coral only ever for what the tuning is doing:
// the corrected line, the note being hit, and the amount it moved.
namespace tuneui
{
    namespace tone
    {
        inline const juce::Colour ink       { 0xff14152f };   // the window
        inline const juce::Colour inkDeep   { 0xff101127 };   // notes outside the scale
        inline const juce::Colour laneNatural { 0xff1c1e3e }; // white-key notes in the scale
        inline const juce::Colour laneSharp { 0xff181a35 };   // black-key notes in the scale
        inline const juce::Colour raised    { 0xff24264a };   // anything a control sits on
        inline const juce::Colour white     { 0xfffcfcff };
        inline const juce::Colour muted     { 0xffaaadcb };
        inline const juce::Colour faint     { 0xff7c7fa6 };
        inline const juce::Colour coral     { 0xffff7365 };
        inline const juce::Colour keyWhite  { 0xffeaeaf3 };
        inline const juce::Colour keyBlack  { 0xff353862 };
        inline const juce::Colour keyWhiteOff { 0xff41446c }; // white key outside the scale
        inline const juce::Colour keyBlackOff { 0xff232547 }; // black key outside the scale
    }

    enum class Weight { regular, medium };

    /** The embedded fonts, shared between open windows and freed with the last one. */
    struct Assets
    {
        Assets();
        juce::Typeface::Ptr regular, medium;
    };

    /** Size is the type size in pixels, not JUCE's taller line height. */
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
