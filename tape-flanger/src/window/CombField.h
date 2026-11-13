#pragma once
#include "../PluginProcessor.h"
#include "Theme.h"
#include <vector>

namespace ui
{
    /** The hero. Across: where the wet deck sits against the dry one, with zero dead centre. Up: frequency.

        The steel picture is the comb filter worked out from the settings, one column per delay. Its notches are the curves that
        rush up and off the top as the delay nears zero, then come back down on the far side. It is a cached image.

        The violet is real sound. Each frame the real output spectrum is painted into the field at the wet deck's real position,
        and fades. So the notches appear in the violet by themselves, and they land on the steel ones because that is what the
        audio is doing.

        In Manual, dragging across the field is riding Flange. In LFO, it sets how far the sweep goes. */
    class CombField final : public juce::Component,
                            public Hinted
    {
    public:
        explicit CombField (PluginProcessor&);
        ~CombField() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** One display frame. Reads the audio, paints the wake, and asks for only the repaint it needs. */
        void advance (double dtSeconds);

        /** While Advanced slides, the cached picture is stretched rather than worked out again each frame. */
        void setHoldPicture (bool shouldHold);

        /** Asks for the sharp picture on the next paint, without waiting for things to settle. For the offline renders. */
        void finishPicture();

        float getCrossingFlash() const noexcept { return flash; }
        float getLeadCrossing() const noexcept { return leadFlash; }
        float getWakeLevel() const noexcept { return wakePeak; }

        /** For the proof that the steel and the violet agree: dB of the prediction and of the painted sound at one delay. */
        float predictedDb (float frequencyHz, float offsetMs) const;
        float paintedAt (float frequencyHz, float offsetMs) const;

        int getPaintCount() const noexcept { return paintCount; }
        double getLastRebuildMs() const noexcept { return lastRebuildMs; }
        int getRebuildCount() const noexcept { return rebuildCount; }

        /** For the window tests: where a delay sits across the field, and one entry of the violet ramp as it is written into the picture. */
        float xForMs (float ms) const noexcept { return xOfMs (ms); }
        juce::PixelARGB getWakeColour (int level) const noexcept { return wakeColours[(size_t) juce::jlimit (0, 255, level)]; }

        static constexpr int axisHeight = 30;

    private:
        struct Settings
        {
            float feedback = 0, mix = 0, tone = 0, lowCut = 0, highCut = 0, depth = 0, delay = 0, drift = 0;
            bool invert = false, comp = true, manual = false, bypass = false;
            bool operator== (const Settings& o) const noexcept;
        };

        Settings readSettings() const;
        void updateAxis (const Settings&);
        void rebuildBase (float scale, bool draft);
        void rebuildOverlay (float scale);
        void rebuildMask();
        void rebuildTables();
        void paintSound (const PluginProcessor::Readings&, int channels);
        void renderWake();

        float uOfMs (float ms) const noexcept;
        float msOfU (float u) const noexcept;
        float xOfMs (float ms) const noexcept;
        float msOfX (float x) const noexcept;
        float yOfHz (float hz) const noexcept;
        juce::Rectangle<int> plotBounds() const noexcept { return getLocalBounds().withTrimmedBottom (axisHeight); }

        void applyDrag (const juce::MouseEvent&);
        void finishGesture();

        PluginProcessor& processor;
        juce::RangedAudioParameter& flangeParam;
        juce::RangedAudioParameter& depthParam;

        std::atomic<float>* feedbackRaw;
        std::atomic<float>* mixRaw;
        std::atomic<float>* toneRaw;
        std::atomic<float>* lowCutRaw;
        std::atomic<float>* highCutRaw;
        std::atomic<float>* depthRaw;
        std::atomic<float>* delayRaw;
        std::atomic<float>* wowRaw;
        std::atomic<float>* flutterRaw;
        std::atomic<float>* randomRaw;
        std::atomic<float>* flangeRaw;
        std::atomic<float>* invertRaw;
        std::atomic<float>* compRaw;
        std::atomic<float>* modeRaw;
        std::atomic<float>* bypassRaw;

        Settings settings, builtFrom;
        float leftMs = 5.25f, rightMs = 5.25f;

        // The cached comb picture, at the screen's real pixels, or at half of them while a parameter keeps moving.
        juce::Image base;
        bool baseDirty = true, baseIsDraft = false, holdPicture = false;
        double lastRebuild = 0.0, lastRebuildMs = 0.0, lastChange = 0.0;
        int rebuildCount = 0;

        // Scales, labels and the axis strip, which stay sharp and are only drawn again when the scale itself changes.
        juce::Image overlay;
        float overlayScale = 0.0f, overlayRight = 0.0f, overlayLow = 0.0f, overlayHigh = 0.0f;
        int overlayWidth = 0;
        bool overlayManual = false;

        // The wake: real sound painted into the field
        static constexpr int wakeColumns = 480, wakeRows = 300;
        std::vector<float> wake;              // wakeColumns x wakeRows, row 0 at the top
        std::vector<float> combMask;          // the prediction at the same grid, 0 at a notch, 1 at level or above. Keeps the fringes dark under the violet.
        juce::Image wakeImage;
        std::array<juce::PixelARGB, 256> wakeColours;
        float wakePeak = 0.0f;
        bool wakeShown = false, maskDirty = true;
        double lastMaskRebuild = 0.0;

        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> fftData, window, column[2], previous[2], tapBuffer, smoothing;
        std::vector<int> binFrom, binTo;
        std::vector<float> binTilt, binExact;
        int fftSize = 0;
        double tableRate = 0.0;

        // Motion
        float cursorMs[2] { 0.0f, 0.0f };
        float lastMs[2] { 0.0f, 0.0f };
        float drawnX[2] { -1.0f, -1.0f };
        float drawnThumb = -1.0f;
        float gate = 1.0f, flash = 0.0f, leadFlash = 0.0f, drawnFlash = 0.0f;
        bool haveLast = false;
        int channelsShown = 2;

        // Dragging
        bool dragging = false, gestureOpen = false;
        juce::RangedAudioParameter* gestureParam = nullptr;
        float fineAnchorValue = 0.0f, fineAnchorX = 0.0f;
        bool fineDrag = false;

        int paintCount = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CombField)
    };
}
