#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "window/Controls.h"
#include "window/PitchDisplay.h"

// The window. A live display on top, then what to tune to, the notes, and how the tuning behaves. Every control is a real
// AU parameter with its own ID. The window only ever adds its size and whether Advanced is open to the saved state.
class TuneEditor final : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit TuneEditor (PluginProcessor&);
    ~TuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    // What the display says now, in words. Used by the tests.
    juce::String readingText() const;

    /** One frame's work. The timer calls it, and so do the offline renders. Returns true while anything is moving. */
    bool tick (double seconds);

    bool isAdvancedOpen() const noexcept;
    void setAdvancedOpen (bool);

    /** A note's click and its right-click, as the mouse does them. */
    void clickNote (int pitchClass);
    void setNoteMode (int pitchClass, int mode);
    int noteMode (int pitchClass) const noexcept;

    PitchDisplay& getDisplay() noexcept  { return display; }
    void previewHint (const juce::String& paramId);
    bool isFastTimer() const noexcept  { return fast; }

    static constexpr int drawerHeight = 66;

private:
    class NoteCell;
    class AdvancedHeader;

    void timerCallback() override;
    void updateNotes();
    void updateFollow();
    void updateAdvancedSummary();
    void showHintFor (juce::Component*);
    void setHint (const juce::String&);

    PluginProcessor& processorRef;
    juce::SharedResourcePointer<ui::Assets> assets;   // fonts live as long as any window
    ui::LookAndFeel lookAndFeel;

    PitchDisplay display;
    ui::PresetBox presets;
    ui::Pill bypass, followKey;
    ui::MenuField key, scale;
    ui::Segments inputType;
    ui::BarSlider retune, humanize, flex, vibrato;
    ui::BarSlider transpose, reference, tracking, throat;
    ui::Pill formant;

    std::vector<std::unique_ptr<NoteCell>> notes;
    std::vector<std::unique_ptr<juce::ParameterAttachment>> noteAttachments;
    std::array<int, 12> modes {};
    int shownTarget = -1;

    std::unique_ptr<AdvancedHeader> advancedHeader;
    juce::Rectangle<int> titleArea, notesLabelArea, hintArea;
    juce::String hintText;
    bool constructed = false, fast = false;
    int slowCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneEditor)
};
