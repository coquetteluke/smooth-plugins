#pragma once
#include "../PluginProcessor.h"
#include "Theme.h"
#include <array>

namespace ui
{
    /** The hero: the vocal tract seen from the side, as one bent tube from the cords up and forward to the lips.
        Bone is the singer's throat, read live from the voice's formants. Oxide is the model: that same live shape with the five
        points, Width and Length applied. Gray is the realistic range. Under the bend sit the formants themselves, before and after.

        The audio is only ever read, through atomics. A drag writes real parameters with begin and end gestures, and everything
        drawn from a parameter is read back from the parameter, so automation moves the same picture. */
    class TractView final : public juce::Component
    {
    public:
        static constexpr int numPoints = 5;

        explicit TractView (PluginProcessor&);
        ~TractView() override;

        /** One display frame. Repaints only if something moved by more than a quarter of a pixel. */
        void advance (double dtSeconds);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** Lights a point from outside, when its numbers are hovered. -1 for none. */
        void setHighlight (int point);
        int getHighlight() const noexcept  { return lit; }

        std::function<void (int)> onPointHovered;

        /** Where a point's handle is drawn, in this component's pixels. Public for the offline renders and tests. */
        juce::Point<float> handlePosition (int point) const;
        /** Where the point really is, which can be outside the view: handlePosition() holds the drawn handle 12 px inside. */
        juce::Point<float> handleAt (int point) const;

        // For the live CPU measurement.
        int getPaintCount() const noexcept          { return paintCount; }
        double getPaintSeconds() const noexcept     { return paintSeconds; }

        static const char* pointName (int);
        static juce::String pointHint (int);

    private:
        class Numbers;

        static constexpr int numSamples = 64;   // along each wall
        static constexpr int numEnvelope = formant::FormantEngine::Analysis::numPoints;
        static constexpr int numFormants = 5;

        struct Tube
        {
            std::array<juce::Point<float>, numSamples> centre, outward;
            float lengthDrawn = -1.0f;
        };

        float place (int point) const;
        float width (int point) const;
        void readParameters();
        void placeTube (Tube&, float lengthDrawn) const;
        void readAudio (double dt);
        void buildShapes();
        void buildLayers (float scale);
        void paintStaticLayer (juce::Graphics&);
        void paintOverlay (juce::Graphics&);
        int pointAt (juce::Point<float>) const;
        void setHover (int);
        void dragPointTo (juce::Point<float>);

        juce::Point<float> spineAt (float u, float lengthDrawn, juce::Point<float>* outward = nullptr) const;
        float radiusFor (float u, float logWidth) const noexcept;
        float innerLimit (float u, float lengthDrawn) const noexcept;
        float settingLogWidth (float u) const noexcept;
        float modelFormant (float hz) const noexcept;
        float xForHz (float) const noexcept;
        float yForDb (float) const noexcept;

        PluginProcessor& processor;
        std::unique_ptr<Numbers> numbers;

        // What the parameters say, polled once a frame so that any writer moves the picture.
        std::array<float, numPoints> widths {}, places {};
        float lengthParam = 1.0f, widthParam = 1.0f;
        bool bypassed = false, layersStale = true, overlayStale = true;
        std::array<float, numSamples> settingLog {};
        std::vector<float> modelMap;

        // What the voice is doing, smoothed.
        std::array<float, numFormants> shapeAmount {};
        std::array<std::array<float, numFormants>, numSamples> shapeBasis {};
        std::array<float, numEnvelope> inputDb {}, outputDb {};
        std::array<float, numFormants> formants {}, formantAge {};   // Held across a brief gap, so the numbers and the shape do not flicker.
        float pitch = 0.0f, pitchAge = 10.0f, sizeLog = 0.0f, voicing = 0.0f, levelDb = -60.0f, levelShown = -60.0f, heat = 0.0f, plotTop = 30.0f;

        Tube singerTube, modelTube;
        std::array<float, numSamples> singerRadius {}, modelRadius {}, drawnSinger {}, drawnModel {};
        std::array<float, numEnvelope> drawnIn {}, drawnOut {};
        float drawnMeter = -1.0f, drawnHeat = -1.0f;
        bool anyDrawn = false;

        juce::Path modelPath, singerPath, curvesIn, curvesOut;
        juce::Image staticLayer, overlay;
        float layerScale = 0.0f;

        int lit = -1, hover = -1, dragging = -1;
        juce::Point<float> dragLast, dragTarget;

        double numbersWait = 0.0;
        int paintCount = 0;
        double paintSeconds = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TractView)
    };
}
