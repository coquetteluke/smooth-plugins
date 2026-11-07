#pragma once
#include "../PluginProcessor.h"
#include "Controls.h"
#include "PunchViews.h"

// The window. The last few seconds across the top, the last hit large below it with the meters beside it, and under the hit the two
// controls that shape it, each under the part of the hit it works on. Every control is a real AU parameter with its own ID.
// The window only ever adds its size and whether Advanced is open to the saved state.
class PunchEditor final : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit PunchEditor (PluginProcessor&);
    ~PunchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** One frame's work. The timer calls it, and so do the offline renders. Returns true while anything is moving. */
    bool tick (double seconds);

    bool isAdvancedOpen() const noexcept;
    void setAdvancedOpen (bool);

    punchview::HitView& getHitView() noexcept        { return hitView; }
    punchview::TimelineView& getTimeline() noexcept  { return timeline; }
    punchview::ReadoutRow& getReadouts() noexcept    { return readouts; }
    juce::String getHintText() const                 { return hintText; }
    void previewHint (const juce::String& paramId);
    bool isFastTimer() const noexcept                { return fast; }

    static constexpr int drawerHeight = 60;

private:
    class AdvancedHeader;

    void timerCallback() override;
    void showHintFor (juce::Component*);
    void setHint (const juce::String&);
    void updateAdvancedSummary();
    punchview::Settings readSettings() const;

    PluginProcessor& processorRef;
    juce::SharedResourcePointer<ui::Assets> assets;   // fonts live as long as any window
    ui::LookAndFeel lookAndFeel;

    punchview::HitView hitView;
    punchview::TimelineView timeline;
    punchview::MeterPanel meters;
    punchview::ReadoutRow readouts;

    ui::PresetBox presets;
    ui::Pill bypass;
    ui::BarSlider attack, sustain, punch, impact, ceiling;
    ui::Pill ceilingOn;
    ui::BarSlider gain, mix;
    std::unique_ptr<AdvancedHeader> advancedHeader;

    juce::Rectangle<int> titleArea, hintArea;
    juce::String hintText;
    std::uint32_t lastBin = 0;
    double sinceBins = 10.0, sinceSound = 10.0, sinceChange = 10.0, sinceReadout = 1.0;
    float shownIn = -60.0f, shownOut = -60.0f, shownImpact = 0.0f, shownCeiling = 0.0f;
    punchview::Settings lastSettings;
    bool constructed = false, fast = false;
    int slowCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PunchEditor)
};
