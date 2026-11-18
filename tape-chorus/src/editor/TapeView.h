#pragma once
#include "../PluginProcessor.h"
#include "Theme.h"

namespace ui
{
    /** The hero: two reels with the voices running between them.

        Each line is one read head's real delay, read from the engine: it moves with Rate and Depth, shakes with Wow, Flutter and
        Random, breaks where Degrade pulls a voice down, and the two colours part as Spread opens. The bars behind are what is really
        being written to the tape, so Feedback shows as repeats after a phrase. The left reel winds out with the input level, the right
        with the output. The tape stops when there is nothing to show, and with no audio running at all it draws what the settings will do. */
    class TapeView final : public juce::Component,
                           public Hinted
    {
    public:
        explicit TapeView (PluginProcessor&);
        ~TapeView() override;

        /** One display frame. Returns false when nothing moved and nothing was repainted. */
        bool tick (double dtSeconds);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** Says something new for the hint line, when the mouse moves between the tape and a reel. */
        std::function<void (const juce::String&)> onHintChanged;

        juce::Point<float> leftReelCentre() const noexcept  { return { leftX, reelY }; }
        juce::Point<float> rightReelCentre() const noexcept { return { rightX, reelY }; }

        int getPaintCount() const noexcept  { return paintCount; }
        bool isTapeRunning() const noexcept { return mode == Mode::live; }
        bool hasAudioRun() const noexcept   { return everRan; }

    private:
        enum class Mode { nothing, preview, live };
        enum class Zone { tape, leftReel, rightReel };

        Zone zoneAt (juce::Point<float>) const;
        void endDrag();

        void rebuild (const PluginProcessor&, bool fromEngine);
        void addLine (juce::Path&, const float* ys, const uint8_t* on, int count);
        float pinchAt (float x) const noexcept;
        float wantedScaleMs() const;
        uint32_t settingsFingerprint() const;
        void updateCaptions();
        void paintReel (juce::Graphics&, float centreX, float amount, bool over, float angle) const;

        PluginProcessor& processor;
        juce::RangedAudioParameter* dragX = nullptr, * dragY = nullptr;
        float dragXStart = 0.0f, dragYStart = 0.0f;
        std::array<std::atomic<float>*, 11> watched {};

        Mode mode = Mode::nothing;
        uint32_t lastCount = 0, lastFingerprint = 0;
        double position = 0.0, sinceChange = 100.0, silentFor = 100.0, staleFor = 100.0;
        bool everRan = false, needsPreview = true;
        float seconds = 2.8f, scaleMs = 6.0f, scaleShownMs = 6.0f;

        float reelRadius = 90.0f, reelY = 200.0f, leftX = 120.0f, rightX = 960.0f, centreY = 200.0f, halfHeight = 180.0f;
        float angle = 0.0f, inLevel = 0.0f, outLevel = 0.0f;
        bool inOver = false, outOver = false, bypassed = false;

        juce::Path leftPath, rightPath;
        juce::RectangleList<float> barRects;
        juce::Path teeth;
        std::vector<float> xs, ys, pinch;
        std::vector<uint8_t> on;
        juce::String scaleCaption, timeCaption;
        int paintCount = 0;

        static constexpr int captionBand = 18;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeView)
    };
}
