#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"
#include "window/Controls.h"
#include "window/TapeDisplay.h"

// The window. Age is the big idea, one ribbon from a fresh reel to a worn cassette, under a live picture of what the tape is doing.
// The main controls sit under it and the rest open in sections below. Every control is a real AU parameter with its own ID.
// The window only ever adds its size and whether Advanced is open to the saved state.
class TapeColorEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit TapeColorEditor (PluginProcessor&);
    ~TapeColorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** One frame's work. The timer calls it, and so do the offline renders. Returns true while anything is moving. */
    bool tick (double seconds);

    bool isAdvancedOpen() const noexcept;
    void setAdvancedOpen (bool);

    ui::ResponseView& getResponse() noexcept  { return response; }
    ui::CurveView& getCurve() noexcept        { return curve; }
    ui::Reel& getReel() noexcept              { return reel; }
    bool isFastTimer() const noexcept         { return fast; }

    void previewHint (const juce::String& paramId);
    void startTypingAge();
    void hoverTile (const juce::String& paramId, bool shouldShow);
    void startTypingTile (const juce::String& paramId);

    static constexpr int drawerHeight = 170;

private:
    class AgeValue;
    class AdvancedHeader;

    void timerCallback() override;
    void updateSettings();
    void updateSummary();
    void updateRestingTiles();
    void readAudio (double seconds);
    void showHintFor (juce::Component*);
    void setHint (const juce::String&);
    juce::String presetNameNow() const;

    PluginProcessor& processorRef;
    juce::SharedResourcePointer<ui::Assets> assets;   // fonts live as long as any window
    ui::LookAndFeel lookAndFeel;

    ui::ResponseView response;
    ui::CurveView curve;
    ui::Reel reel;
    ui::AgeRibbon ribbon;
    ui::PresetBox presets;
    ui::Pill autoLevel, bypass;

    ui::Tile drive, bias, noise, wow, flutter, mix, output;
    ui::Segments speed;

    ui::Tile hardness, texture, dropDepth, dropRate;
    ui::Segments compander;
    ui::Tile encode, decode;
    ui::Tile lowCut, highCut, spacing, azimuth;
    ui::ChoiceTile device;
    ui::Tile deviceMix;
    ui::Tile randomDrift, stereoDrift;

    std::unique_ptr<AgeValue> ageValue;
    std::unique_ptr<AdvancedHeader> advancedHeader;

    juce::Rectangle<int> titleArea, hintArea, readoutArea, ribbonEnds;
    juce::Rectangle<int> tapeLabel, motionLabel, outLabel;
    juce::Rectangle<int> surfaceLabel, companderLabel, playbackLabel, deviceLabel, driftLabel;
    juce::String hintText, topEndText, hissText, wobbleText;
    juce::Image readoutPicture;

    std::vector<float> lastValues;
    std::vector<std::atomic<float>*> values;
    tapecolor::display::Settings settings;
    float usedBoost = 0.0f;
    double sinceBoostUpdate = 0.0;

    // Reading the sound. All of it stays here on the message thread.
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<juce::dsp::Complex<float>> fftIn, fftOut;
    std::vector<float> hann;
    std::array<int, ui::ResponseView::columns> binFrom {}, binTo {};
    std::array<float, ui::ResponseView::columns> binBlend {}, tilt {};
    ui::ResponseView::Column inDb, outDb;
    double columnsRate = 0.0;
    std::uint32_t lastWritten = 0;
    double sinceAudio = 10.0, lastTime = 0.0, sinceMoved = 10.0, textAge = 0.0;
    float heldPeak = 0.0f, shownBoost = -100.0f;
    bool signalPresent = false;

    bool constructed = false, fast = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeColorEditor)
};
