#pragma once
#include "Theme.h"
#include "ToneModel.h"
#include <juce_dsp/juce_dsp.h>
#include <array>

// The three things on screen that really move with the sound: the spectrum and response, the saturation curve, and the reel.
// Everything that does not move with the sound sits in cached images, rebuilt only when a control changes.
namespace ui
{
    /** Frequency response of the tape, drawn from its settings, over the real spectrum going in and coming out. */
    class ResponseView final : public juce::Component,
                               public Hinted
    {
    public:
        static constexpr int columns = 96;
        using Column = std::array<float, columns>;

        ResponseView();

        void setSettings (const tapecolor::display::Settings&, bool bypassed);

        /** New spectra in dB. Returns true if anything will visibly move. */
        bool setSpectra (const Column& in, const Column& out);

        void paint (juce::Graphics&) override;
        void resized() override;

        static float columnHz (int column) noexcept;

        int getPaintCount() const noexcept  { return paintCount; }

    private:
        juce::Rectangle<float> plot() const;
        float xFor (float hz) const;
        float yForResponse (float db) const;
        float yForLevel (float db) const;
        void rebuild (float scale);
        void buildSpectrumPaths();

        tapecolor::display::Settings settings;
        bool bypassed = false, layersValid = false;
        juce::Image back, over;
        float layerScale = 0.0f;
        Column inDb, outDb, shownIn, shownOut;
        juce::Path inPath, outPath;
        bool pathsValid = false;
        int paintCount = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResponseView)
    };

    /** The saturation curve, with the part of it the signal is using right now lit up. */
    class CurveView final : public juce::Component,
                            public Hinted
    {
    public:
        CurveView();

        void setSettings (const tapecolor::display::Settings&, bool bypassed);

        /** Returns true if anything will visibly move. */
        bool setPeak (float linearPeak);

        /** The number, called a few times a second, because faster can't be read. Returns true if it changed. */
        bool updateReading();

        void paint (juce::Graphics&) override;
        void resized() override;

        float getSquashDb() const noexcept  { return squashDb; }
        int getPaintCount() const noexcept  { return paintCount; }

    private:
        juce::Rectangle<float> plot() const;
        void rebuild (float scale);
        juce::Rectangle<int> litArea() const;

        tapecolor::display::Settings settings;
        bool bypassed = false, layersValid = false;
        juce::Image back, lit, reading;
        float layerScale = 0.0f, peak = 0.0f, shownPeak = -1.0f, squashDb = 0.0f, shownSquash = -1.0f;
        juce::String squashText;
        int paintCount = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveView)
    };

    /** One flat reel. It turns at the tape speed, shakes with the real read head, and shrinks from an open reel to a cassette hub with Age. */
    class Reel final : public juce::Component,
                       public Hinted
    {
    public:
        Reel();

        void setAge (float ageZeroToOne);

        /** Returns true while it is moving. */
        bool advance (double seconds, double turnsPerSecond, double headMs, bool running);

        void paint (juce::Graphics&) override;
        void resized() override;

        double getAngle() const noexcept  { return angle; }

    private:
        void rebuildPaths();

        float age = 0.0f;
        double angle = 0.0, shownAngle = 0.0, speed = 0.0;
        juce::Path hub;
        float packRadius = 1.0f;
        juce::Colour packColour;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Reel)
    };
}
