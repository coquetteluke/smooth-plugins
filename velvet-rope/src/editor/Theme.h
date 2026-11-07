#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// The window's look: lapis glaze with gold repairs. Gold always and only means "what is being taken out",
// so anything that is not reduction (or the controls that decide it) stays bone or blue.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour bone        { 0xfff4eedd };
        inline const juce::Colour boneSoft    { 0xffc9d0ea };   // labels
        inline const juce::Colour boneFaint   { 0xff98a5d6 };   // scales and quiet text
        inline const juce::Colour lapis       { 0xff15216a };   // glaze, mid
        inline const juce::Colour lapisDark   { 0xff0f1953 };   // glaze, dark
        inline const juce::Colour lapisLight  { 0xff1e2f8c };   // glaze, light runs
        inline const juce::Colour raised      { 0xff2c43a8 };   // lit edge of a dial, hovered menu row
        inline const juce::Colour well        { 0xff060b2a };   // the graph's background
        inline const juce::Colour voiceTop    { 0xffc4cffa };   // the voice where it is loud
        inline const juce::Colour cobalt      { 0xff3a5ce6 };   // the voice's body
        inline const juce::Colour voiceLow    { 0xff14226c };   // the voice at the floor
        inline const juce::Colour gold        { 0xfff4c34f };
        inline const juce::Colour goldHot     { 0xfffff2c6 };
        inline const juce::Colour copper      { 0xfff0935f };   // the second channel, when channels are treated apart
    }

    enum class Weight { regular, semibold, bold };

    /** Fonts and the glaze texture, shared between open windows and freed with the last one. */
    struct Assets
    {
        Assets();

        juce::Typeface::Ptr regular, semibold, bold;
        juce::Image glaze;
        juce::Path flecks;
    };

    juce::Font font (Weight, float height);

    /** A control that can explain itself in the hint line. */
    struct Hinted
    {
        virtual ~Hinted() = default;
        juce::String hint;
    };

    /** Writes a parameter in real units from the message thread. The caller handles the gesture. */
    inline void setReal (juce::RangedAudioParameter& p, float value)
    {
        p.setValueNotifyingHost (p.convertTo0to1 (value));
    }

    inline float getReal (const juce::RangedAudioParameter& p)
    {
        return p.convertFrom0to1 (p.getValue());
    }

    /** One value change, with a begin and end so automation in Touch or Latch records it. */
    inline void setAsGesture (juce::RangedAudioParameter& p, float value)
    {
        p.beginChangeGesture();
        setReal (p, value);
        p.endChangeGesture();
    }

    juce::String hertzText (float hz);

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
