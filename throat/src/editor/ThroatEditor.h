#pragma once
#include "../PluginProcessor.h"
#include "Controls.h"
#include "TractView.h"

/** The window. Everything is laid out once at 1120 by 740 and scaled as a whole, so type and spacing stay in proportion at any
    size. Every control is a real parameter. The window adds two things to a session: its size, and whether Advanced is open. */
class ThroatEditor final : public juce::AudioProcessorEditor
{
public:
    static constexpr int designWidth = 1120, designHeight = 740;

    explicit ThroatEditor (PluginProcessor&);
    ~ThroatEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** One display frame. The screen's refresh drives this normally. The offline renders call it directly. */
    void tick (double dtSeconds);

    /** For the offline renders, which have no mouse. Lights the control with this ID as if it were hovered. */
    void previewHover (const juce::String& paramId);

    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept;

    ui::TractView& getView() noexcept     { return panel.view; }
    juce::Component& getPanel() noexcept  { return panel; }

    /** Puts the shape back to the singer's own, as one gesture per parameter. Everything else stays. */
    void resetShape();

private:
    class PresetBox;
    class AdvancedHeader;
    class SmallButton;

    struct Panel final : public juce::Component
    {
        Panel (ThroatEditor&, PluginProcessor&);
        ~Panel() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        void showHint (const juce::String&);
        void hintFromComponent (juce::Component*);
        void layoutSide();
        void lightPoint (int);
        int countAdvancedChanges() const;
        juce::String latencyText() const;

        ThroatEditor& owner;
        PluginProcessor& processor;
        juce::SharedResourcePointer<ui::Assets> assets;

        ui::TractView view;
        std::unique_ptr<PresetBox> presets;
        std::unique_ptr<AdvancedHeader> advancedHeader;
        std::unique_ptr<SmallButton> resetButton;
        ui::ParamToggle bypass;

        ui::ParamSlider length, width, breath, mix, output;
        std::array<std::unique_ptr<ui::ParamSlider>, ui::TractView::numPoints> pointWidth, pointPlace;

        ui::ParamChoice sourceVoice, modelVoice, range, precision;
        ui::ParamSlider pulse, breathHp;
        ui::ParamToggle levelMatch;

        juce::Label hintLine;
        juce::String latencyShown;
        float advancedAmount = 0.0f;   // 0 closed, 1 open
        int pointLit = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };

    void applyScale();

    PluginProcessor& processorRef;
    ui::LookAndFeel lookAndFeel;
    Panel panel;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0;
    int slowCountdown = 0;
    bool constructed = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThroatEditor)
};
