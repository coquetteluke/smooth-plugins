#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look: dark sea green and pale mint, type in Work Sans. Pale mint is the left read heads and anything you can set.
// Aqua is the right read heads. Coral only ever means over 0 dB. Things are separated by space, not by lines.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base   { 0xff0f3a33 };   // the window
        inline const juce::Colour deep   { 0xff0a2c27 };   // behind a value being typed, and the menu
        inline const juce::Colour flange { 0xff174e46 };   // a reel at rest
        inline const juce::Colour pack   { 0xff2b8676 };   // the wound tape on a reel, which is the level
        inline const juce::Colour bars   { 0xff174e46 };   // what is on the tape
        inline const juce::Colour track  { 0xff2c6a5f };   // a ruler's empty part
        inline const juce::Colour mint   { 0xffd6f7e7 };   // left read heads, values
        inline const juce::Colour aqua   { 0xff62d0bb };   // right read heads
        inline const juce::Colour muted  { 0xff9ac8bb };   // labels
        inline const juce::Colour faint  { 0xff6ba193 };   // scales, quiet notes
        inline const juce::Colour rest   { 0xff5d8f83 };   // read heads while bypassed
        inline const juce::Colour hot    { 0xffff9f85 };   // over 0 dB
    }

    enum class Weight { light, medium };

    /** The embedded fonts, shared between open windows and freed with the last one. */
    struct Assets
    {
        Assets();
        juce::Typeface::Ptr light, medium;
    };

    /** Size is the type size in pixels, not JUCE's taller line height. */
    juce::Font font (Weight, float size);

    /** A control that can explain itself in the hint line. */
    struct Hinted
    {
        virtual ~Hinted() = default;
        juce::String hint;
    };

    /** The minus sign Work Sans draws to match its digits. */
    juce::String minusSign();

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
