#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>

/** The live cue, and the hero's right half. Your voice and the voice coming out, each one's real spectrum, back to back with low notes
    at the bottom. Leave the voice alone and you get a perfect ink blot, because the two really are the same. Move the dot and the
    reflection stops matching: Pitch slides the teeth (the harmonics), Formant Shift slides the humps, Whisper melts the teeth, Monster
    puts new teeth between them and Robot straightens them. Both sides are measured from the audio itself, through a read that cannot
    touch it. The line across the middle joins the note going in to the note coming out. */
class Mirror final : public juce::Component,
                     public ui::Hinted
{
public:
    explicit Mirror (PluginProcessor&);
    ~Mirror() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** One frame. Returns true while anything is moving. */
    bool advance();

    bool isVoiced() const noexcept  { return voiced; }
    double getYouHz() const noexcept  { return youHz; }
    double getEgoHz() const noexcept  { return egoHz; }
    float getOutputLevel() const noexcept  { return outputLevel; }

    /** How far the voice is from you right now, in semitones, straight from the engine's own readings. Only means something while voiced. */
    double getLiveShift() const noexcept  { return liveShift; }

    static constexpr int numPoints = 176;
    static constexpr double lowestHz = 50.0, highestHz = 12000.0;
    float youAt (int point) const noexcept  { return youShape[(size_t) point]; }
    float egoAt (int point) const noexcept  { return egoShape[(size_t) point]; }

    int getPaintCount() const noexcept  { return paints; }
    double getPaintSeconds() const noexcept  { return paintSeconds; }
    double getPaintedArea() const noexcept  { return paintedArea; }

private:
    class Readout;

    void prepareAnalysis (double sampleRate);
    void measure (long long position);
    void buildPaths();
    void drawLayer (juce::Graphics&) const;
    float yFor (double hz) const noexcept;

    PluginProcessor& processorRef;
    std::unique_ptr<Readout> youReadout, egoReadout;

    // Analysis. Everything is sized in prepareAnalysis, on this thread.
    int fftSize = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> hann, grabbed, spectrum;
    struct Band { int from, to; float low, high, weight, tilt; };
    std::array<Band, numPoints> bands {};
    double analysedRate = 0.0;
    long long lastPosition = -1;
    int quietFrames = 1000, staleFrames = 1000;

    std::array<float, numPoints> youWanted {}, egoWanted {}, youShape {}, egoShape {};
    std::array<float, numPoints> youPainted {}, egoPainted {};
    double youHz = 0.0, egoHz = 0.0, youShownHz = 0.0, egoShownHz = 0.0, liveShift = 0.0;
    float bridgeYou = -1.0f, bridgeEgo = -1.0f, paintedBridgeYou = -1.0f, paintedBridgeEgo = -1.0f;
    float outputLevel = 0.0f;
    bool voiced = false, heard = false, bypassed = false;
    int readoutCounter = 0, quietVoiceFrames = 1000;

    juce::Rectangle<float> plot;
    float axisX = 0.0f, reach = 0.0f;
    static constexpr float gutter = 36.0f;
    juce::Path youPath, egoPath;
    bool pathsValid = false;

    juce::Image layer;
    float layerScale = 0.0f;
    bool layerValid = false;

    int paints = 0;
    double paintSeconds = 0.0, paintedArea = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mirror)
};
