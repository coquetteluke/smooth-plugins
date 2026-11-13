#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "window/Controls.h"
#include "window/SliceViews.h"

// The window. The waveform cut into coloured slices runs edge to edge, each slice flows down to its key, and the controls below
// edit whichever slice is selected. Every knob is a real parameter. Per slice controls point at the selected slot's parameters,
// so all 32 slots are in reach through the Selected Slice control. The window only adds its size and Advanced to the saved state.
class SliceEditor final : public juce::AudioProcessorEditor,
                          public juce::FileDragAndDropTarget,
                          private juce::Timer
{
public:
    explicit SliceEditor (PluginProcessor&);
    ~SliceEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    // Public so a test can drive the window with no message loop running.
    void refresh();
    SliceWaveform& getWaveform() noexcept { return waveform; }
    KeyMap& getKeyMap() noexcept { return keyMap; }
    juce::String getStatusText() const { return statusText; }
    juce::String getSelectedText() const;
    juce::String getHintText() const { return hintText; }
    int getHintRoom() const noexcept { return hintArea.getWidth(); }
    static juce::String getWindowTitle() { return "Cutup"; }

    bool isAdvancedOpen() const noexcept;
    void setAdvancedOpen (bool);
    bool isZoomed() const noexcept { return zoomButton.isLatched(); }
    void setZoomed (bool);
    void previewHint (const juce::String& componentId);
    bool isFastTimer() const noexcept { return fast; }
    juce::String getAdvancedSummary() const;      // what the closed Advanced header shows
    juce::Component& getAdvancedHeader() noexcept;

    // The arrows that step through the slots, for a test to click.
    juce::Component& getStepperComponent() noexcept;
    juce::Point<float> stepperNextArrow() const;
    juce::Point<float> stepperPreviousArrow() const;

private:
    class SliceStepper;
    class AdvancedHeader;

    void timerCallback() override;
    void pollSlow();
    void pollFast();
    void bindSelected (int index);
    void updateToolbar();
    void updateInfo();
    void chooseFile();
    void showHintFor (juce::Component*);
    void setHint (const juce::String&);
    bool isDraggingSlotControl() const;
    bool isTypingSlotControl() const;
    int keyForSlice (int index) const;
    int advancedHeaderWidth (int footWidth) const;
    int advancedRoom (int footWidth) const;
    bool acceptsFile (const juce::String&) const;

    PluginProcessor& processor;
    juce::SharedResourcePointer<ui::Assets> assets;
    ui::LookAndFeel lookAndFeel;

    SliceWaveform waveform;
    KeyMap keyMap;
    AmpGraph ampGraph;
    PitchGraph pitchGraph;

    ui::PresetBox presets;
    ui::ActionButton loadButton, zoomButton;
    ui::Pill bypass;
    ui::Segments sliceMode, gridType, keyMode;
    ui::MenuField grid, gridNote, playMode;
    ui::ValueCell sensitivity, sliceStart, gridTempo;
    ui::ValueCell output, root, polyphony, velSens, loopFade;
    ui::ValueCell decay, sustain, penvAmount, penvTime;
    ui::ValueCell minSlice, transpose, bendRange;
    ui::ValueCell slotPitch, slotLevel, slotPan, slotAttack, slotRelease;
    ui::Pill slotReverse;
    ui::ReadingCell slicesFound, sampleLength, activeVoices;

    std::unique_ptr<SliceStepper> stepper;
    std::unique_ptr<AdvancedHeader> advancedHeader;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> titleArea, infoArea, manualNoteArea, hintArea, ampLabel, pitchLabel;
    juce::String statusText, hintText, summaryText;
    int boundSlot = -1, shownMode = -1, shownLayout = -1, shownRestore = 0, slowCounter = 0;
    bool shownAdvanced = false;
    int longestHint = 0;
    slicecore::SliceTable shownTable;
    bool constructed = false, fast = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceEditor)
};
