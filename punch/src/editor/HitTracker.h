#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "../PunchEngine.h"

// The sums behind the display, with no drawing, so they can be tested. All of this runs on the message thread, reading what the audio thread left in the tap.
namespace punchview
{
    constexpr double binSeconds = 0.0005;
    constexpr int hitBins = 800;          // A hit is shown for 400 ms.
    constexpr int gapBins = 700;          // A new hit no sooner than 350 ms after the last, so one can be read before the next arrives.
    constexpr int smoothBins = 8;         // The drawn level is the highest within 4 ms either side, which takes out the ripple of a voice's own cycles,
    constexpr int roundBins = 5;          // then averaged over 2.5 ms either side, which takes the corners off the steps that leaves.
    constexpr float hitFloor = 0.0056f;   // Below about -45 dBFS is not a hit.
    constexpr float hitRise = 2.0f;       // A hit is the level doubling against 20 to 32 ms earlier.

    // The time axis opens up the start of a hit: 5 ms, 30 ms and 400 ms fall at 16%, 44% and 100% of the width.
    constexpr double axisKnee = 0.005, axisEnd = 0.4;
    inline float xForSeconds (double t)   { return (float) (std::log1p (std::max (t, 0.0) / axisKnee) / std::log1p (axisEnd / axisKnee)); }
    inline double secondsForX (float x)   { return axisKnee * std::expm1 ((double) x * std::log1p (axisEnd / axisKnee)); }

    // The level axis, shared by the plot and the level meters.
    constexpr float topDb = 6.0f, bottomDb = -48.0f;
    inline float yForDb (float db)        { return (topDb - std::clamp (db, bottomDb, topDb)) / (topDb - bottomDb); }
    inline float toDb (float level)       { return 20.0f * std::log10 (std::max (level, 1.0e-6f)); }

    struct HitShape
    {
        std::array<float, hitBins> rawBefore {}, rawAfter {}, peakBefore {}, peakAfter {}, before {}, after {};
        int length = 0, smoothed = 0;

        void clear() { length = smoothed = 0; }

        void append (float b, float a)
        {
            if (length < hitBins)
            {
                rawBefore[(size_t) length] = b;
                rawAfter[(size_t) length] = a;
                ++length;
            }
        }

        // Only what the newest bins can have changed.
        void smooth()
        {
            const int firstPeak = std::max (0, smoothed - smoothBins);

            for (int i = firstPeak; i < length; ++i)
            {
                float highestBefore = 0.0f, highestAfter = 0.0f;
                for (int j = std::max (0, i - smoothBins), last = std::min (length - 1, i + smoothBins); j <= last; ++j)
                {
                    highestBefore = std::max (highestBefore, rawBefore[(size_t) j]);
                    highestAfter = std::max (highestAfter, rawAfter[(size_t) j]);
                }
                peakBefore[(size_t) i] = highestBefore;
                peakAfter[(size_t) i] = highestAfter;
            }

            for (int i = std::max (0, firstPeak - roundBins); i < length; ++i)
            {
                float sumBefore = 0.0f, sumAfter = 0.0f;
                const int first = std::max (0, i - roundBins), last = std::min (length - 1, i + roundBins);
                for (int j = first; j <= last; ++j)
                {
                    sumBefore += peakBefore[(size_t) j];
                    sumAfter += peakAfter[(size_t) j];
                }
                const float count = (float) (last - first + 1);
                before[(size_t) i] = sumBefore / count;
                after[(size_t) i] = sumAfter / count;
            }

            smoothed = length;
        }
    };

    class HitTracker
    {
    public:
        /** One bin. True when a new hit starts on it. */
        bool feed (float before, float after)
        {
            history[historyAt] = before;
            historyAt = (historyAt + 1) % history.size();
            if (sinceHit < gapBins)
                ++sinceHit;

            float recent = 0.0f, earlier = 0.0f;
            for (size_t k = 1; k <= 10; ++k)
                recent = std::max (recent, history[(historyAt + history.size() - k) % history.size()]);
            for (size_t k = 40; k <= history.size(); ++k)
                earlier = std::max (earlier, history[(historyAt + history.size() - k) % history.size()]);

            const bool isHit = sinceHit >= gapBins && recent > hitFloor && recent >= earlier * hitRise;

            if (isHit)
            {
                std::swap (current, previous);
                shapes[(size_t) current].clear();
                sinceHit = 0;
                everHit = true;
            }

            if (everHit)
                shapes[(size_t) current].append (before, after);

            return isHit;
        }

        const HitShape& latest() const noexcept   { return shapes[(size_t) current]; }
        HitShape& latest() noexcept               { return shapes[(size_t) current]; }
        const HitShape& lastOne() const noexcept  { return shapes[(size_t) previous]; }
        HitShape& lastOne() noexcept              { return shapes[(size_t) previous]; }
        bool hasHit() const noexcept              { return everHit; }
        bool isCapturing() const noexcept         { return everHit && latest().length < hitBins; }

    private:
        std::array<HitShape, 2> shapes;
        std::array<float, 64> history {};
        size_t historyAt = 0;
        int current = 0, previous = 1, sinceHit = gapBins;
        bool everHit = false;
    };

    // What the controls are set to, for working out what a hit would look like.
    struct Settings
    {
        float punch = 0.0f, attack = 0.0f, sustain = 0.0f, mix = 100.0f, gain = 0.0f, impact = 0.0f, ceiling = -1.0f;
        bool ceilingOn = false, bypass = false, steadyNotes = false;

        bool operator== (const Settings& o) const noexcept
        {
            return punch == o.punch && attack == o.attack && sustain == o.sustain && mix == o.mix && gain == o.gain
                && impact == o.impact && ceiling == o.ceiling && ceilingOn == o.ceilingOn && bypass == o.bypass && steadyNotes == o.steadyNotes;
        }
        bool operator!= (const Settings& o) const noexcept { return ! (*this == o); }
    };

    /** Runs a hit's level through the plug-in's real detector, to show what the settings would do while no audio is playing.
        It runs at the display's rate, so it is a close drawing of the result, not the result. Nothing here reaches the audio. */
    class Preview
    {
    public:
        Preview() { engine.prepare (1.0 / binSeconds); engine.setLevelsOnly (true); }

        void run (const float* before, int length, const Settings& s, float* after)
        {
            const float inGain = s.bypass ? 1.0f : PunchEngine::dbToGain (s.gain);
            const float ceiling = PunchEngine::dbToGain (s.ceiling);
            const float punch = s.punch * 0.01f, impact = s.impact * 0.01f, wet = s.bypass ? 0.0f : s.mix * 0.01f;
            engine.reset (punch);

            // The first pass lets its slow parts settle, as they would have by the time a real hit comes along. A hit comes out of something
            // quieter, so each pass follows half a second well below its start, or the end of the last pass would read as the tail of a sound.
            const float leadIn = length > 0 ? before[0] * 0.25f : 0.0f;

            for (int pass = 0; pass < 2; ++pass)
            {
                for (int i = 0; i < leadInBins; ++i)
                    engine.process (leadIn * inGain, s.attack, s.sustain, punch, impact, s.steadyNotes, leadIn * inGain);

                for (int i = 0; i < length; ++i)
                {
                    // The display only has levels, not the sound itself, so it cannot hear a pitch: with Steady Notes every sound is drawn
                    // as a note, and the sparing of bright sounds is not drawn.
                    const float gainDb = engine.process (before[i] * inGain, s.attack, s.sustain, punch, impact, s.steadyNotes, before[i] * inGain);
                    if (pass == 1)
                    {
                        const float level = before[i] * inGain * (1.0f + wet * (PunchEngine::dbToGain (gainDb) - 1.0f));
                        after[i] = s.ceilingOn && ! s.bypass ? std::min (level, ceiling) : level;
                    }
                }
            }
        }

    private:
        static constexpr int leadInBins = 1000;
        PunchEngine engine;
    };

    /** A made-up hit, for before there is a real one: a sharp start over a decaying body. */
    inline void fillExampleHit (float* before, int length)
    {
        for (int i = 0; i < length; ++i)
        {
            const double t = i * binSeconds;
            const double rise = std::min (1.0, t / 0.0015);
            before[i] = (float) (rise * (0.2 * std::exp (-t / 0.11) + 0.38 * std::exp (-t / 0.012)));
        }
    }

    /** One bar on the timeline: the highest level in about 33 ms, one per frame at 30 frames a second. */
    constexpr int barBins = 66;
}
