#pragma once
#include "../PluginProcessor.h"
#include "Controls.h"
#include "TapeView.h"

/** The window. Everything is laid out once at 1080 by 640 and scaled as a whole, so type and spacing stay in proportion at any size.
    Every control is a real parameter. The window adds two things to a session: its size, and whether Advanced is open. */
class TapeChorusEditor final : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    static constexpr int designWidth = 1080, designHeight = 640;

    explicit TapeChorusEditor (PluginProcessor&);
    ~TapeChorusEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** One display frame. The screen's refresh drives this normally. The offline renders call it directly. Returns whether anything moved. */
    bool tick (double dtSeconds);

    /** For the offline renders, which have no mouse. Lights the control with this ID as if it were hovered. */
    void previewHover (const juce::String& paramId);

    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept;

    /** What the preset box shows. For the tests. */
    juce::String getPresetText() const;

    ui::TapeView& getView() noexcept      { return panel.view; }
    juce::Component& getPanel() noexcept  { return panel; }

private:
    class PresetBox;
    class AdvancedToggle;

    struct Panel final : public juce::Component
    {
        Panel (TapeChorusEditor&, PluginProcessor&);
        ~Panel() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        void showHint (const juce::String&);
        void hintFromComponent (juce::Component*);
        void layoutColumns();
        void refreshReadings();
        int countAdvancedChanges() const;

        TapeChorusEditor& owner;
        PluginProcessor& processor;
        juce::SharedResourcePointer<ui::Assets> assets;

        ui::TapeView view;
        std::unique_ptr<PresetBox> presets;
        std::unique_ptr<AdvancedToggle> advancedToggle;
        ui::ParamSwitch bypass;

        ui::ParamStrip inputGain, voices, rate, depth, delay, feedback, spread, mix, outputGain;
        ui::Readout inputLevel, pitchSwing, outputLevel;

        ui::ParamStrip wow, wowRate, flutter, flutterRate, random, randomRate, saturation, degrade, degradeRate, noise, tone, lowCut, highCut;
        ui::ParamSwitch unsync, noiseGate, clean, monoSafe;

        juce::Label hintLine;
        float advancedAmount = 0.0f;   // 0 closed, 1 open
        int headerY = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };

    void timerCallback() override;
    void attachScreen();

    PluginProcessor& processorRef;
    ui::LookAndFeel lookAndFeel;
    Panel panel;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0;
    int slowCountdown = 0, stillFrames = 0;
    bool constructed = false, lettingGo = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeChorusEditor)
};
