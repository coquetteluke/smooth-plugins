#pragma once
#include "../PluginProcessor.h"
#include "Controls.h"
#include "HitTracker.h"
#include <vector>

namespace punchview
{
    /** The hero: the last hit, drawn large, before and after. White is the sound as it came in, orange as it goes out, dark orange what the settings took away. */
    class HitView final : public juce::Component,
                          public ui::Hinted
    {
    public:
        enum class Zone { none, start, tail };
        enum class Showing { example, real, preview };

        explicit HitView (PluginProcessor&);

        /** One bin of level from the tap. True if a new hit starts on it. */
        bool feed (float before, float after);

        /** What to draw now. quiet means nothing is playing, so a change of settings can be previewed on the hit in view. True if the picture changed. */
        bool update (const Settings&, bool quiet);

        void setZone (Zone);
        Showing getShowing() const noexcept  { return showing; }
        const HitTracker& getTracker() const noexcept  { return tracker; }
        juce::String captionText() const;
        static juce::String captionFor (Showing);

        /** The plot inside this component, and where 30 ms falls on it, which is where Attack's column ends. */
        juce::Rectangle<int> getPlot() const noexcept  { return plot; }
        int getSplitX() const noexcept;

        int getPaintCount() const noexcept     { return paintCount; }
        double getPaintSeconds() const noexcept { return paintSeconds; }

        const float* shownBefore() const noexcept  { return drawBefore; }
        const float* shownAfter() const noexcept   { return drawAfter; }
        int shownLength() const noexcept           { return drawLength; }

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        float ceilingLineY() const noexcept;
        bool isNearCeilingLine (float y) const noexcept;

    private:
        void rebuildStatic (float scale);
        void rebuildCurves();
        void rebuildGhost();
        void chooseWhatToDraw (const Settings&, bool quiet);
        void setCeilingFromY (float y);
        float valueAtColumn (const float* values, int column, int length) const noexcept;

        PluginProcessor& processor;
        HitTracker tracker;
        Preview preview;
        juce::ParameterAttachment ceilingAttachment;

        std::array<float, hitBins> exampleBefore {}, previewAfter {};
        const float* drawBefore = nullptr;
        const float* drawAfter = nullptr;
        int drawLength = 0;
        Showing showing = Showing::example;
        Settings settings, settingsAtHit, previewedWith;
        bool previewValid = false, newHit = false, curvesDirty = true, ghostDirty = false, cutsAnything = false, movedNow = false;

        Zone zone = Zone::none;
        juce::Rectangle<int> plot;
        std::vector<float> columnBin;
        juce::Image staticLayer;
        float staticScale = 0.0f;
        bool staticDirty = true, overLine = false, draggingLine = false;
        juce::Path ghostPath, emberPath, hotPath, linePath;
        std::vector<juce::Point<float>> linePoints, afterPoints;
        int paintCount = 0;
        double paintSeconds = 0.0;
        const juce::String restingHint;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HitView)
    };

    /** The last few seconds as a row of bars. The bars of the hit shown below it are the bright ones. */
    class TimelineView final : public juce::Component,
                               public ui::Hinted
    {
    public:
        TimelineView();

        void feed (float before, float after);
        void markHit() noexcept;

        /** True if the picture moved. */
        bool update();
        void paint (juce::Graphics&) override;

    private:
        struct Bar { float before = 0.0f, after = 0.0f; };
        static constexpr int numBars = 600;
        static constexpr int barsPerHit = hitBins / barBins;

        std::array<Bar, (size_t) numBars> bars;
        unsigned int written = 0, hitBar = 0xffffffffu, paintedAt = 0;
        int binsInBar = 0, barsSinceSound = 100000;
        bool anyHit = false;
        float peakBefore = 0.0f, peakAfter = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
    };

    /** In and Out on the plot's level scale, then how far Impact and the Ceiling are pulling down, hanging from the top. */
    class MeterPanel final : public juce::Component,
                             public ui::Hinted
    {
    public:
        MeterPanel();

        /** In dB, reductions as positive numbers. True if anything moved on screen. */
        bool setLevels (float inDb, float outDb, float impactDown, float ceilingDown);
        void setCeiling (float db, bool on);

        std::array<juce::Rectangle<int>, 4> getLanes() const;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        static constexpr float reductionRange = 24.0f;
        static constexpr int preferredWidth = 220;

    private:
        void rebuildStatic (float scale);
        juce::String hintAt (int x) const;

        std::array<float, 4> shown { -100.0f, -100.0f, -100.0f, -100.0f };   // in pixels
                float ceilingDb = -1.0f;
        bool ceilingOn = false;
        juce::Image staticLayer;
        float staticScale = 0.0f;
        bool staticDirty = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterPanel)
    };

    /** The four readings as numbers, under their lanes. */
    class ReadoutRow final : public juce::Component,
                             public ui::Hinted
    {
    public:
        ReadoutRow();
        void setLanes (const std::array<juce::Rectangle<int>, 4>& lanesInPanel);
        void setValues (float inDb, float outDb, float impactDown, float ceilingDown, bool ceilingOn);
        juce::String textOf (int index) const  { return texts[(size_t) index]; }
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

    private:
        std::array<juce::Rectangle<int>, 4> lanes;
        std::array<juce::String, 4> texts;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReadoutRow)
    };

    juce::String minusText (float value, int places);
}
