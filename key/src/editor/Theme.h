#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "FontOutlines.h"

#include <string>
#include <unordered_map>

// The look: a sheet of paper, black type, and one red. Red means the key on show, or that something needs a look.
// Greys are for what is not the answer. Things are separated by space.
namespace keyui
{
    namespace tone
    {
        inline const juce::Colour paper { 0xfffbfbf9 };
        inline const juce::Colour ink   { 0xff000000 };
        inline const juce::Colour soft  { 0xff55554f };   // labels
        inline const juce::Colour faint { 0xff8e8e88 };   // what is not sounding or not set
        inline const juce::Colour wash  { 0xffefefeb };   // control and key bodies
        inline const juce::Colour deep  { 0xffd5d5ce };   // the same, one step on: what is chosen, and notes in the key
        inline const juce::Colour red   { 0xffe3170a };
    }

    enum Weight { book = 400, medium = 500, semi = 600, bold = 700, heavy = 800 };

    struct TextShape
    {
        juce::Path path;     // at type size 1000, baseline at y = 0
        float width = 0.0f;
    };

    // The embedded typeface, shared between open windows and freed with the last one.
    struct Assets
    {
        Assets();

        const TextShape& shape (const juce::String& text, int weight);

        VariableFont variable;
        juce::Typeface::Ptr plain;   // the default weight, for typing
        std::unordered_map<std::string, TextShape> shapes;
    };

    enum class Align { left, centre, right };

    float textWidth (const juce::String& text, int weight, float size);

    // Draws at a baseline. Returns the width.
    float drawText (juce::Graphics&, const juce::String& text, int weight, float size, float x, float baseline, Align = Align::left);

    // Sits the capitals in the middle of the height of the area.
    void drawTextIn (juce::Graphics&, const juce::String& text, int weight, float size, juce::Rectangle<float> area, Align = Align::left);

    juce::Font typingFont (float size);

    // Counts paints, for the CPU tool.
    inline int paintCount = 0;

    // A control that can explain itself in the hint line.
    struct Hinted
    {
        virtual ~Hinted() = default;
        juce::String hint;
    };

    inline float defaultOf (const juce::RangedAudioParameter& p) { return p.convertFrom0to1 (p.getDefaultValue()); }

    // One value change, with a begin and end so automation in Touch or Latch records it.
    inline void setAsGesture (juce::RangedAudioParameter& p, float value)
    {
        p.beginChangeGesture();
        p.setValueNotifyingHost (p.convertTo0to1 (value));
        p.endChangeGesture();
    }

    // Reads what someone typed as a key: "F#", "bb", "Bb minor", "am", "C major". False if it is not a key.
    // scale is -1 when the text does not say, 0 for major, 1 for minor.
    bool parseKey (const juce::String& typed, int& key, int& scale);

    // The box for typing a value: no frame or inset of its own, so its text lines up with what it replaces.
    struct TypeBox final : juce::Label
    {
        juce::TextEditor* createEditorComponent() override
        {
            auto* editor = juce::Label::createEditorComponent();
            editor->setIndents (0, 0);
            editor->setBorder ({});
            return editor;
        }
    };

    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();

        void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
        juce::Font getLabelFont (juce::Label&) override;
        void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
        void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    };
}
