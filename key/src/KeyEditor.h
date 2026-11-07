#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

// The readout window: a big key and scale that can be read from across the room, then confidence and the controls.
// It only polls the processor on a timer. Everything it sets is a real parameter.
class KeyEditor final : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit KeyEditor (PluginProcessor&);
    ~KeyEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Public so a test can drive it with no message loop running.
    void refresh();
    juce::String getKeyText() const { return keyText; }
    juce::String getStatusText() const { return statusText; }
    juce::String getNoteText() const { return noteText; }

private:
    void timerCallback() override { refresh(); }
    void addChoice (juce::ComboBox&, juce::Label&, const char* paramId, const char* labelText);

    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    PluginProcessor& processor;

    juce::ComboBox responseBox, sourceBox, modeBox, manualKeyBox, manualScaleBox, tempoScaleBox;
    juce::Label responseLabel, sourceLabel, modeLabel, manualKeyLabel, manualScaleLabel, tempoScaleLabel;
    juce::TextButton listeningButton { "Listening" }, swapButton { "Relative Swap" }, resetButton { "Reset" };
    std::unique_ptr<ComboAttachment> responseAttach, sourceAttach, modeAttach, manualKeyAttach, manualScaleAttach, tempoScaleAttach;
    std::unique_ptr<ButtonAttachment> listeningAttach, swapAttach;

    juce::String keyText, scaleText, otherNameText, relativeText, statusText, noteText, referenceText, tempoText;
    bool referenceOff = false, dimmed = true;
    float confidence = 0.0f;
    int shownKey = -1;
    bool shownMinor = false;
    std::array<float, 12> chroma {};
    juce::Rectangle<int> readoutArea, meterArea, keyboardArea, infoArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyEditor)
};
