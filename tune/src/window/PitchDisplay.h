#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

// The hero. On top, the note being hit. Under it, a ruler of cents away from that note shows right now: a bone mark where the voice
// was sung, a lime mark where it comes out, and the ticks between them lit, which is the correction. Under the ruler, a short trace of
// the last few seconds on the same scale: the centre line is the note, and the fill between the two lines is what got moved.
class PitchDisplay final : public juce::Component,
                           public ui::Hinted
{
public:
    explicit PitchDisplay (PluginProcessor&);

    /** Reads the processor and asks for only what changed to repaint. Returns true while anything on screen is moving. */
    bool advance (double seconds);

    void paint (juce::Graphics&) override;
    void resized() override;

    int getPaintCount() const noexcept  { return paintCount; }

    /** The ruler's map from cents to a position, 0 to 1. Exposed for the tests. */
    static float positionForCents (float cents) noexcept;

private:
    void rebuildStatic (float scale);
    void prepareTrace (float scale);
    void writeNewRows();
    void emitColumn (bool voicedHere, float sungY, float outY);
    float traceY (float cents) const noexcept;
    float rulerX (float cents) const noexcept;
    void updateText (const PluginProcessor::Readings&);
    void paintNote (juce::Graphics&);
    void paintRuler (juce::Graphics&);
    void paintSung (juce::Graphics&);
    void paintCorrection (juce::Graphics&);
    void paintLevels (juce::Graphics&);

    static constexpr int numTicks = 81;
    static constexpr float pointsPerPoint = 0.75f;   // window points of scroll per history point, which is 5 ms: 150 a second

    PluginProcessor& processor;
    std::atomic<float>* bypassValue, *transposeValue, *flexValue;

    // Where things are, in window points.
    juce::Rectangle<int> noteArea, leftArea, rightArea, rulerArea, traceArea, inArea, outArea;
    float noteSize = 110.0f;

    // Cached layers, in real pixels.
    juce::Image staticLayer, fadeLayer, ring;
    float staticScale = 0.0f, ringScale = 0.0f, flexDrawn = -1.0f;
    int ringHead = 0;                 // the next column to write. Newest is just before it
    long long ringDone = 0, columnsTotal = 0, noteStartColumn = 0;
    double columnsPerPoint = 1.0, columnCredit = 0.0;
    int tracePoints = 1000;           // history points that fit across the trace
    bool noteLabelled = true, hadColumn = false;
    float lastSungY = 0.0f, lastOutY = 0.0f, lastInPoint = 0.0f, lastOutPoint = 0.0f;
    int spanNote = -1;

    // What is shown.
    float sungCents = 0.0f, outCents = 0.0f, shownSung = 0.0f, shownOut = 0.0f;
    float inLevel = 0.0f, outLevel = 0.0f;
    int shownNote = -1;
    double heldFor = 1.0e9, textTimer = 0.0, staleFor = 0.0;
    bool voiced = false, bypassed = false, locked = false, noTargets = false, markersShown = false;
    long long seenCount = 0, lastVoicedPoint = -1000000, shownCount = 0;
    juce::String sungValue, sungSub, fixValue, fixSub;
    juce::Colour noteColour = ui::tone::faint;
    int paintCount = 0;
};
