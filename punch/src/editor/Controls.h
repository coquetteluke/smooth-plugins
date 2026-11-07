#pragma once
#include "Theme.h"
#include <functional>

namespace ui
{
    using Apvts = juce::AudioProcessorValueTreeState;

    /** A real parameter on a flat bar: the name and value on one line, the bar under them. Drag the bar, click the value to type
        one, double-click to go back to the default. The attachment carries begin and end gesture, and automation moves it. */
    class BarSlider final : public juce::Slider,
                            public Hinted,
                            private juce::Timer
    {
    public:
        using Formatter = std::function<juce::String (float)>;

        /** Large is for the two controls that shape a hit. */
        enum class Size { large, regular };

        BarSlider (Apvts&, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Size, Formatter);
        ~BarSlider() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::String valueText() const;
        void startTyping();
        void applyTyped (const juce::String&);
        void setForcedHover (bool shouldShow)  { forcedHover = shouldShow; repaint(); }
        bool isTyping() const                  { return typeBox.isVisible(); }

        /** The two ends of a large bar, in plain words. */
        void setEndWords (const juce::String& low, const juce::String& high)  { lowWord = low; highWord = high; }
        /** Greyed, for a control that is not doing anything at the moment. It still works. */
        void setDimmed (bool);

    private:
        void timerCallback() override;
        juce::Rectangle<int> valueArea() const;
        int topHeight() const noexcept  { return size == Size::large ? 44 : 26; }

        struct TypeBox final : public juce::Label
        {
            juce::TextEditor* createEditorComponent() override;
        };

        juce::RangedAudioParameter& param;
        const juce::String label;
        const Size size;
        const Formatter formatter;
        const double restValue;
        juce::String lowWord, highWord;
        bool forcedHover = false, dimmed = false;
        TypeBox typeBox;
        std::unique_ptr<juce::SliderParameterAttachment> attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BarSlider)
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

    /** The name of the preset, with next and previous arrows. Says so when the settings have moved off it. */
    class PresetBox final : public juce::Component,
                            public Hinted
    {
    public:
        explicit PresetBox (juce::AudioProcessor&);

        /** What to call the current settings is asked of the owner, which knows the processor's type. */
        std::function<juce::String()> nameNow;

        void refresh();
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void choose (int index);
        juce::String getShownName() const  { return shown; }

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
}
