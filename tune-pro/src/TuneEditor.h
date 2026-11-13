#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "window/Theme.h"
#include "window/Controls.h"
#include "window/PitchView.h"
#include "window/KeyboardView.h"

/** The window. The pitch display and the keyboard run edge to edge as one instrument, with the controls under them.
    The controls are laid out once at design size and scaled as a whole. The display and keyboard sit at whole pixels so the scrolling image never needs resampling.
    macOS joins every repaint of a frame into one rectangle, so anything that does not move each frame is kept in a cached image. Otherwise it would all be drawn again.
    All of it reads the processor through atomics and writes only through real parameters, so nothing here can block audio. */
class TuneEditor final : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    static constexpr int designWidth = 1120, designHeight = 720;

    explicit TuneEditor (PluginProcessor&);
    ~TuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** One display frame. The screen's refresh drives this normally. The offline renders call it directly. */
    void tick (double dtSeconds);

    /** Opens or closes Advanced. Without animation it is there straight away. */
    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept  { return processor.advancedOpen.load (std::memory_order_relaxed); }

    /** For the offline renders, which have no mouse. Lights a control and shows its hint. */
    void previewHover (const juce::String& paramId);

    // What the reading shows, for tests.
    juce::String readingText() const;
    juce::String headerWords() const;      // the words in the header beside the note, e.g. "On pitch"
    juce::String headerNote() const;       // the big note in the header
    float getAdvancedAmount() const noexcept  { return advancedAmount; }
    tuneui::PresetBox& getPresetBox() noexcept  { return panel.presets; }
    KeyboardView& getKeyboard() noexcept  { return keyboard; }
    PitchView& getPitchView() noexcept    { return pitch; }
    int getNumControls() const noexcept;
    juce::Component& getPanel() noexcept  { return panel; }
    juce::Component* findControl (const juce::String& id);

    /** Whether frames are being driven by the screen right now. Off when nothing has moved for a second. */
    bool isRunningFrames() const noexcept { return vblank != nullptr; }

private:
    class Reading;
    class AdvancedButton;

    struct Panel final : public juce::Component
    {
        Panel (TuneEditor&, PluginProcessor&);
        ~Panel() override;
        void paint (juce::Graphics&) override;
        void layout (float advancedAmount);
        int countChanges() const;

        TuneEditor& owner;
        PluginProcessor& processor;
        std::unique_ptr<AdvancedButton> advancedButton;
        tuneui::PresetBox presets;
        tuneui::ParamMenu key, scale;
        tuneui::ParamToggle followKey;
        tuneui::ParamSlider retune, flex, humanize, naturalVibrato, mix;
        tuneui::ParamToggle bypass;
        tuneui::ParamSegments engine;
        juce::Label latency, hintLine;

        struct AdvancedArea final : public juce::Component
        {
            void paint (juce::Graphics&) override;
            juce::Rectangle<int> voiceTitle, tuningTitle, vibratoTitle, midiTitle;
        } advanced;

        tuneui::ParamMenu inputType, midiOctave;
        tuneui::ParamSlider throat, transpose, tracking, reference, vibratoAmount, vibratoRate, vibratoDelay;
        tuneui::ParamToggle formantPreserve, classic, midiTargets;

        int keyboardTop = 0, keyboardBottom = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };

    void timerCallback() override;
    void placeInstrument();
    void showHint (const juce::String&);
    void hintFromComponent (juce::Component*);

    static constexpr double maxFramesPerSecond = 30.0;
    static constexpr int headerHeight = 70;

    PluginProcessor& processor;
    juce::SharedResourcePointer<tuneui::Assets> assets;   // held for the window's life, so the fonts are loaded once
    tuneui::LookAndFeel lookAndFeel;
    PitchView pitch;
    KeyboardView keyboard;
    Panel panel;
    std::unique_ptr<Reading> reading;
    float advancedAmount = 0.0f;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0, stillFor = 0.0;
    int slowCountdown = 0;
    bool constructed = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneEditor)
};
