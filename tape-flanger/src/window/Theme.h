#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. Cold steel with one colour. Violet only ever means sound that is really coming out, or the thing being
// touched. Steel is the comb as the settings predict it. Things are separated by space, not lines.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base       { 0xff222a34 };  // the window
        inline const juce::Colour raised     { 0xff2d3743 };  // buttons and fields
        inline const juce::Colour raisedLit  { 0xff384451 };
        inline const juce::Colour well       { 0xff0e1218 };  // the field at a notch
        inline const juce::Colour text       { 0xfff0f3f7 };
        inline const juce::Colour muted      { 0xffa2aebc };  // labels
        inline const juce::Colour faint      { 0xff75828f };  // scales
        inline const juce::Colour track      { 0xff3d4957 };
        inline const juce::Colour steel      { 0xffc4cfdb };  // filled part of a control
        inline const juce::Colour violet     { 0xff9f80ff };
        inline const juce::Colour violetPale { 0xffdcd2ff };
        inline const juce::Colour violetDeep { 0xff4a35a8 };
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

    /** Something that can explain itself in the hint line. */
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
