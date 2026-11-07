#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. Near black and warm white. Colour only ever means a slice: slice 7 is the same colour on the waveform,
// on the ribbon, on its key and on the controls that edit it. Nothing else in the window gets a colour of its own.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base   { 0xff141211 };   // the window
        inline const juce::Colour well   { 0xff0b0a09 };   // behind the waveform
        inline const juce::Colour raised { 0xff24211e };   // fields and buttons
        inline const juce::Colour hover  { 0xff2f2b27 };
        inline const juce::Colour key    { 0xff2c2825 };   // a white key with no slice on it
        inline const juce::Colour keyTop { 0xff121110 };   // a black key with no slice on it
        inline const juce::Colour text   { 0xfff6f0e8 };
        inline const juce::Colour muted  { 0xffb4aba1 };   // labels
        inline const juce::Colour faint  { 0xff7c746c };   // scales and things at rest
        inline const juce::Colour ink    { 0xff17110d };   // type on a coloured or light fill
        inline const juce::Colour menu   { 0xff2a2623 };
    }

    /** Eight warm colours in an order that keeps neighbours apart. They repeat, and every slice carries its number. */
    juce::Colour sliceColour (int slot);
    /** The same colour with more light in it: a slice that is sounding. */
    juce::Colour sliceColourLit (int slot);

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
