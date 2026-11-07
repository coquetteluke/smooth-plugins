#pragma once
#include "Theme.h"

namespace tuneui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** A real parameter on a real slider: label, big value, flat bar. The attachment carries begin and end gesture, automation moves it,
        double-click goes back to the default and a click on the value lets you type one. */
    class ParamSlider final : public juce::Slider,
                              public Hinted,
                              private juce::Timer
    {
    public:
        enum class Kind { lead, main, compact };

        ParamSlider (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind);
        ~ParamSlider() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::String valueText() const;
        juce::String unitText() const;
        bool isChanged() const;

        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }

        /** What a click on the value does, and what pressing return in it does. Public so that the offline renders can reach them. */
        void startTyping();
        void applyTyped (const juce::String&);

    private:
        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override;
        };

        void timerCallback() override;
        juce::Rectangle<float> trackArea() const;
        juce::Rectangle<int> valueArea() const;
        juce::String description() const;

        juce::RangedAudioParameter& param;
        const juce::String label;
        const Kind kind;
        bool forcedHover = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSlider)
    };

    /** An on or off parameter, as a flat button that fills when on. */
    class ParamToggle final : public juce::ToggleButton,
                              public Hinted
    {
    public:
        ParamToggle (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour onColour, juce::Colour onText);
        void paintButton (juce::Graphics&, bool isOver, bool isDown) override;
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }
        bool isChanged() const;

    private:
        juce::RangedAudioParameter& param;
        const juce::Colour onFill, onInk;
        bool forcedHover = false;
        std::unique_ptr<juce::ButtonParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamToggle)
    };

    /** A choice parameter as joined buttons, for the few short choices. */
    class ParamSegments final : public juce::Component,
                                public Hinted
    {
    public:
        ParamSegments (Apvts&, const juce::String& paramId, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        int getSelected() const noexcept  { return selected; }

    private:
        int segmentAt (juce::Point<float>) const;

        juce::RangedAudioParameter& param;
        const juce::StringArray choices;
        int selected = 0, hovered = -1;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSegments)
    };

    /** A choice parameter as a menu. A second click straight after opening closes the menu and goes back to the default, so double-click resets here too. */
    class ParamMenu final : public juce::Component,
                            public Hinted
    {
    public:
        ParamMenu (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText);

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        int getSelected() const noexcept  { return selected; }
        bool isChanged() const;

    private:
        juce::RangedAudioParameter& param;
        const juce::String label;
        const juce::StringArray choices;
        int selected = 0;
        juce::uint32 openedAt = 0;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamMenu)
    };

    /** The factory presets, through the host's own program list. The name opens the full list, the arrows step through it. */
    class PresetBox final : public juce::Component,
                            public Hinted
    {
    public:
        explicit PresetBox (juce::AudioProcessor&);

        void refresh();
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override  { setZone (zoneAt (getMouseXYRelative().toFloat().x)); }
        void mouseExit (const juce::MouseEvent&) override  { setZone (none); }
        void mouseDown (const juce::MouseEvent&) override;

    private:
        enum Zone { none, previous, name, next };
        static constexpr float arrowWidth = 28.0f;
        Zone zoneAt (float x) const;
        void setZone (Zone);
        void choose (int);

        juce::AudioProcessor& processor;
        int shown = -1;
        Zone zone = none;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBox)
    };
}
