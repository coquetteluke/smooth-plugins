#pragma once
#include "Theme.h"
#include <functional>

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** One real parameter: the name, a large value, a thin bar. Drag anywhere on it, click to type a value, double-click to go back to
        the default. It can be pointed at another parameter, so one cell can follow the selected slice. The attachment carries the
        begin and end of each gesture, and automation moves it. */
    class ValueCell final : public juce::Slider,
                            public Hinted,
                            private juce::Timer
    {
    public:
        using Formatter = std::function<juce::String (float)>;
        using Parser = std::function<float (const juce::String&, bool& understood)>;

        ValueCell (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Formatter, Parser = nullptr);
        ~ValueCell() override;

        void bind (const juce::String& paramId);
        juce::RangedAudioParameter* getParameter() const noexcept  { return param; }

        void setAccent (juce::Colour);
        void setHero (bool shouldBe)  { valueSize = shouldBe ? 19.0f : 16.0f; repaint(); }

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::String valueText() const;       // as drawn: without its unit when the whole of it would not fit
        juce::String fullValueText() const;   // always with its unit
        void startTyping();
        void applyTyped (const juce::String&);
        bool isBeingDragged() const noexcept  { return dragging; }
        bool isTyping() const noexcept  { return typeBox.isBeingEdited(); }

    private:
        void timerCallback() override;
        void startedDragging() override  { dragging = true; }
        void stoppedDragging() override  { dragging = false; }

        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override;
        };

        Apvts& state;
        juce::RangedAudioParameter* param = nullptr;
        const juce::String label;
        const Formatter formatter;
        const Parser parser;
        juce::Colour accent = tone::text;
        double restValue = 0.0;
        float valueSize = 16.0f;
        bool dragging = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValueCell)
    };

    /** A choice parameter shown as its name over its value. A click opens the list, a double-click goes back to the default. */
    class MenuField final : public juce::Component,
                            public Hinted,
                            private juce::Timer
    {
    public:
        MenuField (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, const juce::String& unitText = {});
        ~MenuField() override;

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }
        int getNumChoices() const noexcept  { return choices.size(); }
        void choose (int index)  { attachment.setValueAsCompleteGesture ((float) index); }

    private:
        void timerCallback() override;
        void openMenu();

        juce::RangedAudioParameter& param;
        const juce::String label, unit;
        const juce::StringArray choices;
        int selected = 0;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MenuField)
    };

    /** A choice parameter with few options, all in view. */
    class Segments final : public juce::Component,
                           public Hinted
    {
    public:
        Segments (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        int getSelected() const noexcept  { return selected; }
        int getNumChoices() const noexcept  { return choices.size(); }
        void choose (int index)  { attachment.setValueAsCompleteGesture ((float) index); }
        juce::Point<float> centreOf (int index) const;

        /** Called with the choice a click makes in this control, just before it is set. A host's change never calls it. */
        std::function<void (int)> onUserChoice;

    private:
        juce::Rectangle<float> buttonsArea() const;
        int segmentAt (juce::Point<float>) const;
        void chooseByHand (int index);

        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray choices;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Segments)
    };

    /** An on or off parameter as a flat button. Filled means on. It can be pointed at another parameter too. */
    class Pill final : public juce::Component,
                       public Hinted
    {
    public:
        Pill (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void bind (const juce::String& paramId);
        void setAccent (juce::Colour);

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        bool isOn() const noexcept  { return on; }

    private:
        Apvts& state;
        juce::RangedAudioParameter* param = nullptr;
        const juce::String label;
        juce::Colour accent = tone::text;
        bool on = false;
        std::unique_ptr<juce::ParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pill)
    };

    /** A flat button that is not a parameter: Load sample, and the view switch that zooms the waveform. */
    class ActionButton final : public juce::Component,
                               public Hinted
    {
    public:
        ActionButton (const juce::String& labelText, const juce::String& hintText, bool isPrimary);

        std::function<void()> onClick;
        void setLatched (bool);
        bool isLatched() const noexcept  { return latched; }

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override;

    private:
        const juce::String label;
        const bool primary;
        bool latched = false;
    };

    /** The name of the preset, with next and previous arrows, and the list on a click. */
    class PresetBox final : public juce::Component,
                            public Hinted
    {
    public:
        explicit PresetBox (juce::AudioProcessor&);

        void refresh();
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void choose (int index);

    private:
        enum Zone { none, previous, name, next };
        static constexpr float arrowWidth = 28.0f;
        Zone zoneAt (float x) const;
        void setZone (Zone);

        juce::AudioProcessor& processor;
        Zone zone = none;
        juce::String shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBox)
    };

    /** A read only value from the processor. */
    class ReadingCell final : public juce::Component,
                              public Hinted
    {
    public:
        ReadingCell (const juce::String& id, const juce::String& labelText, const juce::String& hintText);
        void setValueText (const juce::String&);
        const juce::String& getValueText() const noexcept  { return value; }
        void paint (juce::Graphics&) override;

    private:
        const juce::String label;
        juce::String value;
    };

    /** A small chevron, for lists and for Advanced. */
    void drawChevron (juce::Graphics&, juce::Point<float> centre, int direction /* 0 down, 1 right, 2 left */, juce::Colour);
}
