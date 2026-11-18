#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

/** The big live display: the last eight seconds of pitch on a lane per note, the voice as sung in white and the corrected voice in coral,
    with the gap between them filled and the note being hit shown as a block in its lane.

    Everything scrolling lives in one image at the screen's own pixels, used as a ring. Each frame draws only the few new columns and copies the
    image to the screen at a whole pixel offset, so its cost does not depend on how much is on screen and nothing is rebuilt per frame. */
class PitchView final : public juce::Component,
                        public tuneui::Hinted
{
public:
    explicit PitchView (PluginProcessor&);

    /** One display frame, from the editor. Returns false when nothing on screen moved. */
    bool advance (double dtSeconds);
    /** Set by the editor when no audio has arrived for a while: nothing is shown as being sung then. */
    void setStale (bool s) noexcept  { stale = s; }

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Window scale, for type sizes. The trace itself always works in physical pixels. */
    void setTypeScale (float s);

    float getLowestNote() const noexcept  { return viewLow - 0.5f; }
    float getHighestNote() const noexcept { return viewLow - 0.5f + (float) span; }
    int getPaintCount() const noexcept    { return paintCount; }
    juce::String getFollowText() const    { return legend.follow; }   // tests: the legend's Follow Key line

    static constexpr double secondsShown = 8.0;
    static constexpr int pointsPerSecond = 200;
    static constexpr int pointsShown = (int) (secondsShown * pointsPerSecond);

private:
    int gutterWidth() const noexcept        { return juce::roundToInt (46.0f * typeScale); }
    void layoutChildren();
    juce::Rectangle<int> traceArea() const  { return getLocalBounds().withTrimmedRight (gutterWidth()); }

    void followPitch (long long count);
    bool laneStateChanged();
    void bringRingUpToDate (float pixelScale);
    void drawColumns (juce::Graphics&, long long fromX, long long toX, long long count);
    void drawPoints (juce::Graphics&, long long firstPoint, long long lastPoint, long long count, bool bypassed);
    void strokeLine (juce::Graphics&, long long firstPoint, long long lastPoint, float (*read) (const PluginProcessor::PitchPoint&) noexcept, juce::Colour, float width);

    /** Which line is which, with the live pitch beside each. Opaque and apart from the trace, so its text is only drawn when the numbers change. */
    struct Legend final : public juce::Component
    {
        Legend() { setOpaque (true); setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics&) override;
        juce::String voice, corrected, follow;
        bool bypassed = false;
        float scale = 1.0f;
    } legend;

    /** The note names down the right edge. Cached like the legend, and only the lanes that change are drawn again. */
    struct Gutter final : public juce::Component
    {
        explicit Gutter (PitchView& o) : owner (o) { setOpaque (true); setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics&) override;
        juce::Rectangle<float> cellFor (int note) const noexcept;
        PitchView& owner;
    } gutter { *this };

    long long columnOf (long long point) const noexcept  { return (long long) std::floor ((double) point * pixelsPerPoint); }
    float yOf (float note) const noexcept                { return (float) ringHeight * (1.0f - (note - viewLow + 0.5f) / (float) span); }

    PluginProcessor& processor;
    std::atomic<float>* followKeyParam = nullptr, *bypassParam = nullptr;
    float typeScale = 1.0f;

    // The ring. A column here is a physical pixel, numbered from the first point ever written, and wraps at ringWidth.
    juce::Image ring;
    int ringWidth = 0, ringHeight = 0, visibleColumns = 0;
    double pixelsPerPoint = 1.0;
    float ringScale = 0.0f;
    long long drawnCount = -1;
    bool wholeRingStale = true;
    juce::Path line;

    // What the lanes show
    float viewLow = 48.0f, viewTarget = 48.0f;
    int span = 19;
    std::uint16_t scaleShown = 0x0FFF, removedShown = 0, bypassedShown = 0;

    // What moved
    long long lastSeenCount = -1;
    int voicedOnScreen = 0;
    int targetShown = -1;
    bool bypassedNow = false, stale = false;
    double readoutClock = 1.0;
    int paintCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchView)
};
