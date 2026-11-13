#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "editor/Theme.h"

// The window: a sheet of paper with the key on it in black, large enough to read from across a room, beside a circle of fifths
// whose ink shows which keys look likely right now. It only polls the processor on a timer. Everything it sets is a real parameter.
class KeyEditor final : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    static constexpr int designWidth = 1100, designHeight = 700;

    explicit KeyEditor (PluginProcessor&);
    ~KeyEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Public so a test can drive it with no message loop running.
    void refresh();
    juce::String getKeyText() const { return keyText; }
    juce::String getStatusText() const { return statusText; }
    juce::String getNoteText() const { return noteText; }
    juce::String getSecondLineText() const;   // the line under the reading: the runner up, a close call, or the lock
    juce::String getUseText() const;          // the block that takes the other key, empty when it is not offered
    juce::String getCentreText() const;       // the middle of the circle

    bool isAdvancedOpen() const noexcept;
    void setAdvancedOpen (bool shouldBeOpen, bool animate);

    // Sets Manual Key, and Manual Scale when scale is 0 or 1, as one gesture each.
    void setManualKey (int key, int scale);

    // Takes a key by hand: Manual Key, Manual Scale and Mode to Manual, as one gesture each, and only those that change.
    void useKey (int key, bool minor);

    // For the tests and the offline renders.
    juce::Component& getPanel();
    juce::Rectangle<int> getKeyLetterBounds();   // in panel coordinates
    void previewHover (const juce::String& componentId);
    void previewHover (juce::Component*);

private:
    struct Panel;

    void timerCallback() override;

    PluginProcessor& processor;
    keyui::LookAndFeel lookAndFeel;
    juce::SharedResourcePointer<keyui::Assets> assets;
    std::unique_ptr<Panel> panel;

    juce::String keyText, statusText, noteText;
    float advancedAmount = 0.0f;
    std::array<float, 24> candidateInk {};
    std::array<juce::uint8, 24> candidateLevels {};
    int quietTicks = 0, timerRate = 0;
    double lastTick = 0.0;
    bool constructed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyEditor)
};
