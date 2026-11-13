#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// The whole signal path, free of plug-in framework code.
//
// The lead goes straight to the output through its level and pan, with no delay.
//
// The doubles start from the mono sum of the input:
//   mono sum > short time spectrum step (Timbre Variation moves each double's formants) > one delay line per double > read heads
// Each double's read head wanders by itself:
//   delay = base delay + Extra Delay + slow timing drift + quicker pitch drift + vibrato bend
// Reading through a moving delay changes the pitch by exactly the rate the delay moves, so the same contour gives a voice
// its timing and its pitch. Nothing here repeats: both drifts are random curves (cubic B-splines through random points),
// each double with its own seeds and its own speeds, so there is no sweep rate to hear.
//
// Vibrato Variation follows the pitch of the input, finds its vibrato, and bends each double by a share of it, so one double
// sings straighter and another sings wider than the lead. Precise Vibrato (0.4.0) fits a line plus one sinusoid to the last
// cycle and a half of the pitch and reads that sinusoid at the moment each double is playing, which is its own delay back
// from now. So the bend lands on the vibrato it is meant to change, at any vibrato speed and while the double wanders. With
// Precise Vibrato off the 0.3 law runs instead, unchanged: one band pass around 6.3 Hz, the same bend for every double.
//
// Width moves paired doubles apart by level. A double with no partner across the centre fades from its one centre read
// head to two side heads a few ms either side of it, one for each output. Width only changes gains, never a delay, so
// moving it never bends the pitch.
//
// Reads use a 32 tap windowed sinc, so the moving fraction of the read head does not turn into a buzz on high notes.
// The plug-in has zero latency: the spectrum step's delay comes out of each double's own delay, and doubles only ever
// arrive after the lead.
class DuoEngine
{
public:
    static constexpr int maxDoubles = 3;
    static constexpr float offDb = -60.0f; // a level at the bottom of its range is off
    enum Range { soprano, altoTenor, baritoneBass, instrument };

    struct Params
    {
        int doubles = 2;
        float loosePct = 30.0f, widthPct = 70.0f, leadDb = 0.0f;
        std::array<float, maxDoubles> levelDb { -6.0f, -6.0f, -8.0f };
        float timingPct = 100.0f, pitchPct = 100.0f, vibratoPct = 0.0f, timbrePct = 0.0f, leadPanPct = 0.0f, doublesPanPct = 0.0f, extraMs = 0.0f;
        int range = altoTenor;
        bool bypass = false;
        // Since 0.4.0. Precise Vibrato off is the 0.3 vibrato law, bit for bit: sessions saved before 0.4.0 load with it off.
        bool vibratoPrecise = true;
        // Mono Safe puts every double on left and right with opposite signs, so (L + R) / 2 is the lead alone.
        bool monoSafe = false;
        // Match Level turns the lead and the doubles down together, so what comes out is as loud as what went in.
        bool matchLevel = false;
    };

    // How far each double's formants move at Timbre Variation 100, in semitones.
    static constexpr std::array<double, maxDoubles> timbreSemitones { 3.0, -3.0, 1.5 };
    // Share of the input's vibrato each double adds at Vibrato Variation 100. -1 removes it, +0.5 makes it half as wide again.
    static constexpr std::array<double, maxDoubles> vibratoShare { -1.0, 0.5, -0.5 };

    // Length of the Timbre spectrum frame at a sample rate, in samples.
    static int spectrumFrameSamples (double sampleRate)
    {
        // 512 points, 1024 at 88.2 and 96 kHz, 2048 at 176.4 and 192 kHz: about 11 ms at every rate. The lifter and the smoothers
        // per frame are made for that length.
        const int order = sampleRate < 60000.0 ? 9 : sampleRate < 120000.0 ? 10 : 11;
        return 1 << order;
    }

    void prepare (double sampleRate, const Params& p, int numOutputs)
    {
        sr = sampleRate;
        msToSamples = sr * 0.001;
        buildKernel();
        prepareSpectrum();
        prepareTracker();

        const double maxTotalMs = (double) baseLooseMs[maxDoubles - 1] + maxExtraMs + 3.0 * maxTimingMs + 3.0 * maxPitchLayerMs + splitMs + splitDriftMs + maxBendMs + 2.0;
        maxDelaySamples = maxTotalMs * msToSamples;
        const int needed = (int) std::ceil (maxDelaySamples) + 2 * tapsEachSide + 4;
        size = 1024;
        while (size < needed) size *= 2;
        mask = size - 1;
        for (auto& l : lines)
            l.assign ((size_t) (2 * size), 0.0f); // written twice, so a read never has to wrap
        reset (p, numOutputs);
    }

    // Back to how prepare leaves it, at the same rate and size, without allocating: empty lines and spectrum buffers, a new pitch
    // history, the drift curves from their start, every glide at its target. The host's Reset comes here.
    void reset (const Params& p, int numOutputs)
    {
        if (lines[0].empty())
            return; // not prepared yet
        for (auto& l : lines)
            std::fill (l.begin(), l.end(), 0.0f);
        writeIndex = 0;
        clearSpectrum();
        clearTracker();

        static constexpr std::array<double, maxDoubles> slowSeconds { 2.1, 1.9, 2.3 }, quickSeconds { 0.43, 0.47, 0.41 }, sideSeconds { 3.1, 2.7, 3.4 };
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            const auto base = (uint32_t) (0x51ed27u + 0x9e3779u * (v + 1));
            slow[v].reset (base ^ 0x1a2b3cu, slowSeconds[v], sr);
            quick[v].reset (base ^ 0x4d5e6fu, quickSeconds[v], sr);
            side[v].reset (base ^ 0x7f8091u, sideSeconds[v], sr);
        }

        slowCoef = 1.0 - std::exp (-1.0 / (0.6 * sr)); // slow, because moving a delay bends the pitch
        extraCoef = 1.0 - std::exp (-1.0 / (1.0 * sr));

        leadL.reset (sr, 0.02);
        leadR.reset (sr, 0.02);
        for (auto* set : { &gainL, &gainR, &centreWeight, &sideWeight })
            for (auto& g : *set)
                g.reset (sr, 0.03);

        computeTargets (p, false, numOutputs);
        leadL.setCurrentAndTargetValue (leadLTarget);
        leadR.setCurrentAndTargetValue (leadRTarget);
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            gainL[v].setCurrentAndTargetValue (gainLTarget[v]);
            gainR[v].setCurrentAndTargetValue (gainRTarget[v]);
            centreWeight[v].setCurrentAndTargetValue (centreTarget[v]);
            sideWeight[v].setCurrentAndTargetValue (sideTarget[v]);
        }
        loose = looseTarget;
        timingAmount = timingTarget;
        pitchAmount = pitchTarget;
        extra = extraTarget;
        timbre = timbreTarget;
        vibrato = vibratoTarget;
        for (auto& r : readDelayMs)
            r.store (0.0f, std::memory_order_relaxed);
        readPitchHz.store (0.0f, std::memory_order_relaxed);
    }

    // Input comes from the first numIn channels and output goes to the first numOut channels, in place.
    void process (float* const* io, int numIn, int numOut, int numSamples, const Params& p, bool forceBypass)
    {
        computeTargets (p, forceBypass, numOut);
        leadL.setTargetValue (leadLTarget);
        leadR.setTargetValue (leadRTarget);
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            gainL[v].setTargetValue (gainLTarget[v]);
            gainR[v].setTargetValue (gainRTarget[v]);
            centreWeight[v].setTargetValue (centreTarget[v]);
            sideWeight[v].setTargetValue (sideTarget[v]);
        }

        const bool stereoIn = numIn > 1, stereoOut = numOut > 1;
        const double frameLengthMs = frameSamples * 1000.0 / sr;
        std::array<double, maxDoubles> lastCentre {};
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = io[0][i], inR = stereoIn ? io[1][i] : inL;
            const float feed = stereoIn ? 0.5f * (inL + inR) : inL;

            pushTracker (feed);
            pushSpectrum (feed);
            for (size_t v = 0; v < maxDoubles; ++v)
            {
                const float x = spectrumOut[v];
                lines[v][(size_t) writeIndex] = x;
                lines[v][(size_t) (writeIndex + size)] = x;
            }

            float outL = leadL.getNextValue() * inL, outR = leadR.getNextValue() * inR;

            loose += slowCoef * (looseTarget - loose);
            timingAmount += slowCoef * (timingTarget - timingAmount);
            pitchAmount += slowCoef * (pitchTarget - pitchAmount);
            extra += extraCoef * (extraTarget - extra);
            const double timingFull = minTimingMs + (maxTimingMs - minTimingMs) * loose;
            const double pitchFull = minPitchLayerMs + (maxPitchLayerMs - minPitchLayerMs) * loose;
            const double timingMs = timingFull * timingAmount, pitchMs = pitchFull * pitchAmount;
            // Past 100 percent, a wider swing needs more room: the doubles sit later instead of hitting their minimum delay.
            const double room = std::max (0.0, timingAmount - 1.0) * timingFull + std::max (0.0, pitchAmount - 1.0) * pitchFull;

            for (size_t v = 0; v < maxDoubles; ++v)
            {
                // The curves always advance, even for a silent double, so each voice's path never depends on the others.
                const double s = slow[v].next(), q = quick[v].next(), d = side[v].next();
                bendNow[v] += bendStep * (bendTarget[v] - bendNow[v]);
                bendSmooth[v] += bendSmoothCoef * (bendNow[v] - bendSmooth[v]);

                const float gl = gainL[v].getNextValue(), gr = gainR[v].getNextValue();
                const float cw = centreWeight[v].getNextValue(), sw = sideWeight[v].getNextValue();
                const double base = (double) baseTightMs[v] + (double) (baseLooseMs[v] - baseTightMs[v]) * loose;
                const double centreMs = base + room + extra + timingMs * s + pitchMs * q + bendSmooth[v];
                lastCentre[v] = centreMs;
                delayNowMs[v] = centreMs;
                if (gl == 0.0f && gr == 0.0f)
                    continue;

                const auto& line = lines[v];
                const float centre = cw != 0.0f ? cw * read (line, (centreMs - frameLengthMs) * msToSamples) : 0.0f;
                outL += gl * centre;
                outR += gr * centre;
                if (sw != 0.0f)
                {
                    const double offsetMs = splitMs + splitDriftMs * d;
                    if (gl != 0.0f) outL += gl * sw * read (line, (centreMs + offsetMs - frameLengthMs) * msToSamples);
                    if (gr != 0.0f) outR += gr * sw * read (line, (centreMs - offsetMs - frameLengthMs) * msToSamples);
                }
            }

            io[0][i] = outL;
            if (stereoOut) io[1][i] = outR;
            writeIndex = (writeIndex + 1) & mask;
        }

        for (size_t v = 0; v < maxDoubles; ++v)
            readDelayMs[v].store ((float) lastCentre[v], std::memory_order_relaxed);
        readPitchHz.store ((float) trackedHz, std::memory_order_relaxed);
    }

    static float levelToGain (float db)
    {
        if (db <= offDb) return 0.0f;
        if (std::abs (db) < 0.001f) return 1.0f; // 0 dB is exact, however the host's parameter value rounds
        return juce::Decibels::decibelsToGain (db, offDb - 1.0f);
    }

    // For the window to draw, safe from any thread: each double's delay behind the lead, and the pitch followed (0 when none).
    float getDelayMs (int v) const { return readDelayMs[(size_t) v].load (std::memory_order_relaxed); }
    float getPitchHz() const { return readPitchHz.load (std::memory_order_relaxed); }

    // Where each double sits at full Width, -1 left to +1 right. A double with no partner across the centre stays
    // there and is split in time instead, so the stereo picture stays balanced.
    static constexpr float panAtFullWidth[maxDoubles][maxDoubles] = { { 0.0f, 0.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f } };
    static constexpr float splitAtFullWidth[maxDoubles][maxDoubles] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

    // The window's picture of the settings, from the same numbers the sound uses. None of these are called from the audio path.
    // How far behind the lead a double's path is centred, in ms.
    static double restDelayMs (int v, double loosePct, double timingPct, double pitchPct, double extraMs)
    {
        const double l = juce::jlimit (0.0, 1.0, loosePct * 0.01), t = juce::jlimit (0.0, 2.0, timingPct * 0.01), q = juce::jlimit (0.0, 2.0, pitchPct * 0.01);
        const double room = std::max (0.0, t - 1.0) * timingFullMs (l) + std::max (0.0, q - 1.0) * pitchFullMs (l);
        return (double) baseTightMs[(size_t) v] + (double) (baseLooseMs[(size_t) v] - baseTightMs[(size_t) v]) * l + room + juce::jlimit (0.0, maxExtraMs, extraMs);
    }

    // How far a double's path wanders either side of that, in ms.
    static double driftRangeMs (double loosePct, double timingPct, double pitchPct, double vibratoPct)
    {
        const double l = juce::jlimit (0.0, 1.0, loosePct * 0.01);
        return timingFullMs (l) * juce::jlimit (0.0, 2.0, timingPct * 0.01) + pitchFullMs (l) * juce::jlimit (0.0, 2.0, pitchPct * 0.01)
             + maxBendMs * juce::jlimit (0.0, 1.0, vibratoPct * 0.01);
    }

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

    static constexpr int tapsEachSide = 16, numTaps = 2 * tapsEachSide, kernelRows = 256;

    // Delay of each double when tight and when loose. Never less than the spectrum step's own delay plus the room to move.
    static constexpr std::array<float, maxDoubles> baseTightMs { 21.0f, 26.0f, 33.0f }, baseLooseMs { 36.0f, 44.0f, 56.0f };
    // Size of the slow timing curve and the quicker pitch curve, tight to loose. See README for what these measure as.
    static constexpr double minTimingMs = 1.5, maxTimingMs = 14.0, minPitchLayerMs = 0.35, maxPitchLayerMs = 2.5;
    static double timingFullMs (double l) { return minTimingMs + (maxTimingMs - minTimingMs) * l; }
    static double pitchFullMs (double l)  { return minPitchLayerMs + (maxPitchLayerMs - minPitchLayerMs) * l; }
    // A split double's two side heads sit this far either side of its centre head.
    static constexpr double splitMs = 3.0, splitDriftMs = 0.8;
    static constexpr double maxExtraMs = 100.0, maxBendMs = 3.0;

    // ------------------------------------------------------------ read head

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

    float read (const std::vector<float>& line, double delaySamples) const noexcept
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

    // ------------------------------------------------------------ Timbre Variation

    // Overlapped short spectra. Each double multiplies the magnitudes by "its envelope moved up or down over the input's own
    // envelope", so formants move and harmonics, and so the pitch, stay put. The envelope comes from a window twice as long as
    // the one the sound passes through: it resolves formants better and costs no extra delay. With Timbre Variation at 0 no
    // spectrum is taken at all and a frame's overlap adds back to the input exactly.
    void prepareSpectrum()
    {
        const int order = juce::roundToInt (std::log2 ((double) spectrumFrameSamples (sr)));
        frameSamples = 1 << order;
        hopSamples = frameSamples / 4;
        analysisSamples = frameSamples * 2;
        fft = std::make_unique<juce::dsp::FFT> (order);
        analysisFft = std::make_unique<juce::dsp::FFT> (order + 1);
        const auto n = (size_t) frameSamples, na = (size_t) analysisSamples;
        hann.resize (n);
        for (size_t i = 0; i < n; ++i)
            hann[i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) n));
        analysisHann.resize (na);
        for (size_t i = 0; i < na; ++i)
            analysisHann[i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) na));
        fifo.assign (na, 0.0f);
        frame.assign (n, 0.0f);
        spectrum.assign (n * 2, 0.0f);
        scratch.assign (n * 2, 0.0f);
        analysis.assign (na * 2, 0.0f);
        envelope.assign (na / 2 + 1, 0.0f);
        logAverage.assign (na / 2 + 1, 0.0f);
        logAverageCoef = (float) (1.0 - std::exp (-(double) hopSamples / (0.016 * sr)));
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            overlap[v].assign (n, 0.0f);
            logGain[v].assign (n / 2 + 1, 0.0f);
        }
    }

    void clearSpectrum() noexcept
    {
        for (auto* b : { &fifo, &frame, &spectrum, &scratch, &analysis, &envelope, &logAverage })
            std::fill (b->begin(), b->end(), 0.0f);
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            std::fill (overlap[v].begin(), overlap[v].end(), 0.0f);
            std::fill (logGain[v].begin(), logGain[v].end(), 0.0f);
            levelFix[v] = 0.0f;
            shifted[v] = false;
            spectrumOut[v] = 0.0f;
        }
        fifoPos = 0;
        olaPos = 0;
        hopCount = 0;
    }

    void pushSpectrum (float x) noexcept
    {
        fifo[(size_t) fifoPos] = x;
        fifoPos = (fifoPos + 1) & (analysisSamples - 1);
        olaPos = (olaPos + 1) & (frameSamples - 1);
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            auto& ola = overlap[v][(size_t) olaPos];
            spectrumOut[v] = ola;
            ola = 0.0f;
        }
        if (++hopCount == hopSamples)
        {
            hopCount = 0;
            processFrame();
        }
    }

    void processFrame() noexcept
    {
        const int n = frameSamples, na = analysisSamples;
        timbre += 0.35 * (timbreTarget - timbre);
        if (timbreTarget == 0.0 && timbre < 1.0e-4) timbre = 0.0;

        // fifoPos is the oldest sample of the long window; the short window is its newest part.
        for (int i = 0; i < n; ++i)
            frame[(size_t) i] = fifo[(size_t) ((fifoPos + na - n + i) & (na - 1))] * hann[(size_t) i];

        bool anySpectrum = timbre != 0.0;
        for (size_t v = 0; v < maxDoubles; ++v)
            anySpectrum = anySpectrum || shifted[v];

        if (anySpectrum)
        {
            std::copy_n (frame.data(), n, spectrum.data());
            std::fill (spectrum.begin() + n, spectrum.end(), 0.0f);
            fft->performRealOnlyForwardTransform (spectrum.data());
            findEnvelope();
        }

        const float scale = 1.0f / 1.5f; // Hann in and Hann out at a quarter frame hop add up to 1.5
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            if (anySpectrum && (timbre != 0.0 || shifted[v]))
                shapeVoice (v);

            // The first sample of this frame comes out at the next sample: exactly one frame behind the input.
            const float* source = shifted[v] ? scratch.data() : frame.data();
            auto& ola = overlap[v];
            for (int i = 0; i < n; ++i)
                ola[(size_t) ((olaPos + 1 + i) & (n - 1))] += source[i] * hann[(size_t) i] * scale;
        }
    }

    // A smooth outline of the spectrum: keep only the low quefrencies of the log magnitude. The cut sits below the note's own
    // period when one is being followed, so harmonics stay out of the outline and formants stay in.
    void findEnvelope() noexcept
    {
        const int na = analysisSamples, half = na / 2;
        for (int i = 0; i < na; ++i)
            analysis[(size_t) i] = fifo[(size_t) ((fifoPos + i) & (na - 1))] * analysisHann[(size_t) i];
        std::fill (analysis.begin() + na, analysis.end(), 0.0f);
        analysisFft->performRealOnlyForwardTransform (analysis.data());
        for (int k = 0; k <= half; ++k)
        {
            const float re = analysis[(size_t) (2 * k)], im = analysis[(size_t) (2 * k + 1)];
            // A few frames of averaging steadies the outline on breath and consonants, so it can follow narrow formants.
            logAverage[(size_t) k] += logAverageCoef * (0.5f * std::log (re * re + im * im + 1.0e-10f) - logAverage[(size_t) k]);
            analysis[(size_t) (2 * k)] = logAverage[(size_t) k];
            analysis[(size_t) (2 * k + 1)] = 0.0f;
        }
        std::fill (analysis.begin() + na + 2, analysis.end(), 0.0f);
        analysisFft->performRealOnlyInverseTransform (analysis.data());
        const double cutSeconds = trackedHz > 0.0 ? juce::jlimit (0.0015, 0.005, 0.6 / trackedHz) : 0.005;
        const int lifter = juce::jlimit (8, half - 2, (int) std::round (cutSeconds * sr));
        for (int i = 1; i < na; ++i)
        {
            const int q = juce::jmin (i, na - i);
            // Flat up to half the cut, then a smooth fall: a gentler fall would blur narrow formants and leave them behind.
            const int flat = lifter / 2;
            const float keep = q <= flat ? 1.0f : q < lifter ? 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (q - flat) / (float) (lifter - flat)) : 0.0f;
            analysis[(size_t) i] *= keep;
        }
        std::fill (analysis.begin() + na, analysis.end(), 0.0f);
        analysisFft->performRealOnlyForwardTransform (analysis.data());
        for (int k = 0; k <= half; ++k)
            envelope[(size_t) k] = analysis[(size_t) (2 * k)];
    }

    // Leaves this double's time signal for this frame in scratch, unless the double has settled back to untouched.
    void shapeVoice (size_t v) noexcept
    {
        const int n = frameSamples, half = n / 2;
        const double ratio = std::pow (2.0, timbre * timbreSemitones[v] / 12.0);
        auto& lg = logGain[v];
        double inEnergy = 1.0e-20, outEnergy = 1.0e-20, biggest = 0.0;
        for (int k = 0; k <= half; ++k)
        {
            const double pos = juce::jmin ((double) (2 * half), (double) (2 * k) / ratio); // envelope bins are half as wide
            const int a = juce::jmin ((int) pos, 2 * half - 1);
            const float u = (float) (pos - a);
            const float moved = envelope[(size_t) a] + u * (envelope[(size_t) a + 1] - envelope[(size_t) a]);
            const float wanted = timbre == 0.0 ? 0.0f : juce::jlimit (-1.386f, 1.386f, moved - envelope[(size_t) (2 * k)]); // at most 12 dB
            lg[(size_t) k] += 0.5f * (wanted - lg[(size_t) k]);
            biggest = std::max (biggest, (double) std::abs (lg[(size_t) k]));
            const double power = (double) spectrum[(size_t) (2 * k)] * spectrum[(size_t) (2 * k)] + (double) spectrum[(size_t) (2 * k + 1)] * spectrum[(size_t) (2 * k + 1)];
            inEnergy += power;
            outEnergy += power * std::exp (2.0 * (double) lg[(size_t) k]);
        }
        // Moving formants must not change the double's level.
        const float wantedFix = inEnergy > 1.0e-12 ? (float) juce::jlimit (-0.7, 0.7, 0.5 * std::log (inEnergy / outEnergy)) : 0.0f;
        levelFix[v] += 0.25f * (wantedFix - levelFix[v]);

        if (timbre == 0.0 && biggest < 1.0e-5 && std::abs (levelFix[v]) < 1.0e-5f)
        {
            std::fill (lg.begin(), lg.end(), 0.0f);
            levelFix[v] = 0.0f;
            shifted[v] = false;
            return;
        }
        shifted[v] = true;
        for (int k = 0; k <= half; ++k)
        {
            const float g = std::exp (lg[(size_t) k] + levelFix[v]);
            scratch[(size_t) (2 * k)] = spectrum[(size_t) (2 * k)] * g;
            scratch[(size_t) (2 * k + 1)] = spectrum[(size_t) (2 * k + 1)] * g;
        }
        std::fill (scratch.begin() + n + 2, scratch.end(), 0.0f);
        fft->performRealOnlyInverseTransform (scratch.data());
    }

    // ------------------------------------------------------------ Vibrato Variation

    void prepareTracker()
    {
        trackerFactor = juce::jmax (1, (int) std::round (sr / 6000.0));
        trackerRate = sr / trackerFactor;
        trackerLp = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 1600.0 / sr);
        trackerWindow = (int) std::round (0.03 * trackerRate);
        trackerHop = juce::jmax (1, (int) std::round (0.005 * trackerRate));
        const double frameRate = trackerRate / trackerHop;
        trackerSeconds = 1.0 / frameRate;
        trackerRing.assign ((size_t) trackerRingSize, 0.0f);
        trackerDiff.assign ((size_t) trackerRingSize, 0.0f);
        trackerLinear.assign ((size_t) trackerRingSize, 0.0f);
        // Band pass over the speeds vibrato has, about 4 to 9.5 Hz. Narrow, so a slide between notes barely gets through. Centred
        // a little high on purpose: its phase lead makes up for the pitch tracker's own lag across the usual vibrato speeds.
        const double w = juce::MathConstants<double>::twoPi * 6.3 / frameRate, alpha = std::sin (w) / (2.0 * 1.2), a0 = 1.0 + alpha;
        bp0 = alpha / a0; bp1 = -2.0 * std::cos (w) / a0; bp2 = (1.0 - alpha) / a0;
        bendStep = 1.0 - std::exp (-1.0 / (trackerSeconds * sr));
        bendSmoothCoef = 1.0 - std::exp (-1.0 / (0.003 * sr));

        // Precise Vibrato: for each vibrato speed tried, how many frames make its window and the cosine and sine at each of them
        // (time runs back from the newest frame).
        fitCos.assign ((size_t) (fitRates * fitFrames), 0.0);
        fitSin.assign ((size_t) (fitRates * fitFrames), 0.0);
        for (int j = 0; j < fitRates; ++j)
        {
            const double hz = fitRateHz (j), w = juce::MathConstants<double>::twoPi * hz;
            fitLength[(size_t) j] = juce::jlimit (8, fitFrames - 1, (int) std::round (fitCycles / (hz * trackerSeconds)));
            for (int k = 0; k < fitFrames; ++k)
            {
                fitCos[(size_t) (j * fitFrames + k)] = std::cos (-w * trackerSeconds * k);
                fitSin[(size_t) (j * fitFrames + k)] = std::sin (-w * trackerSeconds * k);
            }
        }
    }

    void clearTracker() noexcept
    {
        for (auto* b : { &trackerRing, &trackerDiff, &trackerLinear })
            std::fill (b->begin(), b->end(), 0.0f);
        trackerPos = trackerCount = trackerSkip = 0;
        lp1 = lp2 = 0.0;
        trackedHz = 0.0;
        recent.fill (0.0);
        recentPos = 0;
        hold = 0;
        gate = 0.0;
        haveCents = false;
        lastCents = 0.0;
        bpX1 = bpX2 = bpY1 = bpY2 = 0.0;
        bend.fill (0.0);
        bendTarget.fill (0.0);
        bendNow.fill (0.0);
        bendSmooth.fill (0.0);
        // Precise Vibrato: the pitch history and the sinusoid in use.
        fitCents.fill (0.0);
        fitOk.fill (0);
        fitRecent.fill (0.0);
        fitHead = fitSince = fitRecentPos = 0;
        fitHave = fitGood = false;
        fitLast = fitGate = fitA = fitB = fitOmega = 0.0;
        fitAge = trackerAge = fitOriginAge = 0.025;
        fitJumpAge = -1.0;
        delayNowMs.fill (0.0);
    }

    void pushTracker (float x) noexcept
    {
        lp1 += trackerLp * ((double) x - lp1);
        lp2 += trackerLp * (lp1 - lp2);
        if (++trackerSkip < trackerFactor)
            return;
        trackerSkip = 0;
        trackerRing[(size_t) trackerPos] = (float) lp2;
        trackerPos = (trackerPos + 1) & (trackerRingSize - 1);
        if (++trackerCount >= trackerHop)
        {
            trackerCount = 0;
            trackFrame();
        }
    }

    // The difference function method: find the lag at which the signal best matches itself.
    double findPitch() noexcept
    {
        static constexpr std::array<double, 4> lows { 170.0, 75.0, 60.0, 50.0 }, highs { 1100.0, 750.0, 400.0, 1500.0 };
        const int r = juce::jlimit (0, 3, rangeTarget);
        const int maxLag = juce::jlimit (4, trackerRingSize - trackerWindow - 2, (int) std::ceil (trackerRate / lows[(size_t) r]));
        const int minLag = juce::jmax (2, (int) std::floor (trackerRate / highs[(size_t) r]));
        const int span = trackerWindow + maxLag + 1;
        for (int i = 0; i < span; ++i)
            trackerLinear[(size_t) i] = trackerRing[(size_t) ((trackerPos - span + i) & (trackerRingSize - 1))];
        const float* s = trackerLinear.data();

        double energy = 0.0;
        for (int i = 0; i < trackerWindow; ++i) energy += (double) s[i] * s[i];
        if (energy / trackerWindow < 1.0e-6) // below about -60 dB: nothing to follow
            return 0.0;

        double running = 0.0;
        trackerDiff[0] = 1.0f;
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            double d = 0.0;
            for (int i = 0; i < trackerWindow; ++i)
            {
                const double e = (double) s[i] - s[i + lag];
                d += e * e;
            }
            running += d;
            trackerDiff[(size_t) lag] = running > 0.0 ? (float) (d * lag / running) : 1.0f;
        }
        for (int lag = minLag; lag < maxLag; ++lag)
            if (trackerDiff[(size_t) lag] < 0.15f)
            {
                while (lag + 1 < maxLag && trackerDiff[(size_t) lag + 1] < trackerDiff[(size_t) lag]) ++lag;
                const double a = trackerDiff[(size_t) lag - 1], b = trackerDiff[(size_t) lag], c = trackerDiff[(size_t) lag + 1];
                const double denom = a - 2.0 * b + c;
                const double refined = lag + (std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0);
                // The stretch this was read from starts span samples back and is one window plus one period long: its middle is this old.
                trackerAge = ((double) span - 0.5 * ((double) trackerWindow + refined)) / trackerRate;
                return trackerRate / refined;
            }
        return 0.0;
    }

    void trackFrame() noexcept
    {
        trackedHz = findPitch();
        const bool voiced = trackedHz > 0.0;
        pushFit (voiced); // the history Precise Vibrato reads; nothing below reads it
        if (voiced)
        {
            const double cents = 1200.0 * std::log2 (trackedHz / 55.0);
            const double before = recent[(size_t) recentPos]; // 8 frames ago, about 40 ms
            recent[(size_t) recentPos] = cents;
            recentPos = (recentPos + 1) % (int) recent.size();
            // A new note, not vibrato: start the filter over on it and wait a moment. Vibrato cannot move this fast.
            if (! haveCents || std::abs (cents - before) > 150.0)
            {
                if (haveCents) hold = 30;
                else recent.fill (cents);
                bpX1 = bpX2 = cents;
                bpY1 = bpY2 = 0.0;
                haveCents = true;
            }
            lastCents = cents;
        }

        const double y = bp0 * (lastCents - bpX2) - bp1 * bpY1 - bp2 * bpY2;
        bpX2 = bpX1; bpX1 = lastCents; bpY2 = bpY1; bpY1 = y;

        if (hold > 0) --hold;
        const double wanted = (voiced && hold == 0) ? 1.0 : 0.0;
        gate += juce::jlimit (-0.05, 0.05, wanted - gate);
        if (! voiced && gate == 0.0) haveCents = false;

        vibrato += 0.2 * (vibratoTarget - vibrato);
        if (vibratoTarget == 0.0 && vibrato < 1.0e-4) vibrato = 0.0;
        const double vib = juce::jlimit (-120.0, 120.0, y * gate);
        const double leak = 1.0 - trackerSeconds / 1.0;
        const bool precise = vibratoPreciseTarget;
        if (precise)
        {
            fitOriginAge += trackerSeconds; // the sinusoid in use runs on, so while no new fit is taken it keeps moving as it fades
            if (vibrato != 0.0) solveFit(); else fitGood = false;
            fitGate += juce::jlimit (-fitGateStep, fitGateStep, (fitGood ? 1.0 : 0.0) - fitGate);
        }
        else
        {
            fitGate = 0.0; // switched back on, it fades in from nothing instead of where it was left
        }
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            // Pitch follows the slope of the delay: 1 cent is 1/1731 of real time. The slow leak keeps the delay from walking away.
            const double cents = vibratoShare[v] * vibrato * (precise ? preciseVibrato (v) : vib);
            bend[v] = juce::jlimit (-maxBendMs, maxBendMs, (bend[v] - cents / 1731.2 * trackerSeconds * 1000.0) * leak);
            bendTarget[v] = bend[v];
        }
    }

    // ------------------------------------------------------------ Precise Vibrato

    static constexpr int fitFrames = 128, fitRates = 12;
    static constexpr double fitCycles = 1.5;      // the window is this many cycles of the vibrato speed being tried
    static constexpr double fitMinShare = 0.5;    // of what a straight line leaves, the sinusoid must explain this much
    static constexpr double fitMinCents = 3.0;    // smaller than this is not vibrato
    static constexpr double fitMaxTrend = 3.0;    // the line may move this many times the sinusoid's size across the window; more is a slide
    static constexpr double fitGateStep = 0.1;    // opens and shuts in 10 frames, about 50 ms
    static constexpr double bendLandsSeconds = 0.008; // the two smoothers after the bend put it this much later
    static double fitRateHz (int j) { return 3.5 + 0.5 * j; } // 3.5 to 9 Hz; the two ends are only there to be refused

    void pushFit (bool voiced) noexcept
    {
        if (fitJumpAge >= 0.0) fitJumpAge += trackerSeconds;
        double c = fitLast;
        if (voiced)
        {
            c = 1200.0 * std::log2 (trackedHz / 55.0);
            if (! fitHave) fitRecent.fill (c);
            const double before = fitRecent[(size_t) fitRecentPos]; // 8 frames ago, about 40 ms
            fitRecent[(size_t) fitRecentPos] = c;
            fitRecentPos = (fitRecentPos + 1) % (int) fitRecent.size();
            const bool jump = fitHave && std::abs (c - before) > 150.0;
            if (! fitHave || jump) fitSince = 0; // a new note: the window starts over on it
            // The sinusoid in use belongs to the old note. Remember how long ago the new one came in (the first frame that shows it
            // only), so no double is bent by the old vibrato once it plays the new note.
            if (jump && fitJumpAge < 0.0) fitJumpAge = trackerAge;
            fitHave = true;
            fitLast = c;
            fitAge = trackerAge;
        }
        fitHead = (fitHead + 1) % fitFrames;
        fitCents[(size_t) fitHead] = c;
        fitOk[(size_t) fitHead] = voiced ? 1 : 0;
        if (fitSince < fitFrames) ++fitSince;
    }

    // Weighted least squares of 1, t, cos wt, sin wt on the pitch of the last cycle and a half, for each speed. The best speed is
    // the one whose sinusoid explains the largest share of what a straight line leaves. Refused: the slowest and the fastest speed
    // tried (a slide or a new note fits best there), a fit that explains too little or is too small to be vibrato, and a line
    // that moves much further than the sinusoid swings (a slide with a wobble on it).
    void solveFit() noexcept
    {
        fitGood = false;
        double best = -1.0, bestAmp = 0.0, bestTrend = 0.0, bestA = 0.0, bestB = 0.0;
        int bestJ = -1;
        for (int j = 0; j < fitRates; ++j)
        {
            const int n = fitLength[(size_t) j];
            if (n > fitSince) continue;
            const double* cs = fitCos.data() + j * fitFrames;
            const double* sn = fitSin.data() + j * fitFrames;
            double m[4][5] = {};
            double yy = 0.0;
            int valid = 0;
            for (int k = 0; k < n; ++k)
            {
                const int idx = (fitHead - k + fitFrames) % fitFrames;
                if (! fitOk[(size_t) idx]) continue;
                ++valid;
                const double t = -trackerSeconds * k, y = fitCents[(size_t) idx] - fitLast;
                const double b[4] = { 1.0, t, cs[k], sn[k] };
                for (int r = 0; r < 4; ++r)
                {
                    for (int c = r; c < 4; ++c) m[r][c] += b[r] * b[c];
                    m[r][4] += b[r] * y;
                }
                yy += y * y;
            }
            if (valid < juce::jmax (24, (int) (0.8 * n))) continue;
            for (int r = 1; r < 4; ++r) for (int c = 0; c < r; ++c) m[r][c] = m[c][r];
            const double rhs[4] = { m[0][4], m[1][4], m[2][4], m[3][4] };
            // what a straight line alone leaves
            const double det = m[0][0] * m[1][1] - m[0][1] * m[0][1];
            if (std::abs (det) < 1.0e-12) continue;
            const double la = (rhs[0] * m[1][1] - rhs[1] * m[0][1]) / det, lb = (m[0][0] * rhs[1] - m[0][1] * rhs[0]) / det;
            const double leftByLine = juce::jmax (1.0e-9, yy - la * rhs[0] - lb * rhs[1]);
            // the four together, by elimination
            bool singular = false;
            for (int col = 0; col < 4 && ! singular; ++col)
            {
                int piv = col;
                for (int r = col + 1; r < 4; ++r) if (std::abs (m[r][col]) > std::abs (m[piv][col])) piv = r;
                if (std::abs (m[piv][col]) < 1.0e-12) { singular = true; break; }
                if (piv != col) for (int c = 0; c < 5; ++c) std::swap (m[col][c], m[piv][c]);
                for (int r = col + 1; r < 4; ++r)
                {
                    const double g = m[r][col] / m[col][col];
                    for (int c = col; c < 5; ++c) m[r][c] -= g * m[col][c];
                }
            }
            if (singular) continue;
            double p[4] = {};
            for (int r = 3; r >= 0; --r)
            {
                double sum = m[r][4];
                for (int c = r + 1; c < 4; ++c) sum -= m[r][c] * p[c];
                p[r] = sum / m[r][r];
            }
            const double leftByFit = juce::jmax (0.0, yy - (p[0] * rhs[0] + p[1] * rhs[1] + p[2] * rhs[2] + p[3] * rhs[3]));
            const double share = 1.0 - leftByFit / leftByLine;
            if (share > best)
            {
                best = share; bestJ = j;
                bestA = p[2]; bestB = p[3];
                bestAmp = std::sqrt (p[2] * p[2] + p[3] * p[3]);
                bestTrend = std::abs (p[1]) * trackerSeconds * n;
            }
        }
        fitGood = best >= fitMinShare && bestAmp >= fitMinCents && bestAmp <= 150.0 && bestJ >= 1 && bestJ <= fitRates - 2 && bestTrend <= fitMaxTrend * bestAmp;
        // Only a fit that passes is used. A refused one (a slide, a new note) never bends a double: the last good one runs on
        // while the gate shuts.
        if (fitGood)
        {
            fitA = bestA; fitB = bestB;
            fitOmega = juce::MathConstants<double>::twoPi * fitRateHz (bestJ);
            fitOriginAge = fitAge;
            fitJumpAge = -1.0;
        }
    }

    // The vibrato, in cents, of the sound double v is playing now. That sound left the input one delay ago; the frame the fit
    // ends on is fitOriginAge old (the newest pitch reading when the fit is new); and the bend made now lands bendLandsSeconds
    // later. So the sinusoid is read this far past its newest frame (often a few ms into the future of the readings, which a
    // sinusoid can give and a filter cannot). Once the double plays a note that came in after the fit, it gets nothing.
    double preciseVibrato (size_t v) const noexcept
    {
        if (fitGate <= 0.0) return 0.0;
        const double playing = delayNowMs[v] * 0.001 - bendLandsSeconds; // how long ago the sound the bend lands on was sung
        if (fitJumpAge >= 0.0 && playing <= fitJumpAge) return 0.0;
        const double t = fitOriginAge - delayNowMs[v] * 0.001 + bendLandsSeconds;
        return juce::jlimit (-120.0, 120.0, fitGate * (fitA * std::cos (fitOmega * t) + fitB * std::sin (fitOmega * t)));
    }

    // ------------------------------------------------------------ controls

    void computeTargets (const Params& p, bool forceBypass, int numOutputs)
    {
        const bool off = p.bypass || forceBypass;
        const int n = juce::jlimit (1, (int) maxDoubles, p.doubles);
        const bool stereo = numOutputs > 1;
        const double width = stereo ? juce::jlimit (0.0, 1.0, (double) p.widthPct * 0.01) : 0.0;
        const double doublesPan = stereo ? juce::jlimit (-1.0, 1.0, (double) p.doublesPanPct * 0.01) : 0.0;
        const double leadPan = stereo ? juce::jlimit (-1.0, 1.0, (double) p.leadPanPct * 0.01) : 0.0;

        const auto sides = [] (double pan, float& l, float& r, float g)
        {
            // Equal power, and exactly 1 on both sides in the centre, so centred and Width 0 leave left and right identical.
            const double th = juce::jlimit (-1.0, 1.0, pan) * juce::MathConstants<double>::pi * 0.25;
            const double c = std::cos (th), s = std::sin (th), a = c - s, b = c + s;
            l = std::abs (a) < 1.0e-9 ? 0.0f : g * (float) a;
            r = std::abs (b) < 1.0e-9 ? 0.0f : g * (float) b;
        };

        sides (off ? 0.0 : leadPan, leadLTarget, leadRTarget, off ? 1.0f : levelToGain (p.leadDb));
        looseTarget = juce::jlimit (0.0, 1.0, (double) p.loosePct * 0.01);
        timingTarget = juce::jlimit (0.0, 2.0, (double) p.timingPct * 0.01);
        pitchTarget = juce::jlimit (0.0, 2.0, (double) p.pitchPct * 0.01);
        vibratoTarget = juce::jlimit (0.0, 1.0, (double) p.vibratoPct * 0.01);
        timbreTarget = juce::jlimit (0.0, 1.0, (double) p.timbrePct * 0.01);
        extraTarget = juce::jlimit (0.0, maxExtraMs, (double) p.extraMs);
        rangeTarget = p.range;
        vibratoPreciseTarget = p.vibratoPrecise;
        const bool monoSafe = p.monoSafe && stereo;
        for (size_t v = 0; v < maxDoubles; ++v)
        {
            const float g = (off || (int) v >= n) ? 0.0f : levelToGain (p.levelDb[v]);
            if (monoSafe)
            {
                // The same double on both sides with opposite signs, at the level it has anywhere else (every pan gives a
                // double the same power, taken over both sides). Left plus right then holds no double at all. One read head
                // only: two different side heads would not cancel. Width and Doubles Pan have nothing to move here.
                const float sign = v % 2 == 0 ? 1.0f : -1.0f;
                gainLTarget[v] = sign * g;
                gainRTarget[v] = -sign * g;
                centreTarget[v] = 1.0f;
                sideTarget[v] = 0.0f;
                continue;
            }
            sides ((double) panAtFullWidth[n - 1][v] * width + doublesPan, gainLTarget[v], gainRTarget[v], g);
            // Left and right differ only through the side heads, so correlation falls smoothly as the blend moves to them.
            const double phi = width * (double) splitAtFullWidth[n - 1][v] * juce::MathConstants<double>::halfPi;
            const double cw = std::cos (phi), sw = std::sin (phi);
            centreTarget[v] = cw < 1.0e-9 ? 0.0f : (float) cw;
            sideTarget[v] = sw < 1.0e-9 ? 0.0f : (float) sw;
        }

        if (p.matchLevel && ! off)
        {
            // The voices are separate enough to add in power (measured on real singing: within 0.2 dB of this sum). Take that sum
            // for each side, average the sides, and turn everything down by it. Every voice gets the same factor, so the balance
            // between the lead and the doubles, the pans and the width stay exactly as set.
            double left = (double) leadLTarget * leadLTarget, right = (double) leadRTarget * leadRTarget;
            for (size_t v = 0; v < maxDoubles; ++v)
            {
                const double heads = (double) centreTarget[v] * centreTarget[v] + (double) sideTarget[v] * sideTarget[v];
                left += (double) gainLTarget[v] * gainLTarget[v] * heads;
                right += (double) gainRTarget[v] * gainRTarget[v] * heads;
            }
            const double power = stereo ? 0.5 * (left + right) : left;
            if (power > 1.0e-6)
            {
                const auto k = (float) (1.0 / std::sqrt (power));
                leadLTarget *= k;
                leadRTarget *= k;
                for (size_t v = 0; v < maxDoubles; ++v)
                {
                    gainLTarget[v] *= k;
                    gainRTarget[v] *= k;
                }
            }
        }
    }

    double sr = 48000.0, msToSamples = 48.0, maxDelaySamples = 0.0;
    int size = 0, mask = 0, writeIndex = 0;
    std::array<std::vector<float>, maxDoubles> lines;
    std::vector<float> kernel;

    std::array<DriftCurve, maxDoubles> slow, quick, side;
    using Linear = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    Linear leadL, leadR;
    std::array<Linear, maxDoubles> gainL, gainR, centreWeight, sideWeight;
    float leadLTarget = 1.0f, leadRTarget = 1.0f;
    std::array<float, maxDoubles> gainLTarget {}, gainRTarget {}, centreTarget {}, sideTarget {};
    double loose = 0.3, looseTarget = 0.3, timingAmount = 1.0, timingTarget = 1.0, pitchAmount = 1.0, pitchTarget = 1.0;
    double extra = 0.0, extraTarget = 0.0, slowCoef = 0.0, extraCoef = 0.0;

    // Timbre Variation
    std::unique_ptr<juce::dsp::FFT> fft, analysisFft;
    int frameSamples = 512, hopSamples = 128, analysisSamples = 1024, fifoPos = 0, olaPos = 0, hopCount = 0;
    std::vector<float> hann, analysisHann, fifo, frame, spectrum, scratch, analysis, envelope, logAverage;
    float logAverageCoef = 0.15f;
    std::array<std::vector<float>, maxDoubles> overlap, logGain;
    std::array<float, maxDoubles> levelFix {}, spectrumOut {};
    std::array<bool, maxDoubles> shifted {};
    double timbre = 0.0, timbreTarget = 0.0;

    // Vibrato Variation
    static constexpr int trackerRingSize = 1024;
    int trackerFactor = 8, trackerWindow = 180, trackerHop = 30, trackerPos = 0, trackerCount = 0, trackerSkip = 0, rangeTarget = altoTenor;
    double trackerRate = 6000.0, trackerSeconds = 0.005, trackerLp = 0.0, lp1 = 0.0, lp2 = 0.0, trackedHz = 0.0;
    std::vector<float> trackerRing, trackerDiff, trackerLinear;
    std::array<double, 8> recent {};
    int recentPos = 0, hold = 0;
    bool haveCents = false;
    double lastCents = 0.0, gate = 0.0, vibrato = 0.0, vibratoTarget = 0.0;
    double bp0 = 0.0, bp1 = 0.0, bp2 = 0.0, bpX1 = 0.0, bpX2 = 0.0, bpY1 = 0.0, bpY2 = 0.0;
    std::array<double, maxDoubles> bend {}, bendTarget {}, bendNow {}, bendSmooth {};
    double bendStep = 0.0, bendSmoothCoef = 0.0;

    // Precise Vibrato
    std::array<double, fitFrames> fitCents {};
    std::array<unsigned char, fitFrames> fitOk {};
    std::array<double, 8> fitRecent {};
    std::array<int, fitRates> fitLength {};
    std::vector<double> fitCos, fitSin;
    int fitHead = 0, fitSince = 0, fitRecentPos = 0;
    bool fitHave = false, fitGood = false, vibratoPreciseTarget = true;
    double fitLast = 0.0, fitGate = 0.0, fitA = 0.0, fitB = 0.0, fitOmega = 0.0, fitAge = 0.025, trackerAge = 0.025;
    double fitOriginAge = 0.025;  // how old the newest frame of the sinusoid in use is
    double fitJumpAge = -1.0;     // how long ago a new note came in after the sinusoid in use was fitted, -1 for none
    std::array<double, maxDoubles> delayNowMs {};

    // Readings for a later window
    std::array<std::atomic<float>, maxDoubles> readDelayMs {};
    std::atomic<float> readPitchHz { 0.0f };
};
