#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// The window's look: charcoal, bone and oxide red. Bone always means the singer's own throat. Oxide always means the model.
// Gray always means the realistic range. Nothing else gets a colour. Things are separated by space, not by lines.
namespace ui
{
    namespace tone
    {
        inline const juce::Colour base      { 0xff201f1d };   // the window
        inline const juce::Colour well      { 0xff181716 };   // behind the tract
        inline const juce::Colour text      { 0xffece5d6 };   // bone
        inline const juce::Colour muted     { 0xffa89f90 };   // labels
        inline const juce::Colour faint     { 0xff7d766a };   // scales and quiet readouts
        inline const juce::Colour range     { 0xff363431 };   // the realistic range
        inline const juce::Colour oxide     { 0xffb5472a };   // the model, sounding
        inline const juce::Colour oxideDim  { 0xff6d3323 };   // the model, silent
        inline const juce::Colour oxideLine { 0xffe26c4b };   // the model as thin lines and small type, which need more light
        inline const juce::Colour resting   { 0xff45413c };   // the model while bypassed
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

    inline void setReal (juce::RangedAudioParameter& p, float value)  { p.setValueNotifyingHost (p.convertTo0to1 (value)); }
    inline float getReal (const juce::RangedAudioParameter& p)        { return p.convertFrom0to1 (p.getValue()); }

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
