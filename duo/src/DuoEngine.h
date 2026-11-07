#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// The whole signal path, free of plug-in framework code.
//
// The lead goes straight to the output through its level control, with no delay.
// The doubles all read one delay line, fed by the mono sum of the input. Each double has its own read head whose delay
// wanders by itself:
//   delay = base delay + slow timing drift + quicker pitch drift
// Reading through a moving delay changes the pitch by exactly the rate the delay moves, so the same contour gives a voice
// its timing and its pitch. Nothing here repeats: both drifts are random curves (cubic B-splines through random points),
// each double with its own seeds and its own speeds, so there is no sweep rate to hear.
//
// Width moves paired doubles apart by level. A double with no partner across the centre fades from its one centre read
// head to two side heads a few ms either side of it, one for each output. Width only changes gains, never a delay, so
// moving it never bends the pitch.
//
// Reads use a 16 tap windowed sinc, so the moving fraction of the read head does not turn into a buzz on high notes.
// The plug-in has zero latency: doubles only ever arrive after the lead.
class DuoEngine
{
public:
    static constexpr int maxDoubles = 3;
    static constexpr float offDb = -60.0f; // a level at the bottom of its range is off

    struct Params
    {
        int doubles = 2;
        float loosePct = 30.0f, widthPct = 70.0f, leadDb = 0.0f;
        std::array<float, maxDoubles> levelDb { -6.0f, -6.0f, -8.0f };
        bool bypass = false;
    };

    void prepare (double sampleRate, const Params& p, int numOutputs)
    {
        sr = sampleRate;
        msToSamples = sr * 0.001;
        buildKernel();

        // Longest reach: largest base + full drift + full split, plus the interpolator's own span.
        const auto maxMs = (double) baseLooseMs[maxDoubles - 1] + maxTimingMs + maxPitchLayerMs + splitMs + splitDriftMs + 2.0;
        maxDelaySamples = maxMs * msToSamples;
        const int needed = (int) std::ceil (maxDelaySamples) + 2 * tapsEachSide + 4;
        size = 1024;
        while (size < needed) size *= 2;
        mask = size - 1;
        line.assign ((size_t) (2 * size), 0.0f); // written twice, so a read never has to wrap
        writeIndex = 0;

        static constexpr std::array<double, maxDoubles> slowSeconds { 2.1, 1.9, 2.3 }, quickSeconds { 0.43, 0.47, 0.41 }, sideSeconds { 3.1, 2.7, 3.4 };
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            const auto base = (uint32_t) (0x51ed27u + 0x9e3779u * (v + 1));
            slow[v].reset (base ^ 0x1a2b3cu, slowSeconds[v], sr);
            quick[v].reset (base ^ 0x4d5e6fu, quickSeconds[v], sr);
            side[v].reset (base ^ 0x7f8091u, sideSeconds[v], sr);
        }

        looseCoef = 1.0 - std::exp (-1.0 / (0.4 * sr));

        leadGain.reset (sr, 0.02);
        for (auto& g : gainL) g.reset (sr, 0.03);
        for (auto& g : gainR) g.reset (sr, 0.03);
        for (auto& g : centreWeight) g.reset (sr, 0.03);
        for (auto& g : sideWeight) g.reset (sr, 0.03);

        computeTargets (p, false, numOutputs);
        leadGain.setCurrentAndTargetValue (leadTarget);
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            gainL[v].setCurrentAndTargetValue (gainLTarget[v]);
            gainR[v].setCurrentAndTargetValue (gainRTarget[v]);
            centreWeight[v].setCurrentAndTargetValue (centreTarget[v]);
            sideWeight[v].setCurrentAndTargetValue (sideTarget[v]);
        }
        loose = looseTarget;
    }

    // Input comes from the first numIn channels and output goes to the first numOut channels, in place.
    void process (float* const* io, int numIn, int numOut, int numSamples, const Params& p, bool forceBypass)
    {
        computeTargets (p, forceBypass, numOut);
        leadGain.setTargetValue (leadTarget);
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            gainL[v].setTargetValue (gainLTarget[v]);
            gainR[v].setTargetValue (gainRTarget[v]);
            centreWeight[v].setTargetValue (centreTarget[v]);
            sideWeight[v].setTargetValue (sideTarget[v]);
        }

        const bool stereoIn = numIn > 1, stereoOut = numOut > 1;
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = io[0][i], inR = stereoIn ? io[1][i] : inL;
            const float feed = stereoIn ? 0.5f * (inL + inR) : inL;
            line[(size_t) writeIndex] = feed;
            line[(size_t) (writeIndex + size)] = feed;

            const float lead = leadGain.getNextValue();
            float outL = lead * inL, outR = lead * inR;

            loose += looseCoef * (looseTarget - loose);
            const double timingMs = minTimingMs + (maxTimingMs - minTimingMs) * loose;
            const double pitchMs = minPitchLayerMs + (maxPitchLayerMs - minPitchLayerMs) * loose;

            for (size_t v = 0; v < maxDoubles; ++v)
            {
                // The curves always advance, even for a silent double, so each voice's path never depends on the others.
                const double s = slow[v].next(), q = quick[v].next(), d = side[v].next();
                const float gl = gainL[v].getNextValue(), gr = gainR[v].getNextValue();
                const float cw = centreWeight[v].getNextValue(), sw = sideWeight[v].getNextValue();
                if (gl == 0.0f && gr == 0.0f)
                    continue;

                const double base = (double) baseTightMs[v] + (double) (baseLooseMs[v] - baseTightMs[v]) * loose;
                const double centreMs = base + timingMs * s + pitchMs * q;
                const float centre = cw != 0.0f ? cw * read (centreMs * msToSamples) : 0.0f;
                outL += gl * centre;
                outR += gr * centre;
                if (sw != 0.0f)
                {
                    const double offsetMs = splitMs + splitDriftMs * d;
                    if (gl != 0.0f) outL += gl * sw * read ((centreMs + offsetMs) * msToSamples);
                    if (gr != 0.0f) outR += gr * sw * read ((centreMs - offsetMs) * msToSamples);
                }
            }

            io[0][i] = outL;
            if (stereoOut) io[1][i] = outR;
            writeIndex = (writeIndex + 1) & mask;
        }
    }

    static float levelToGain (float db) { return db <= offDb ? 0.0f : juce::Decibels::decibelsToGain (db, offDb - 1.0f); }

private:
    // A smooth random curve between -1 and +1: a uniform cubic B-spline through random points. The weights are positive
    // and add up to 1, so it can never leave that range. It has no repeat and no fixed rate, and both its slope (pitch)
    // and the slope's slope are continuous, so nothing steps.
    class DriftCurve
    {
    public:
        void reset (uint32_t seed, double secondsPerPoint, double sampleRate)
        {
            rng = seed != 0u ? seed : 1u;
            step = 1.0 / (secondsPerPoint * sampleRate);
            u = 0.0;
            for (int i = 0; i < 8; ++i) nextRandom(); // leave the seed's own pattern behind
            for (auto& c : points) c = nextRandom();
        }

        double next() noexcept
        {
            u += step;
            if (u >= 1.0)
            {
                u -= 1.0;
                points[0] = points[1]; points[1] = points[2]; points[2] = points[3];
                points[3] = nextRandom();
            }
            const double u2 = u * u, u3 = u2 * u;
            const double w0 = (1.0 - 3.0 * u + 3.0 * u2 - u3) / 6.0;
            const double w1 = (4.0 - 6.0 * u2 + 3.0 * u3) / 6.0;
            const double w2 = (1.0 + 3.0 * u + 3.0 * u2 - 3.0 * u3) / 6.0;
            const double w3 = u3 / 6.0;
            return w0 * points[0] + w1 * points[1] + w2 * points[2] + w3 * points[3];
        }

    private:
        double nextRandom() noexcept
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            return (double) rng / 2147483648.0 - 1.0;
        }

        uint32_t rng = 1u;
        double step = 0.0, u = 0.0;
        std::array<double, 4> points {};
    };

    static constexpr int tapsEachSide = 8, numTaps = 2 * tapsEachSide, kernelRows = 256;

    // Delay of each double when tight and when loose.
    static constexpr std::array<float, maxDoubles> baseTightMs { 14.0f, 21.0f, 29.0f }, baseLooseMs { 28.0f, 38.0f, 50.0f };
    // Size of the slow timing curve and the quicker pitch curve, tight to loose. See README for what these measure as.
    static constexpr double minTimingMs = 1.5, maxTimingMs = 14.0, minPitchLayerMs = 0.35, maxPitchLayerMs = 2.5;
    // A split double's two side heads sit this far either side of its centre head.
    static constexpr double splitMs = 4.0, splitDriftMs = 1.0;

    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    void buildKernel()
    {
        if (! kernel.empty())
            return;
        constexpr double beta = 9.5;
        kernel.resize ((size_t) ((kernelRows + 1) * numTaps));
        const double norm = besselI0 (beta);
        for (int row = 0; row <= kernelRows; ++row)
        {
            const double frac = (double) row / kernelRows;
            double sum = 0.0;
            std::array<double, numTaps> h {};
            for (int t = 0; t < numTaps; ++t)
            {
                const double x = (double) (t - (tapsEachSide - 1)) - frac;
                const double r = x / tapsEachSide;
                const double win = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / norm;
                const double px = juce::MathConstants<double>::pi * x;
                h[(size_t) t] = (std::abs (x) < 1.0e-12 ? 1.0 : std::sin (px) / px) * win;
                sum += h[(size_t) t];
            }
            for (int t = 0; t < numTaps; ++t)
                kernel[(size_t) (row * numTaps + t)] = (float) (h[(size_t) t] / sum); // low notes keep their exact level at every fraction
        }
    }

    float read (double delaySamples) const noexcept
    {
        const double d = juce::jlimit ((double) tapsEachSide + 1.0, maxDelaySamples, delaySamples);
        const int whole = (int) std::ceil (d);
        const double frac = (double) whole - d; // position = (now - whole) + frac
        const double rowPos = frac * kernelRows;
        const int row = juce::jmin ((int) rowPos, kernelRows - 1);
        const float a = (float) (rowPos - row);
        const float* k0 = kernel.data() + row * numTaps;
        const float* k1 = k0 + numTaps;
        const float* x = line.data() + writeIndex + size - whole - (tapsEachSide - 1);
        float sum = 0.0f;
        for (int t = 0; t < numTaps; ++t)
            sum += (k0[t] + a * (k1[t] - k0[t])) * x[t];
        return sum;
    }

    void computeTargets (const Params& p, bool forceBypass, int numOutputs)
    {
        const bool off = p.bypass || forceBypass;
        const int n = juce::jlimit (1, (int) maxDoubles, p.doubles);
        const double width = numOutputs > 1 ? juce::jlimit (0.0, 1.0, (double) p.widthPct * 0.01) : 0.0;

        // Where each double sits at full Width, -1 left to +1 right. A double with no partner across the centre stays
        // there and is split in time instead, so the stereo picture stays balanced.
        static constexpr float pans[maxDoubles][maxDoubles] = { { 0.0f, 0.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f } };
        static constexpr float splits[maxDoubles][maxDoubles] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

        leadTarget = off ? 1.0f : levelToGain (p.leadDb);
        looseTarget = juce::jlimit (0.0, 1.0, (double) p.loosePct * 0.01);
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            const float g = (off || (int) v >= n) ? 0.0f : levelToGain (p.levelDb[v]);
            // Equal power, and exactly 1 on both sides in the centre, so Width 0 leaves left and right identical.
            const double th = (double) pans[n - 1][v] * width * juce::MathConstants<double>::pi * 0.25;
            const double c = std::cos (th), s = std::sin (th);
            const double l = c - s, r = c + s;
            gainLTarget[v] = std::abs (l) < 1.0e-9 ? 0.0f : g * (float) l;
            gainRTarget[v] = std::abs (r) < 1.0e-9 ? 0.0f : g * (float) r;
            // Left and right differ only through the side heads, so correlation falls smoothly as the blend moves to them.
            const double phi = width * (double) splits[n - 1][v] * juce::MathConstants<double>::halfPi;
            const double cw = std::cos (phi), sw = std::sin (phi);
            centreTarget[v] = cw < 1.0e-9 ? 0.0f : (float) cw;
            sideTarget[v] = sw < 1.0e-9 ? 0.0f : (float) sw;
        }
    }

    double sr = 48000.0, msToSamples = 48.0, maxDelaySamples = 0.0;
    int size = 0, mask = 0, writeIndex = 0;
    std::vector<float> line, kernel;

    std::array<DriftCurve, maxDoubles> slow, quick, side;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> leadGain;
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, maxDoubles> gainL, gainR, centreWeight, sideWeight;
    float leadTarget = 1.0f;
    std::array<float, maxDoubles> gainLTarget {}, gainRTarget {}, centreTarget {}, sideTarget {};
    double loose = 0.3, looseTarget = 0.3, looseCoef = 0.0;
};
