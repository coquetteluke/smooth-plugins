#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

// The window's look. True black, hot magenta for the voice coming out and the tuning controls, and one flat colour for each module.
namespace fxui
{
    namespace tone
    {
        inline const juce::Colour black   { 0xff000000 };
        inline const juce::Colour raised  { 0xff171717 };   // anything a control sits on
        inline const juce::Colour lifted  { 0xff262626 };   // the same, under the mouse
        inline const juce::Colour white   { 0xffffffff };
        inline const juce::Colour muted   { 0xffa6a6a6 };
        inline const juce::Colour faint   { 0xff6f6f6f };
        inline const juce::Colour magenta { 0xffff1f8f };
    }

    struct Module
    {
        const char* name;
        const char* onId;
        juce::Colour colour;
        const char* about;
    };

    static constexpr int numModules = 11;

    /** All eleven in signal order, left to right. */
    const std::array<Module, numModules>& modules();

    /** The panel behind a module: black with a little of its colour. */
    juce::Colour panelTint (juce::Colour moduleColour, bool on);

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
