#pragma once

#include "../PluginProcessor.h"
#include "Controls.h"
#include "ResonanceGraph.h"

/** The window. Everything is laid out once at 1160 by 760 and scaled as a whole, so type, spacing and curves stay in
    proportion at any size. Every control is a real parameter. The window adds two things to a session: its size, and whether
    the Advanced section is open. The processor saves both. */
class ResonanceEditor final : public juce::AudioProcessorEditor
{
public:
    static constexpr int designWidth = 1160, designHeight = 760;

    explicit ResonanceEditor (SmoothResonanceProcessor&);
    ~ResonanceEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** One display frame. The screen's refresh drives this normally. The offline renders call it directly. */
    void tick (double dtSeconds);

    /** For the offline renders, which have no mouse. Lights the control with this ID as if it were hovered. */
    void previewHover (const juce::String& paramId);

    /** Opens or closes the Advanced section. Without animation it is there straight away. */
    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept;

    ui::ResonanceGraph& getGraph() noexcept  { return panel.graph; }
    juce::Component& getPanel() noexcept     { return panel; }

private:
    class PresetBox;
    class PeakReading;
    class BandPicker;
    class AdvancedHeader;
    class ChannelMeters;
    class SmallButton;

    struct Panel final : public juce::Component
    {
        Panel (ResonanceEditor&, SmoothResonanceProcessor&);
        ~Panel() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override  {}

        void showHint (const juce::String&);
        void hintFromComponent (juce::Component*);
        void showBand (int);
        void layoutSide();
        void refreshDots();
        void refreshChannels();
        int countAdvancedChanges() const;

        ResonanceEditor& owner;
        SmoothResonanceProcessor& processor;
        juce::SharedResourcePointer<ui::Assets> assets;

        ui::ResonanceGraph graph;

        std::unique_ptr<PresetBox> presets;
        std::unique_ptr<PeakReading> reading;
        std::unique_ptr<BandPicker> bandPicker;
        std::unique_ptr<AdvancedHeader> advancedHeader;
        std::unique_ptr<ChannelMeters> channelMeters;
        std::unique_ptr<SmallButton> listenButton, removeButton[smoothres::Params::numBands];

        ui::ParamSlider depth, maxCut, sharpness, selectivity, attack, release;
        ui::ParamChoice mode, quality;
        ui::ParamSlider hardThreshold;
        ui::ParamChoice stereoMode;
        ui::ParamSlider link, stereoFocus, sharpnessTilt, attackTilt, releaseTilt, wetTrim;
        ui::ParamToggle sideChain, sideChainListen;
        ui::ParamSlider lowFreq, highFreq;
        std::unique_ptr<ui::ParamSlider> bandFreq[smoothres::Params::numBands], bandSens[smoothres::Params::numBands], bandWidth[smoothres::Params::numBands];
        ui::ParamToggle bypass, delta;
        ui::ParamSlider mix, outputGain;

        juce::Label hintLine;
        float advancedAmount = 0.0f;   // 0 closed, 1 open
        bool thresholdShown = false;
        juce::uint32 dotsShown = 0xffffffff;
        int listenShown = -2, channelsShown = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };

    void applyScale();

    SmoothResonanceProcessor& processorRef;
    ui::LookAndFeel lookAndFeel;
    Panel panel;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0;
    int changesCountdown = 0;
    bool constructed = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceEditor)
};
