#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// The window's look. A calm near-flat base with colour kept for the graph. Gold only ever means "what is being taken out",
// or the control that decides it. Controls are neutral. Things are separated by space, not by lines.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base      { 0xff0f1218 };   // the window
        inline const juce::Colour well      { 0xff0a0c12 };   // the graph
        inline const juce::Colour text      { 0xffe9ecf2 };
        inline const juce::Colour muted     { 0xff969dae };   // labels
        inline const juce::Colour faint     { 0xff6e7588 };   // scales
        inline const juce::Colour voiceTop  { 0xff8ea6ff };   // the voice where it is loud
        inline const juce::Colour cobalt    { 0xff3a5ce6 };   // the voice's body
        inline const juce::Colour voiceLow  { 0xff131d52 };   // the voice at the floor
        inline const juce::Colour voiceEdge { 0xffb4c4ff };
        inline const juce::Colour gold      { 0xffdfb563 };
        inline const juce::Colour goldEdge  { 0xfff0d08f };
        inline const juce::Colour second    { 0xfff6ecdd };   // the second channel, when channels are treated apart
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
