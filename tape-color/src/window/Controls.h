#pragma once
#include "Theme.h"
#include <functional>

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** A real parameter as a flat block that fills up: the name on top, the value below it. Drag anywhere on it, click the value to
        type one, double-click to go back to the default. The attachment carries begin and end gesture, and automation moves it. */
    class Tile final : public juce::Slider,
                       public Hinted,
                       private juce::Timer
    {
    public:
        using Formatter = std::function<juce::String (float)>;

        Tile (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Formatter);
        ~Tile() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::String valueText() const;
        /** The text a value would show, for whatever is worked out from the parameter itself rather than this block's copy of it. */
        juce::String textFor (float value) const  { return formatter (value); }
        void startTyping();
        void applyTyped (const juce::String&);
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }
        bool isTyping() const noexcept  { return typeBox.isVisible(); }

        /** Smaller type, for the tighter blocks of Advanced. */
        void setCompact (bool shouldBe)  { compact = shouldBe; repaint(); }

        /** Shown fainter when nothing else lets it do anything right now. It still works. */
        void setResting (bool shouldRest);
        bool isResting() const noexcept  { return resting; }

        /** A control whose word means a value: "Off" on the cuts. */
        juce::String offWord;

        /** Frequencies may be typed with a k. */
        bool acceptsKilo = false;

    private:
        void timerCallback() override;
        juce::Rectangle<int> valueArea() const;

        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override;
        };

        juce::RangedAudioParameter& param;
        const juce::String label;
        const Formatter formatter;
        const double restValue;
        bool forcedHover = false, compact = false, resting = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Tile)
    };

    /** A choice parameter with few options, all in view, inside a block the same size as a Tile. */
    class Segments final : public juce::Component,
                           public Hinted
    {
    public:
        Segments (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::StringArray shortNames);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }
        void choose (int index)  { attachment.setValueAsCompleteGesture ((float) index); }

        /** A live reading beside the name, or nothing. */
        void setReading (const juce::String&);

    private:
        juce::Rectangle<float> chipArea (int index) const;
        int chipAt (juce::Point<float>) const;

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray names;
        juce::String reading;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Segments)
    };

    /** A longer choice parameter, in a block that shows the choice and opens the list. */
    class ChoiceTile final : public juce::Component,
                             public Hinted,
                             private juce::Timer
    {
    public:
        ChoiceTile (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);
        ~ChoiceTile() override;

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }
        void choose (int index)  { attachment.setValueAsCompleteGesture ((float) index); }

        /** How long until the list opens, in ms, or 0 when it is not waiting to. For the tests. */
        int getMenuWait() const noexcept  { return isTimerRunning() ? getTimerInterval() : 0; }

    private:
        void timerCallback() override;

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray choices;
        int selected = 0;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceTile)
    };

    /** An on or off parameter as a flat button. Filled means on. */
    class Pill final : public juce::Component,
                       public Hinted
    {
    public:
        Pill (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        bool isOn() const noexcept  { return on; }
        void setOn (bool shouldBeOn)  { attachment.setValueAsCompleteGesture (shouldBeOn ? 1.0f : 0.0f); }

    private:
        juce::RangedAudioParameter& param;
        const juce::String label;
        bool on = false;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pill)
    };

    /** The name of the preset, with next and previous arrows. */
    class PresetBox final : public juce::Component,
                            public Hinted
    {
    public:
        explicit PresetBox (juce::AudioProcessor&);

        /** What to call the current settings is asked of the owner, which knows the processor's type. */
        std::function<juce::String()> nameNow;

        /** The preset the arrows step from and the list ticks, or -1 for none. The processor's current one if the owner says nothing. */
        std::function<int()> indexNow;

        /** What the box says when no preset is known. Shown in the quieter colour. */
        static juce::String noneName()  { return "Choose a preset"; }

        void refresh();
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void choose (int index);

    private:
        enum Zone { none, previous, name, next };
        static constexpr float arrowWidth = 30.0f;
        Zone zoneAt (float x) const;
        void setZone (Zone);

        juce::AudioProcessor& processor;
        Zone zone = none;
        juce::String shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBox)
    };

    /** Age: one long ribbon that narrows from a wide fresh reel to a thin worn cassette. Click or drag anywhere on it. */
    class AgeRibbon final : public juce::Slider,
                            public Hinted
    {
    public:
        AgeRibbon (Apvts&, const juce::String& paramId, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void buildImages (float scale);

        juce::Image filled, empty, picture;
        float imageScale = 0.0f, pictureProportion = -1.0f;
        bool pictureLit = false;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AgeRibbon)
    };
}
