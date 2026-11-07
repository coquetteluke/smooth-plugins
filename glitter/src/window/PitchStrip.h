#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

/** The live pitch strip: the last five seconds, your voice in white, what comes out in magenta, and Harmony's voices in Harmony's colour.
    A flat piano on the right edge lights the scale and the note being hit.

    Everything scrolling lives in one image at the screen's own pixels, used as a ring. Each frame writes only the few new columns straight into its pixels
    and copies the image to the screen at a whole pixel offset, so its cost does not depend on how much is on screen and no path is ever stroked. */
class PitchStrip final : public juce::Component,
                         public fxui::Hinted
{
public:
    explicit PitchStrip (PluginProcessor&);

    /** One display frame, from the editor. Returns false when nothing on screen moved. */
    bool advance (double dtSeconds);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Window scale, for type sizes. The trace itself always works in physical pixels. */
    void setTypeScale (float s)  { typeScale = s; resized(); }

    float getLowestNote() const noexcept  { return viewLow - 0.5f; }
    float getHighestNote() const noexcept { return viewLow - 0.5f + (float) span; }
    int getPaintCount() const noexcept    { return paintCount; }

    /** The newest output note, or -1 with no voice. */
    int getOutputNote() const noexcept    { return outputShown; }

    static constexpr double secondsShown = 5.0;
    static constexpr int pointsPerSecond = 200;
    static constexpr int pointsShown = (int) (secondsShown * pointsPerSecond);

private:
    int pianoWidth() const noexcept         { return juce::roundToInt (30.0f * typeScale); }
    juce::Rectangle<int> traceArea() const  { return getLocalBounds().withTrimmedRight (pianoWidth() + juce::roundToInt (3.0f * typeScale)); }

    void followPitch (long long count);
    bool scaleChanged();
    void bringRingUpToDate (float pixelScale);
    void rebuildLanes();
    void drawColumn (juce::Image::BitmapData&, long long column, long long count);
    template <typename Read>
    void drawLineInColumn (juce::Image::BitmapData&, int ringX, long long column, long long oldest, long long count, Read, juce::Colour, float thickness);
    void paintPiano (juce::Graphics&);

    long long columnOf (long long point) const noexcept  { return (long long) std::floor ((double) point * pixelsPerPoint); }
    float yOf (float note) const noexcept                { return (float) ringHeight * (1.0f - (note - viewLow + 0.5f) / (float) span); }

    PluginProcessor& processor;
    float typeScale = 1.0f;

    // The ring. A column here is a physical pixel, numbered from the first point ever written, and wraps at ringWidth.
    juce::Image ring;
    int ringWidth = 0, ringHeight = 0, visibleColumns = 0;
    double pixelsPerPoint = 1.0;
    float ringScale = 0.0f;
    long long drawnCount = -1;
    bool wholeRingStale = true;
    std::vector<juce::PixelARGB> laneColours;
    std::array<juce::Path, 4> corners;
    juce::Image pianoImage;
    float pianoScale = 0.0f;
    bool pianoStale = true;

    float viewLow = 48.0f, viewTarget = 48.0f;
    int span = 19;
    std::uint16_t scaleShown = 0x0FFF;

    long long lastSeenCount = -1;
    int voicedOnScreen = 0, outputShown = -1;
    int paintCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchStrip)
};
