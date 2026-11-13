#pragma once

#include "../PluginProcessor.h"
#include "Controls.h"
#include "StageView.h"

/** The window. Everything is laid out once at 1180 by 720 and scaled as a whole, so type and spacing stay in proportion at any size.
    Every control is a real parameter. The window adds two things to a session: its size, and whether Advanced is open. The processor saves both. */
class DuoEditor final : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    static constexpr int designWidth = 1180, designHeight = 720;

    explicit DuoEditor (PluginProcessor&);
    ~DuoEditor() override;

    void resized() override;

    /** One display frame. The screen's refresh drives this normally. The offline renders and tests call it directly. */
    void tick (double dtSeconds);

    /** For the offline renders, which have no mouse. Lights the control with this ID as if it were hovered. */
    void previewHover (const juce::String& paramId);

    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept;
    float getAdvancedAmount() const noexcept  { return panel.advancedAmount; }

    ui::StageView& getStage() noexcept        { return panel.stage; }
    juce::Component& getPanel() noexcept      { return panel; }
    juce::String getHintText() const          { return panel.hintLine.getText(); }
    juce::String getReadoutText (int singer) const  { return panel.readouts[(size_t) singer].getText(); }
    /** What the preset box reads, for the tests. */
    juce::String getPresetText() const;

private:
    class PresetBox;
    class AdvancedHeader;

    struct Panel final : public juce::Component
    {
        Panel (DuoEditor&, PluginProcessor&);
        ~Panel() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        void showHint (const juce::String&);
        void hintFromComponent (juce::Component*);

        DuoEditor& owner;
        PluginProcessor& processor;
        juce::SharedResourcePointer<ui::Assets> assets;

        ui::StageView stage;
        std::unique_ptr<PresetBox> presets;
        std::unique_ptr<AdvancedHeader> advancedHeader;

        ui::CountPicker doublesCount;
        ui::PillToggle bypass;
        ui::WidthControl width;
        ui::TightnessControl tightness;
        ui::LevelFader leadLevel, level1, level2, level3;
        std::array<ui::Readout, DuoEngine::maxDoubles + 1> readouts;

        // Advanced
        ui::ValueSlider timing, pitch, vibrato, timbre, leadPan, doublesPan, extraDelay;
        ui::ChoiceField range;
        ui::PillToggle preciseVibrato, monoSafe, matchLevel;

        juce::Label hintLine;
        float advancedAmount = 0.0f;   // 0 closed, 1 open

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };

    void timerCallback() override;
    bool isBusy();
    void updateReadouts (double dtSeconds);

    PluginProcessor& processorRef;
    ui::LookAndFeel lookAndFeel;
    Panel panel;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0, restingFor = 0.0, sinceReadout = 10.0;
    int noteShown = -1, noteCandidate = -1, noteHeld = 0;
    bool constructed = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DuoEditor)
};
