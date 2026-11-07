#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

// The plain window: waveform with slice markers, Load and drag and drop, slice settings. A styled pass comes later.
// Every knob here is attached to a real parameter. It only polls the processor on a timer.
class SliceWaveform final : public juce::Component
{
public:
    explicit SliceWaveform (PluginProcessor& p) : processor (p) {}

    void refresh (const slicecore::SliceTable& t, int selected, bool playing);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    // Public so a test can drive the window with no message loop running.
    int frameAtX (float x) const;
    int sliceAtFrame (int frame) const;
    int markerNearX (float x) const;

private:
    float xForFrame (int frame) const;

    PluginProcessor& processor;
    slicecore::SliceTable table;
    int selected = 0, dragging = -1;
    bool wasPlaying = false;
};

class SliceEditor final : public juce::AudioProcessorEditor,
                          public juce::FileDragAndDropTarget,
                          private juce::Timer
{
public:
    explicit SliceEditor (PluginProcessor&);
    ~SliceEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    // Public so a test can drive the window with no message loop running.
    void refresh();
    SliceWaveform& getWaveform() noexcept { return waveform; }
    juce::String getStatusText() const { return statusLabel.getText(); }
    juce::String getSelectedText() const { return selectedLabel.getText(); }

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };
    struct Choice
    {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<ComboAttachment> attachment;
    };

    void timerCallback() override { refresh(); }
    void setupKnob (Knob&, const char* text, const char* paramId);
    void setupChoice (Choice&, const char* text, const char* paramId);
    void bindSelected (int index);
    void chooseFile();

    PluginProcessor& processor;
    SliceWaveform waveform;

    juce::TextButton loadButton { "Load" };
    juce::Label statusLabel, selectedLabel, helpLabel;
    std::unique_ptr<juce::FileChooser> chooser;

    Choice sliceMode, gridType, grid, gridNote, playMode, keyMode;
    Knob sensitivity, minSlice, sliceStart, gridTempo, polyphony, transpose, rootNote, velSens, penvAmount, penvTime, bendRange, output, loopFade, decay, sustain;
    Knob slotPitch, slotPan, slotAttack, slotRelease, slotLevel;
    juce::ToggleButton slotReverse { "Reverse" };
    std::unique_ptr<ButtonAttachment> slotReverseAttachment;

    int boundSlot = -1;
    int shownMode = -1;
    slicecore::SliceTable shownTable;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceEditor)
};
