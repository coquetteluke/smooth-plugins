#pragma once

#include "../PluginProcessor.h"
#include "Controls.h"
#include "StageView.h"

/** The window. Everything is laid out once at 1200 by 780 and scaled as a whole, so type and spacing stay in proportion at any size.
    Every control is a real parameter. The window adds two things to a session: its size, and whether Advanced is open. The processor saves both. */
class HarmonyEditor final : public juce::AudioProcessorEditor
{
public:
    static constexpr int designWidth = 1200, designHeight = 780;

    explicit HarmonyEditor (PluginProcessor&);
    ~HarmonyEditor() override;

    void resized() override;

    /** One display frame. The screen's refresh drives this normally. The offline renders and tests call it directly. */
    void tick (double dtSeconds);

    /** For the offline renders, which have no mouse. Lights the control with this ID as if it were hovered. */
    void previewHover (const juce::String& paramId);

    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept;

    ui::StageView& getStage() noexcept        { return panel.stage; }
    juce::Component& getPanel() noexcept      { return panel; }
    juce::String getHintText() const          { return panel.hintLine.getText(); }

private:
    class PresetBox;
    class AdvancedHeader;

    /** One voice: what it sings, how far from the lead, and its four main controls. */
    class VoiceStrip final : public juce::Component
    {
    public:
        VoiceStrip (PluginProcessor&, int index);

        void resized() override;
        void setSinging (const juce::String& note, bool isLive, bool isOn, bool isCompact);

        ui::SingerSwitch head;
        ui::IntervalField interval;
        ui::BarSlider level, pan, drift, tone;

    private:
        PluginProcessor& processor;
        const int voice;
        const juce::Colour colour;
        bool onShown = true, compactShown = false;
    };

    struct Panel final : public juce::Component
    {
        Panel (HarmonyEditor&, PluginProcessor&);
        ~Panel() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        void showHint (const juce::String&);
        void hintFromComponent (juce::Component*);
        void placeForMode (int mode);
        int voiceUnder (juce::Component*) const;

        HarmonyEditor& owner;
        PluginProcessor& processor;
        juce::SharedResourcePointer<ui::Assets> assets;

        ui::StageView stage;
        std::unique_ptr<PresetBox> presets;
        std::unique_ptr<AdvancedHeader> advancedHeader;

        ui::VoicePicker voicesCount;
        ui::Segments modePicker;
        ui::SingerSwitch leadSwitch;
        ui::PillToggle bypass;
        ui::ChoiceField keyField, scaleField, chordRootField, chordTypeField, inputType;
        ui::PillToggle followKey, chordHold;

        std::unique_ptr<VoiceStrip> strips[PluginProcessor::maxVoices];

        ui::BarSlider humanize, vibrato, leadLevel, harmonyLevel, mix;

        // Advanced
        std::unique_ptr<ui::BarSlider> throat[PluginProcessor::maxVoices];
        ui::BarSlider vibratoRate, glide, followLead, breath, consonants, scatter, leadPan, tracking, reference;

        juce::Label hintLine;
        float advancedAmount = 0.0f;   // 0 closed, 1 open
        int modeShown = -1, cardsShown = 4;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };

    void applyScale();

    PluginProcessor& processorRef;
    ui::LookAndFeel lookAndFeel;
    Panel panel;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0;
    bool constructed = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonyEditor)
};
