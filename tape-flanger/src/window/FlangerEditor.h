#pragma once
#include "../PluginProcessor.h"
#include "Controls.h"
#include "CombField.h"

/** The window. Laid out once at 1040 by 680 and scaled as a whole, so type and spacing stay in proportion at any size. Every
    control is a real parameter. The window adds two things to a session: its size, and whether Advanced is open. */
class FlangerEditor final : public juce::AudioProcessorEditor
{
public:
    static constexpr int designWidth = 1040, designHeight = 680;

    explicit FlangerEditor (PluginProcessor&);
    ~FlangerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** One display frame. The screen's refresh drives this normally. The offline renders call it directly. */
    void tick (double dtSeconds);

    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept;

    ui::CombField& getField() noexcept;
    juce::Component& getPanel() noexcept;
    /** For the offline renders, which have no mouse. Lights the control with this ID as if it were hovered. */
    void previewHover (const juce::String& paramId);

    class Panel;

private:
    PluginProcessor& processorRef;
    ui::LookAndFeel lookAndFeel;
    std::unique_ptr<Panel> panel;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0;
    bool constructed = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlangerEditor)
};
