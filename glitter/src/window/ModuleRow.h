#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

/** The bold row of eleven colour blocks in signal order. A block is lit when its module is on, and says what it is set to.
    Clicking a block shows its controls. Clicking the switch on a block turns it on or off, through its real On parameter. */
class ModuleRow final : public juce::Component,
                        public fxui::Hinted
{
public:
    explicit ModuleRow (PluginProcessor&);

    /** A module picked, or none (-1) while Advanced is showing. */
    void setSelected (int module);
    int getSelected() const noexcept  { return selected; }
    std::function<void (int)> onSelect;
    std::function<void (const juce::String&)> onHint;
    std::function<void()> onSwitched;

    /** From the editor's frame. Returns true if anything changed. */
    bool refresh();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    bool isOn (int module) const noexcept        { return onNow[(size_t) module]; }
    juce::String summary (int module) const      { return summaries[(size_t) module]; }

    /** Height a selected block reaches down into the panel below. */
    static constexpr int tabDepth = 12;

    /** For the offline renders and tests: the same as a click on a switch. */
    void pressSwitch (int module);

private:
    juce::Rectangle<float> blockBounds (int) const;
    juce::Rectangle<float> switchBounds (int) const;
    juce::Rectangle<float> switchHitArea (int) const;
    int blockAt (juce::Point<float>) const;
    void updateSummaries();
    void updateHint();

    PluginProcessor& processor;
    std::array<std::unique_ptr<juce::ParameterAttachment>, fxui::numModules> switches;
    std::array<bool, fxui::numModules> onNow {};
    std::array<juce::String, fxui::numModules> summaries;
    std::vector<std::atomic<float>*> watched;
    std::vector<float> seen;
    std::atomic<float>* bypassParam = nullptr;
    bool bypassed = false, hoverOnSwitch = false;
    int selected = 0, hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleRow)
};
