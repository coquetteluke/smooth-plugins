#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../ResonanceEngine.h"
#include "Theme.h"

class SmoothResonanceProcessor;

namespace ui
{
    /** The hero: the voice's spectrum rising from the floor, gold reduction hanging from the ceiling, and the
        sensitivity curve with its dots across the middle.

        Runs on the message thread only. Data comes from the processor's lock free readGraph(). Dots and range handles
        write real parameters with begin and end gesture, and are drawn from those parameters every frame, so automation
        moves them. Nothing about the sound is kept here. */
    class ResonanceGraph final : public juce::Component
    {
    public:
        explicit ResonanceGraph (SmoothResonanceProcessor&);
        ~ResonanceGraph() override;

        /** Moves the display on by dt seconds. Returns true if anything visible changed and a repaint is needed. */
        bool advance (double dt);

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        int getSelectedBand() const noexcept  { return selectedBand; }
        void setSelectedBand (int);

        /** Lights the part of the graph a side control is about. Empty for none. */
        void setHighlight (const juce::String& paramId);

        std::function<void (int)> onBandSelected;
        std::function<void (const juce::String&)> onHint;

        float getPeakDb() const noexcept  { return peakDb; }

        /** Times the graph has been drawn. For measuring the real frame rate. */
        int getPaintCount() const noexcept  { return paintCount; }

        /** Where the axes go, so that the static labels around the graph line up. */
        float xForHz (float hz) const noexcept;
        float yForReductionLine (float db) const noexcept;

    private:
        static constexpr int numPoints = smoothres::GraphFrame::numPoints;
        static constexpr int numDense = numPoints * 2 - 1;
        static constexpr int numBands = smoothres::Params::numBands;
        static constexpr int numSensSteps = 221;

        enum class Grab { none, band1, band2, band3, low, high };

        struct BandParams { juce::RangedAudioParameter* freq, * sens, * width; };

        float hzForX (float x) const noexcept;
        float yForSensitivity (float db) const noexcept;
        float sensitivityForY (float y) const noexcept;
        float yForLevel (float db) const noexcept;
        float yForReduction (float db) const noexcept;
        int denseIndexForX (float x) const noexcept;

        void buildTargets();
        Grab grabAt (juce::Point<float>) const;
        void updateHover (juce::Point<float>);
        void endGesture();
        juce::String tagFor (Grab) const;

        void refreshVoicePicture (float scale, float lowX, float highX);
        void drawVoice (juce::Graphics&, float lowX, float highX);
        void drawGrid (juce::Graphics&);
        void drawGold (juce::Graphics&);
        void drawSensitivity (juce::Graphics&, float lowX, float highX);
        void drawHandles (juce::Graphics&);
        void drawOverlays (juce::Graphics&);
        void drawTag (juce::Graphics&, juce::Point<float> anchor, const juce::String&, juce::Colour text);

        // JUCE strokes a path by building a new one with round joins, which is too slow for every frame of a Retina canvas,
        // so lines are ribbons built here in one pass, and only over the part of the graph that needs each colour.
        // These paths keep their memory between frames.
        void buildRibbon (const float* ys, const float* xs, int count, float halfWidth);
        void fillRangedRibbon (juce::Graphics&, const float* ys, const float* xs, int count, int lowIndex, int highIndex, float halfWidth, juce::Colour inside, juce::Colour outside);
        void upsample (const std::array<float, numPoints>& from, std::array<float, numDense>& to) const;

        SmoothResonanceProcessor& processor;
        BandParams bands[numBands];
        juce::RangedAudioParameter* lowParam, * highParam, * depthParam, * mixParam, * deltaParam, * bypassParam;

        smoothres::GraphFrame frame;

        // Point positions and display values. Everything visual glides toward its target, so 47 graph frames a second
        // read as continuous motion at the screen's rate.
        std::array<float, numPoints> pointX {};       // 0 to 1 across the graph
        std::array<float, numPoints> pointHz {};
        std::array<float, numPoints> tilt {};         // display only: lifts the highs, so a voice reads as level
        std::array<float, numPoints> scratch {};
        std::array<float, numPoints> targetLevel {}, targetCut[2] {};
        std::array<float, numPoints> shownLevel {}, shownCut[2] {};
        std::array<float, numPoints> yLevel {}, yAfter {}, yCut[2] {};
        std::array<float, numDense> denseX {}, yVoice {}, yShaved {}, yGold[2] {}, normalX {}, normalY {};
        std::array<float, numSensSteps> sensX {}, sensY {};
        juce::Path ribbon, shape, corners;

        // The voice's colours as one finished picture. Only redrawn when Low, High, Delta or the size changes.
        juce::Image voicePicture;
        float pictureScale = 0.0f, pictureLowX = -1.0f, pictureHighX = -1.0f, pictureDelta = -1.0f;

        double staleSeconds = 10.0;
        float bypassShown = 0.0f, deltaShown = 0.0f, hintShown = 1.0f, peakDb = 0.0f;
        bool slotsApart = false;
        juce::uint32 paramStamp = 0;
        int paintCount = 0;

        int selectedBand = 1;
        Grab hover = Grab::none, drag = Grab::none;
        juce::Point<float> mousePos { -1.0f, -1.0f }, logicalPos, lastDrag;
        float dragStartY = 0.0f, widthAtDragStart = 1.0f;
        bool mouseInside = false, widthDrag = false;
        juce::String highlight;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceGraph)
    };
}
