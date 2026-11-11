#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#include "FastOversampler.h"
#include "CleanOversampler.h"

// The whole signal path, free of plug-in framework code.
//
// Wet path, per input channel:
//   pre-emphasis > 4x (2x at high rates) oversampled soft clip > de-emphasis > DC block > tape delay line
// Then up to 8 voices read each delay line through a modulated, interpolated read head.
// Each voice has a left and a right read head. Spread moves them apart in time and sweep, and places the voices
// across the stereo field. The voices are summed, Low Cut and High Cut shape the sum, part of it goes back to the
// delay line as Feedback, Tone shapes the top end, and Mix blends it with the dry signal.
// Input Gain sits before the split into dry and wet, Output Gain after the blend. Bypass fades both to unity.
//
// The oversampling filters' delay is taken off the read head's delay, so the delay you set is the delay you get,
// and the plug-in itself has zero latency. The dry path therefore needs no delay at all.
class TapeChorusEngine
{
public:
    static constexpr int maxVoices = 8;
    static constexpr float maxWowMs = 2.5f;      // peak delay swing of Wow at 100 percent
    static constexpr float maxFlutterMs = 0.25f; // peak delay swing of Flutter at 100 percent
    static constexpr float maxSpreadOffsetMs = 1.5f;  // each side moves this far from centre at Spread 100
    static constexpr float maxSatDrive = 6.0f;
    static constexpr float maxRandomMs = 2.0f;   // peak delay swing of Random at 100 percent
    static constexpr float maxOffsetMs = 7.0f;   // furthest any voice sits from the centre delay
    static constexpr float maxFeedbackPct = 90.0f;
    static constexpr float lowCutOffHz = 20.0f;  // at this value Low Cut is out of the path
    static constexpr float highCutOffHz = 20000.0f; // at this value High Cut is out of the path
    static constexpr float defaultWowHz = 0.53f;
    static constexpr float defaultFlutterHz = 7.1f;
    static constexpr float maxNoiseGain = 0.01f; // Noise 100 percent is -40 dBFS rms

    // A picture of the tape for the window, one row about 250 times a second. The audio thread only writes it, nothing in the sound
    // ever reads it, and the window reads it with no lock. Relaxed atomics, so a row read during a write is at worst a mix of two neighbours.
    struct TraceRow
    {
        std::array<std::array<std::atomic<float>, maxVoices>, 2> delayMs {};   // where each read head is. 0 when that voice is silent
        std::array<std::atomic<float>, maxVoices> voiceGain {};                // the voice's fade and Degrade dip
        std::atomic<float> tapePeak { 0.0f };                                  // loudest sample written to the tape since the row before
        std::atomic<float> centreMs { 15.0f };                                 // the centre delay at this moment
    };
    static constexpr uint32_t traceRows = 2048;

    struct Params
    {
        int voices = 2;
        float rateHz = 0.8f, depthMs = 1.5f, delayMs = 15.0f, wowPct = 15.0f, flutterPct = 10.0f;
        float satPct = 20.0f, toneDb = -2.0f, spreadPct = 70.0f, mixPct = 35.0f;
        bool bypass = false;
        float feedbackPct = 0.0f, inputDb = 0.0f, outputDb = 0.0f, lowCutHz = lowCutOffHz, highCutHz = highCutOffHz;
        float wowHz = defaultWowHz, flutterHz = defaultFlutterHz;
        float randomPct = 0.0f, randomHz = 1.0f, degradePct = 0.0f, degradeHz = 2.0f, noisePct = 0.0f;
        bool unsync = false, noiseGate = true;
        // Added in 0.4.0. Off is the sound of every version before it, sample for sample.
        bool clean = false;
    };

    void prepare (double sampleRate, const Params& p)
    {
        sr = sampleRate;
        // At 96 kHz, 2x already puts the clip stage at 192 kHz.
        const size_t stages = sr < 60000.0 ? 2u : 1u;
        os = std::make_unique<FastOversampler> (2, stages, false);
        os->initProcessing ((size_t) tickLen);
        os->reset();
        wetLatency = (double) os->getLatencyInSamples();
        cleanOs.prepare (1 << (int) stages, wetLatency, 2, tickLen);
        cleanOs.reset();
        cleanAmt = cleanAmtNext = p.clean ? 1.0f : 0.0f;
        cleanRunning = p.clean; standardRunning = ! p.clean; cleanWarm = 0;
        wowSmoothA = wowSmoothB = flutSmoothA = flutSmoothB = 0.0;
        minDelayMs = std::max (1.0, (wetLatency + 4.0) * 1000.0 / sr);

        const auto maxMs = 40.0 + 8.0 + maxWowMs + maxFlutterMs + maxRandomMs + maxOffsetMs + maxSpreadOffsetMs + 2.0;
        const int needed = (int) std::ceil (maxMs * 0.001 * sr + wetLatency) + 8;
        int size = 1024;
        while (size < needed) size *= 2;
        mask = size - 1;
        for (auto& l : line) l.assign ((size_t) size, 0.0f);
        writeIndex = 0;

        tickSec = tickLen / sr;
        // Smoothers run once per tick.
        cSlow = coef (0.12); cMid = coef (0.06); cFast = coef (0.03); cVoice = coef (0.25); cVoiceSoft = coef (0.08); cVoiceFade = coef (0.008);
        gainSlew = (float) (tickSec / 0.03);   // a voice fades in or out in 30 ms
        mixSlew = (float) (tickSec / 0.02);

        // Fixed filters
        {
            const double K = std::tan (juce::MathConstants<double>::pi * 3000.0 / sr), V = 2.0;
            preB0 = (V + K) / (1.0 + K); preB1 = (K - V) / (1.0 + K); preA1 = (K - 1.0) / (1.0 + K);
        }
        dcR = std::exp (-juce::MathConstants<double>::twoPi * 6.0 / sr);
        dcRClean = std::exp (-juce::MathConstants<double>::twoPi * 1.0 / sr);
        { const double K = std::tan (juce::MathConstants<double>::pi * 2500.0 / sr); toneA = K / (1.0 + K); }
        wowHz = p.wowHz; flutterHz = p.flutterHz;
        setWowFilters(); setFlutterFilters();
        loopLpA = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 12000.0 / sr);
        loopHpA = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 40.0 / sr);
        meterDecay = std::exp (-1.0 / (0.3 * sr));
        gateAttack = 1.0 - std::exp (-1.0 / (0.005 * sr));
        gateRelease = 1.0 - std::exp (-1.0 / (0.04 * sr));
        cGateUp = coef (0.005); cGateDown = coef (0.06);

        for (auto& s : chan) s = {};
        lfo = wowA = wowB = flutA = flutB = 0.0;
        wowNoise = flutLo = flutHi = 0.0;
        rngState = 0x1234567u;
        slowRng = 0x2bcd1357u; hissRngA = 0x3e7a9c11u; hissRngB = 0x51f2d4b9u;
        for (auto& side : randomNoise) for (auto& n : side) n = {};
        for (auto& side : randomVal) side.fill (0.0);
        for (auto& n : degradeNoise) n = {};
        dip.fill (1.0f);
        for (auto& side : feedbackRing) side.fill (0.0f);
        ringPos = 0;
        ringLen = juce::jlimit (1, (int) feedbackRing[0].size(), juce::roundToInt (wetLatency));
        random = p.randomPct * 0.01f * maxRandomMs; randomHz = p.randomHz; degradeAmt = p.degradePct * 0.01f; degradeHz = p.degradeHz;
        setRandomFilter(); setDegradeFilter();
        unsyncAmt = p.unsync ? 1.0f : 0.0f;
        inRaw = gainFromDb (p.inputDb); outRaw = gainFromDb (p.outputDb);
        fb = fbNext = feedbackFrom (p);
        hiss = hissNext = noiseGain (p);
        gate = gateNext = p.noiseGate ? 0.0f : 1.0f;
        gateEnv = 0.0; inPeak = outPeak = 0.0; gateOpen = false;
        lowLog = (float) std::log (p.lowCutHz); highLog = (float) std::log (p.highCutHz);
        setCutCoefs();
        lowAmt = lowAmtNext = lowCutIsOn (p) ? 1.0f : 0.0f;
        highAmt = highAmtNext = highCutIsOn (p) ? 1.0f : 0.0f;

        depth = p.depthMs; rate = p.rateHz; centre = p.delayMs; wow = p.wowPct * 0.01f * maxWowMs; flutter = p.flutterPct * 0.01f * maxFlutterMs;
        spread = p.spreadPct * 0.01f; drive = p.satPct * 0.01f * maxSatDrive; toneG = juce::Decibels::decibelsToGain (p.toneDb);
        mix = p.mixPct * 0.01f;
        on = p.bypass ? 0.0f : 1.0f;
        inGain = inGainNext = 1.0f + on * (inRaw - 1.0f);
        outGain = outGainNext = 1.0f + on * (outRaw - 1.0f);
        setBlendGains (dryGainNext, wetGainNext);
        dryGain = dryGainNext; wetGain = wetGainNext;
        const int n = juce::jlimit (1, maxVoices, p.voices);
        lfoSideTurn = 0.5f / (float) n;
        for (int v = 0; v < maxVoices; ++v)
        {
            voiceOffset[(size_t) v] = offsetAim[(size_t) v] = offsetFor (n, v);
            voicePhase[(size_t) v] = phaseAim[(size_t) v] = phaseFor (n, v);
            voicePan[(size_t) v] = panFor (n, v);
            voiceGain[(size_t) v] = voiceGainNext[(size_t) v] = gainAim[(size_t) v] = v < n ? 1.0f / std::sqrt ((float) n) : 0.0f;
        }
        reach = targetReach (n);
        computeSideGains();
        sideGain = sideGainNext;
        tapSum = tapSumNext;
        tapShare = tapShareNext;
        toneGNext = toneG; driveNext = drive;
        target = p;
        computeDelays (delayNext);
        delayCur = delayNext;
        for (auto& side : delayStep) side.fill (0.0);
        tickLeft = 0;
        traceTicks = std::max (1, juce::roundToInt (sr / (double) tickLen / 250.0));
        traceTicksLeft = traceTicks;
        traceHz.store ((float) (sr / (double) (tickLen * traceTicks)), std::memory_order_relaxed);
    }

    double getWetLatencySamples() const { return wetLatency; }
    double getMinDelayMs() const { return minDelayMs; }

    // Readings for meters and a later window. Written by the audio thread, safe to read from anywhere.
    float getInputPeak() const noexcept { return readInPeak.load (std::memory_order_relaxed); }
    float getOutputPeak() const noexcept { return readOutPeak.load (std::memory_order_relaxed); }
    // Largest pitch movement the Rate and Depth sweep gives right now, in cents.
    float getPitchSwingCents() const noexcept { return readPitchSwing.load (std::memory_order_relaxed); }
    // Where a voice's read head is now, in ms. 0 when that voice is silent.
    float getVoiceDelayMs (int side, int voice) const noexcept { return readDelayMs[(size_t) side][(size_t) voice].load (std::memory_order_relaxed); }

    // How many rows have been written, ever. The newest is at (count - 1) % traceRows. Rows go back traceRows less a safety margin.
    uint32_t getTraceCount() const noexcept { return traceCount.load (std::memory_order_acquire); }
    float getTraceHz() const noexcept { return traceHz.load (std::memory_order_relaxed); }
    const TraceRow& getTraceRow (uint32_t index) const noexcept { return (*trace)[(size_t) (index & (traceRows - 1))]; }

    // The layout of the voices, for a window drawing what a setting will do while no audio runs.
    static float voiceOffsetMs (int numVoices, int voice) { return offsetFor (numVoices, voice); }
    static float voicePhaseTurns (int numVoices, int voice) { return phaseFor (numVoices, voice); }

    // Input is read from the first numIn channels and output written to the first numOut channels, in place.
    void process (float* const* io, int numIn, int numOut, int numSamples, const Params& p, bool forceBypass)
    {
        target = p;
        target.bypass = p.bypass || forceBypass;
        if (numOut < 2) target.spreadPct = 0.0f; // one output has no left and right to spread across
        for (int pos = 0; pos < numSamples;)
        {
            if (tickLeft == 0) startTick();
            const int n = std::min (tickLeft, numSamples - pos);
            renderChunk (io, numIn, numOut, pos, n);
            tickLeft -= n;
            pos += n;
        }
        publishReadings();
    }

private:
    static constexpr int tickLen = 32;

    double coef (double seconds) const { return 1.0 - std::exp (-tickSec / seconds); }

    // Copies sit evenly around the centre delay, so the average delay is always the one you set.
    static float offsetFor (int n, int v)
    {
        static constexpr float table[maxVoices][maxVoices] = {
            { 0.0f },
            { -2.0f, 2.0f },
            { -3.0f, 0.0f, 3.0f },
            { -4.5f, -1.5f, 1.5f, 4.5f },
            // From 5 voices up the copies sit about 2 ms apart, a little unevenly so that no one note has them all lining up.
            // Spread moves the two sides 3 ms apart, so left and right copies fall between each other.
            { -4.1f, -1.9f, 0.0f, 1.9f, 4.1f },
            { -3.8f, -2.2f, -0.8f, 0.8f, 2.2f, 3.8f },
            { -6.0f, -4.1f, -1.9f, 0.0f, 1.9f, 4.1f, 6.0f },
            { -7.0f, -4.9f, -3.1f, -0.9f, 0.9f, 3.1f, 4.9f, 7.0f } };
        return table[n - 1][v];
    }
    static float phaseFor (int n, int v) { return (float) v / (float) n; }
    // Where each copy sits at full Spread, -1 left to +1 right. Always balanced around the centre.
    static float panFor (int n, int v)
    {
        static constexpr float table[maxVoices][maxVoices] = {
            { 0.0f },
            { -0.7f, 0.7f },
            { -0.7f, 0.7f },
            { -0.7f, 0.7f, -0.7f, 0.7f },
            // Left and right take turns, so at full Spread the two sides are mostly different copies.
            { -0.95f, 0.95f, 0.0f, -0.95f, 0.95f },
            { -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f },
            { -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f },
            { -0.85f, 0.85f, -0.85f, 0.85f, -0.85f, 0.85f, -0.85f, 0.85f } };
        return table[n - 1][v];
    }

    // Equal power, and exactly 1 on both sides at the centre, so Spread 0 leaves left and right identical.
    void computeSideGains()
    {
        for (size_t v = 0; v < maxVoices; ++v)
        {
            const double th = (double) voicePan[v] * spread * juce::MathConstants<double>::pi * 0.25;
            const double c = std::cos (th), sn = std::sin (th);
            sideGainNext[0][v] = voiceGainNext[v] * dip[v] * (float) (c - sn);
            sideGainNext[1][v] = voiceGainNext[v] * dip[v] * (float) (c + sn);
        }
        for (size_t s = 0; s < 2; ++s)
        {
            float sum = 0.0f;
            for (size_t v = 0; v < maxVoices; ++v)
                sum += std::abs (sideGainNext[s][v]);
            tapSumNext[s] = sum;
        }
        // Energy of the sum against the sum of the gains squared: 1 with one voice, 1 over the count with several equal ones.
        double energy = 0.0;
        for (size_t v = 0; v < maxVoices; ++v) energy += (double) sideGainNext[0][v] * sideGainNext[0][v];
        tapShareNext = (float) (energy / std::max ((double) tapSumNext[0] * tapSumNext[0], 1.0e-6));
    }

    // Furthest a voice sits from the centre. Silent voices do not count, so going from 8 voices back to 2 gives the same room as 2 always had.
    float targetReach (int n) const
    {
        float most = 0.0f;
        for (int v = 0; v < maxVoices; ++v)
        {
            const auto i = (size_t) v;
            if (v < n) most = std::max (most, std::abs (offsetFor (n, v)));
            else if (voiceGain[i] != 0.0f || voiceGainNext[i] != 0.0f) most = std::max (most, std::abs (voiceOffset[i]));
        }
        return most;
    }

    // Exactly 1 at 0 dB. A control's stored 0 can come back a few millionths off, and the dry path must stay untouched.
    static float gainFromDb (float db) { return std::abs (db) < 0.005f ? 1.0f : juce::Decibels::decibelsToGain (db); }

    static float feedbackFrom (const Params& p) { return juce::jlimit (0.0f, maxFeedbackPct, p.feedbackPct) * 0.01f; }
    static bool lowCutIsOn (const Params& p) { return p.lowCutHz > lowCutOffHz + 0.01f; }
    static bool highCutIsOn (const Params& p) { return p.highCutHz < highCutOffHz - 0.5f; }
    static float noiseGain (const Params& p)
    {
        const float x = p.noisePct * 0.01f;
        // The source is uniform, so its rms is 1 over the square root of 3. The read heads' interpolation takes 0 to 1.5 dB off white noise,
        // depending on where they sit between samples, so 0.75 dB is added to land in the middle.
        return maxNoiseGain * x * x * 1.7320508f * 1.09f;
    }

    // Exactly 1 at the default rate, so the default motion is what it always was.
    static double rateRatio (float hz, float defaultHz) { return std::abs (hz - defaultHz) < 5.0e-4f ? 1.0 : (double) hz / (double) defaultHz; }

    void setWowFilters()
    {
        wowRatio = rateRatio (wowHz, defaultWowHz);
        wowNoiseC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 0.9 * wowRatio * tickSec);
        wowSmoothC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 3.6 * wowRatio * tickSec);
    }

    void setFlutterFilters()
    {
        flutterRatio = rateRatio (flutterHz, defaultFlutterHz);
        flutNoiseLoC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 16.0 * flutterRatio * tickSec);
        flutNoiseHiC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 5.0 * flutterRatio * tickSec);
        flutSmoothC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 60.0 * flutterRatio * tickSec);
    }

    // Two one pole low passes in a row, from white noise. The scale gives an rms of 0.4 at any rate, so the swing does not change with speed.
    struct SlowNoise
    {
        double a = 0, b = 0;
        double step (double white, double c) { a += c * (white - a); b += c * (a - b); return b; }
    };
    struct SlowNoiseShape { double c = 0.1, scale = 1.0; };

    SlowNoiseShape shapeFor (double hz) const
    {
        SlowNoiseShape sh;
        // Each pole at 1.55 times the rate puts the corner of the pair at the rate.
        sh.c = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 1.55 * hz * tickSec);
        const double r = (1.0 - sh.c) * (1.0 - sh.c), one = 1.0 - r;
        const double variance = sh.c * sh.c * sh.c * sh.c * (1.0 + r) / (one * one * one) / 3.0;
        sh.scale = 0.4 / std::sqrt (variance);
        return sh;
    }

    void setRandomFilter() { randomShape = shapeFor (randomHz); }
    void setDegradeFilter() { degradeShape = shapeFor (degradeHz); }

    void setCutCoefs()
    {
        lowCoef.set (std::exp ((double) lowLog), sr);
        highCoef.set (std::exp ((double) highLog), sr);
    }

    void publishReadings()
    {
        readInPeak.store ((float) inPeak, std::memory_order_relaxed);
        readOutPeak.store ((float) outPeak, std::memory_order_relaxed);
        const double slope = juce::MathConstants<double>::twoPi * rate * depth * scaleNow * 0.001;
        readPitchSwing.store ((float) (1200.0 * std::log2 (1.0 + slope)), std::memory_order_relaxed);
        for (size_t s = 0; s < 2; ++s)
            for (size_t v = 0; v < maxVoices; ++v)
                readDelayMs[s][v].store (voiceGainNext[v] > 0.0f ? (float) ((delayNext[s][v] + wetLatency) * 1000.0 / sr) : 0.0f, std::memory_order_relaxed);
    }

    static double sinTurns (double t) { return std::sin (juce::MathConstants<double>::twoPi * t); }

    uint32_t nextRandom()
    {
        rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
        return rngState;
    }

    void computeDelays (std::array<std::array<double, maxVoices>, 2>& dest)
    {
        const double excursion = (double) reach + spread * maxSpreadOffsetMs + depth + wow + flutter + random;
        const double room = centre - minDelayMs;
        // Everything shrinks together when the delay is too short to hold it, so the read head never reaches the write head.
        const double scale = excursion > room ? std::max (room, 0.0) / excursion : 1.0;
        scaleNow = scale;

        // The irregular part of Wow and Flutter is noise through one filter pole, which leaves a little of it at every speed up to the
        // 1500 steps a second it is made at: hash around a held note (42 dB under a 3 kHz tone at Wow 15 and Flutter 10, 24 dB under
        // at Flutter 100). Clean runs it through two more poles, so only the slow part that sounds like tape is left.
        const double flutRaw = flutLo - flutHi;
        const double wowIrregular = cleanAmtNext > 0.0f ? wowNoise + (double) cleanAmtNext * (wowSmoothB - wowNoise) : wowNoise;
        const double flutIrregular = cleanAmtNext > 0.0f ? flutRaw + (double) cleanAmtNext * (flutSmoothB - flutRaw) : flutRaw;

        for (int side = 0; side < 2; ++side)
        {
            const double sideTurn = side * spread;
            const double sideOffset = (side == 0 ? -1.0 : 1.0) * spread * maxSpreadOffsetMs;
            for (int v = 0; v < maxVoices; ++v)
            {
                const double w = 0.62 * sinTurns (wowA + 0.37 * v + 0.25 * sideTurn)
                               + 0.26 * sinTurns (wowB + 0.61 * v + 0.40 * sideTurn)
                               + 0.12 * wowIrregular;
                const double f = 0.55 * sinTurns (flutA + 0.23 * v + 0.25 * sideTurn)
                               + 0.28 * sinTurns (flutB + 0.71 * v + 0.40 * sideTurn)
                               + 0.17 * flutIrregular;
                const double l = sinTurns (lfo + voicePhase[(size_t) v] + (double) lfoSideTurn * sideTurn);
                const double rn = randomVal[0][(size_t) v] + (double) unsyncAmt * side * (randomVal[1][(size_t) v] - randomVal[0][(size_t) v]);
                const double ms = centre + scale * (voiceOffset[(size_t) v] + sideOffset + depth * l + wow * w + flutter * f + random * rn);
                dest[(size_t) side][(size_t) v] = std::max (ms * 0.001 * sr - wetLatency, 3.0); // the read head never reaches the write head
            }
        }
    }

    void startTick()
    {
        tickLeft = tickLen;

        // Lands on the target. Up to 0.3.1 it only did so within 0.00001, and a float holding 40 ms cannot take a step smaller than
        // 0.000004, so Delay Time stopped up to 0.0003 ms short for good, and Voices, Depth, the cuts and Saturation likewise.
        // Now a step too small to move the value any further is the last one.
        const auto smooth = [] (float& v, float t, double c)
        {
            const float next = v + (float) c * (t - v);
            v = (next == v || std::abs (t - next) < 1.0e-5f) ? t : next;
        };
        const auto slew = [] (float& v, float t, float maxStep)
        {
            v += juce::jlimit (-maxStep, maxStep, t - v);
        };

        smooth (rate, target.rateHz, cMid);
        smooth (depth, target.depthMs, cMid);
        smooth (centre, target.delayMs, cSlow);
        smooth (wow, target.wowPct * 0.01f * maxWowMs, cMid);
        smooth (flutter, target.flutterPct * 0.01f * maxFlutterMs, cMid);
        smooth (spread, target.spreadPct * 0.01f, cMid);
        smooth (random, target.randomPct * 0.01f * maxRandomMs, cMid);
        smooth (degradeAmt, target.degradePct * 0.01f, cMid);
        slew (unsyncAmt, target.unsync ? 1.0f : 0.0f, unsyncSlew);

        // Speeds. Filters are only worked out again when a speed moves.
        const auto track = [&] (float& v, float t, auto&& onChange)
        {
            if (v != t) { smooth (v, t, cMid); onChange(); }
        };
        track (wowHz, target.wowHz, [this] { setWowFilters(); });
        track (flutterHz, target.flutterHz, [this] { setFlutterFilters(); });
        track (randomHz, target.randomHz, [this] { setRandomFilter(); });
        track (degradeHz, target.degradeHz, [this] { setDegradeFilter(); });

        // Moving phases
        const auto advance = [this] (double& ph, double hz) { ph += hz * tickSec; if (ph >= 1.0) ph -= 1.0; };
        advance (lfo, rate);
        advance (wowA, 0.53 * wowRatio); advance (wowB, 0.87 * wowRatio);
        advance (flutA, 7.1 * flutterRatio); advance (flutB, 11.3 * flutterRatio);

        // Slow and fast irregular motion, so the transport never sounds like a pure sine wave.
        const auto rnd = [this] { return (double) nextRandom() * (2.0 / 4294967295.0) - 1.0; };
        wowNoise += wowNoiseC * (rnd() * 6.0 - wowNoise);
        wowNoise = juce::jlimit (-1.0, 1.0, wowNoise);
        const double r = rnd() * 4.0;
        flutLo += flutNoiseLoC * (r - flutLo);
        flutHi += flutNoiseHiC * (flutLo - flutHi);
        flutLo = juce::jlimit (-1.0, 1.0, flutLo);
        flutHi = juce::jlimit (-1.0, 1.0, flutHi);
        // For Clean. They always run, so they are ready when it is switched on, and nothing reads them while it is off.
        wowSmoothA += wowSmoothC * (wowNoise - wowSmoothA); wowSmoothB += wowSmoothC * (wowSmoothA - wowSmoothB);
        flutSmoothA += flutSmoothC * ((flutLo - flutHi) - flutSmoothA); flutSmoothB += flutSmoothC * (flutSmoothA - flutSmoothB);

        // Clean fades in and out over 30 ms. Both clip stages run during the fade, and the one that was resting gets 8 ticks (5 ms at
        // 48 kHz) to fill its filters before the fade starts. They have the same delay, so the fade is between two signals in step.
        cleanAmt = cleanAmtNext;
        if (target.clean && ! cleanRunning) { cleanOs.reset(); cleanRunning = true; cleanWarm = 8; }
        if (! target.clean && ! standardRunning) { os->reset(); standardRunning = true; cleanWarm = 8; }
        if (cleanWarm > 0) --cleanWarm;
        else slew (cleanAmtNext, target.clean ? 1.0f : 0.0f, gainSlew);
        // A stage rests only once the fade away from it is over and the switch still points the other way.
        if (target.clean && cleanAmt >= 1.0f && cleanAmtNext >= 1.0f) standardRunning = false;
        if (! target.clean && cleanAmt <= 0.0f && cleanAmtNext <= 0.0f) cleanRunning = false;

        // Random and Degrade have their own noise source, one for each voice.
        const auto slowWhite = [this]
        {
            slowRng ^= slowRng << 13; slowRng ^= slowRng >> 17; slowRng ^= slowRng << 5;
            return (double) slowRng * (2.0 / 4294967295.0) - 1.0;
        };
        for (size_t v = 0; v < maxVoices; ++v)
        {
            for (size_t s = 0; s < 2; ++s)
                randomVal[s][v] = juce::jlimit (-1.0, 1.0, randomNoise[s][v].step (slowWhite(), randomShape.c) * randomShape.scale);
            const double dn = juce::jlimit (-1.0, 1.0, degradeNoise[v].step (slowWhite(), degradeShape.c) * degradeShape.scale);
            dip[v] = 1.0f - degradeAmt * 0.9f * (float) (dn * dn); // mostly small dips, now and then down to -20 dB
        }

        // Voices
        const int n = juce::jlimit (1, maxVoices, target.voices);
        smooth (reach, targetReach (n), cVoice);
        // At full Spread the right side's sweeps sit halfway between the left side's, so no left and right pair ever lock together.
        smooth (lfoSideTurn, 0.5f / (float) n, cVoice);
        for (int v = 0; v < maxVoices; ++v)
        {
            const auto i = (size_t) v;
            const float gainT = v < n ? 1.0f / std::sqrt ((float) n) : 0.0f;
            const float offT = v < n ? offsetFor (n, v) : voiceOffset[i];
            const float phT = v < n ? phaseFor (n, v) : voicePhase[i];
            const float panT = v < n ? panFor (n, v) : voicePan[i];
            if (voiceGain[i] == 0.0f && voiceGainNext[i] == 0.0f)
            {
                // Silent, so it can jump to its new place without anyone hearing it.
                voiceOffset[i] = offsetAim[i] = offT;
                voicePhase[i] = phaseAim[i] = phT;
                voicePan[i] = panT;
            }
            else
            {
                // Two stages, so a head starts and ends its glide gently and the pitch has no corner in it.
                smooth (offsetAim[i], offT, cVoice);
                smooth (voiceOffset[i], offsetAim[i], cVoiceSoft);
                smooth (phaseAim[i], phT, cVoice);
                smooth (voicePhase[i], phaseAim[i], cVoiceSoft);
                smooth (voicePan[i], panT, cVoice);
            }
            voiceGain[i] = voiceGainNext[i];
            slew (gainAim[i], gainT, gainSlew);
            smooth (voiceGainNext[i], gainAim[i], cVoiceFade);
        }

        sideGain = sideGainNext;
        tapSum = tapSumNext;
        tapShare = tapShareNext;
        computeSideGains();

        // Ramps across this tick
        delayCur = delayNext;
        computeDelays (delayNext);
        for (size_t s = 0; s < 2; ++s)
            for (size_t v = 0; v < maxVoices; ++v)
                delayStep[s][v] = (delayNext[s][v] - delayCur[s][v]) / tickLen;

        drive = driveNext;
        smooth (driveNext, target.satPct * 0.01f * maxSatDrive, cFast);
        toneG = toneGNext;
        smooth (toneGNext, juce::Decibels::decibelsToGain (target.toneDb), cFast);
        slew (mix, target.mixPct * 0.01f, mixSlew);
        slew (on, target.bypass ? 0.0f : 1.0f, mixSlew);
        dryGain = dryGainNext; wetGain = wetGainNext;
        setBlendGains (dryGainNext, wetGainNext);

        smooth (inRaw, gainFromDb (target.inputDb), cFast);
        smooth (outRaw, gainFromDb (target.outputDb), cFast);
        inGain = inGainNext; inGainNext = 1.0f + on * (inRaw - 1.0f);
        outGain = outGainNext; outGainNext = 1.0f + on * (outRaw - 1.0f);

        fb = fbNext;
        smooth (fbNext, feedbackFrom (target), cMid);

        const float lowT = (float) std::log (target.lowCutHz), highT = (float) std::log (target.highCutHz);
        if (lowLog != lowT || highLog != highT)
        {
            smooth (lowLog, lowT, cMid);
            smooth (highLog, highT, cMid);
            setCutCoefs();
        }
        lowAmt = lowAmtNext; slew (lowAmtNext, lowCutIsOn (target) ? 1.0f : 0.0f, gainSlew);
        highAmt = highAmtNext; slew (highAmtNext, highCutIsOn (target) ? 1.0f : 0.0f, gainSlew);

        hiss = hissNext;
        smooth (hissNext, noiseGain (target), cFast);
        gate = gateNext;
        if (gateEnv > gateOpenLevel) gateOpen = true;
        else if (gateEnv < gateCloseLevel) gateOpen = false;
        const float gateT = (! target.noiseGate || gateOpen) ? 1.0f : 0.0f;
        gateNext += (float) (gateT > gateNext ? cGateUp : cGateDown) * (gateT - gateNext);
        if (std::abs (gateT - gateNext) < 1.0e-5f) gateNext = gateT;
        { const double a = (double) spread * juce::MathConstants<double>::halfPi; hissCos = (float) std::cos (a); hissSin = (float) std::sin (a); }

        clearIfBad();
        tickPos = 0;

        if (--traceTicksLeft <= 0)
        {
            traceTicksLeft = traceTicks;
            writeTraceRow();
        }
    }

    // Only reads the state above, and writes nothing the sound depends on.
    void writeTraceRow() noexcept
    {
        const uint32_t count = traceCount.load (std::memory_order_relaxed);
        auto& row = (*trace)[(size_t) (count & (traceRows - 1))];
        for (size_t s = 0; s < 2; ++s)
            for (size_t v = 0; v < maxVoices; ++v)
                row.delayMs[s][v].store (voiceGainNext[v] > 0.0f ? (float) ((delayNext[s][v] + wetLatency) * 1000.0 / sr) : 0.0f, std::memory_order_relaxed);
        for (size_t v = 0; v < maxVoices; ++v)
            row.voiceGain[v].store (voiceGainNext[v] * dip[v], std::memory_order_relaxed);
        row.tapePeak.store (tapeMost, std::memory_order_relaxed);
        row.centreMs.store (centre, std::memory_order_relaxed);
        tapeMost = 0.0f;
        traceCount.store (count + 1, std::memory_order_release);
    }

    // Equal power blend: the level holds as Mix moves, because a moving chorus voice is not in step with the dry signal.
    void setBlendGains (float& dryG, float& wetG) const
    {
        const double a = (double) (mix * on) * juce::MathConstants<double>::halfPi;
        dryG = (float) std::cos (a);
        wetG = (float) std::sin (a);
    }

    static float saturate (float x, float g, float den)
    {
        constexpr float bias = 0.05f; // tape's slight asymmetry: adds even harmonics
        return (std::tanh (g * (x + bias)) - std::tanh (g * bias)) / den;
    }
    // Clean: the slope at silence is exactly 1 at every drive, so quiet sounds keep their level and only the peaks are rounded off.
    // (The standard curve holds a -12 dBFS sine level instead, which lifts anything quieter: 4.4 dB at Saturation 100.)
    static float saturateClean (float x, float g)
    {
        constexpr float bias = 0.05f;
        const float centre = std::tanh (g * bias);
        return (std::tanh (g * (x + bias)) - centre) / (g * (1.0f - centre * centre));
    }
    // Keeps a -12 dBFS sine at about the same level at every drive.
    static float driveDenominator (float g) { return 4.0f * std::tanh (g * 0.25f); }

    void renderChunk (float* const* io, int numIn, int numOut, int offset, int n)
    {
        const int numLines = std::min (numIn, 2);
        std::array<float, tickLen> workL, workR, dryL, dryR;
        float* work[2] = { workL.data(), workR.data() };
        float* dry[2] = { dryL.data(), dryR.data() };

        for (int ch = 0; ch < numLines; ++ch)
        {
            const float* in = io[ch] + offset;
            auto& st = chan[(size_t) ch];
            for (int i = 0; i < n; ++i)
            {
                const float ig = inGain + (float) (tickPos + i) / (float) tickLen * (inGainNext - inGain);
                dry[ch][i] = juce::jlimit (-16.0f, 16.0f, in[i] * ig);
                const double x = (double) dry[ch][i];
                const double y = preB0 * x + st.pre;
                st.pre = preB1 * x - preA1 * y;
                work[ch][i] = (float) y;
            }
        }

        // Clean's own clip stage works on a copy, taken before the standard stage writes over the input.
        std::array<float, tickLen> cleanL, cleanR;
        float* cleanWork[2] = { cleanL.data(), cleanR.data() };
        if (cleanRunning)
            for (int ch = 0; ch < numLines; ++ch)
                std::copy_n (work[ch], n, cleanWork[ch]);

        // Soft clip, oversampled. The filters always run, so nothing changes in level or timing when drive starts.
        if (standardRunning)
        {
            juce::dsp::AudioBlock<float> block (work, (size_t) numLines, (size_t) n);
            auto up = os->processSamplesUp (block);
            const int upN = (int) up.getNumSamples();
            const float g0 = lerpTick (drive, driveNext, tickPos);
            const float g1 = lerpTick (drive, driveNext, tickPos + n);
            if (g0 > 1.0e-4f || g1 > 1.0e-4f)
            {
                const float gStep = (g1 - g0) / (float) upN;
                for (int ch = 0; ch < numLines; ++ch)
                {
                    auto* d = up.getChannelPointer ((size_t) ch);
                    float g = g0;
                    if (gStep == 0.0f)
                    {
                        // Drive is holding, so the parts of the curve that depend only on it are worked out once.
                        constexpr float bias = 0.05f;
                        const float centre = std::tanh (g * bias), den = driveDenominator (g);
                        for (int i = 0; i < upN; ++i)
                            d[i] = (std::tanh (g * (d[i] + bias)) - centre) / den;
                    }
                    else
                    {
                        for (int i = 0; i < upN; ++i, g += gStep)
                            if (g > 1.0e-4f)
                                d[i] = saturate (d[i], g, driveDenominator (g));
                    }
                }
            }
            os->processSamplesDown (block);
        }

        if (cleanRunning)
        {
            const int upN = n * cleanOs.getFactor();
            const float g0 = lerpTick (drive, driveNext, tickPos), g1 = lerpTick (drive, driveNext, tickPos + n);
            const float gStep = (g1 - g0) / (float) upN;
            const float a0 = lerpTick (cleanAmt, cleanAmtNext, tickPos), a1 = lerpTick (cleanAmt, cleanAmtNext, tickPos + n);
            for (int ch = 0; ch < numLines; ++ch)
            {
                float* d = cleanOs.up (ch, cleanWork[ch], n);
                if (g0 > 1.0e-4f || g1 > 1.0e-4f)
                {
                    float g = g0;
                    for (int i = 0; i < upN; ++i, g += gStep)
                        if (g > 1.0e-4f)
                            d[i] = saturateClean (d[i], g);
                }
                cleanOs.down (ch, cleanWork[ch], n);
                if (! standardRunning)
                    std::copy_n (cleanWork[ch], n, work[ch]);
                else
                    for (int i = 0; i < n; ++i)
                        work[ch][i] += (a0 + (a1 - a0) * (float) i / (float) n) * (cleanWork[ch][i] - work[ch][i]);
            }
        }

        const int numOutSides = numOut >= 2 ? 2 : 1;

        for (int i = 0; i < n; ++i)
        {
            const float tp = (float) (tickPos + i) / (float) tickLen;

            // Level and gate come from the signal after Input Gain, for the meter and for Noise Gate.
            {
                double most = std::abs ((double) dry[0][i]);
                if (numLines == 2) most = std::max (most, std::abs ((double) dry[1][i]));
                inPeak = std::max (most, inPeak * meterDecay);
                gateEnv += (most > gateEnv ? gateAttack : gateRelease) * (most - gateEnv);
            }

            // Tape hiss and feedback go onto the tape with the signal. The feedback left the voices about as long ago as
            // the oversampling filters delay the input, so one pass around the loop takes the Delay Time you set.
            const float fbNow = fb + tp * (fbNext - fb);
            // Holds the level steady as Feedback rises. Copies that do not line up only feed back their share of the sum, hence the energy share.
            const double shareNow = (double) (tapShare + tp * (tapShareNext - tapShare));
            // 0.92 because the loop rolls off the very top: this splits the difference between white noise and darker sounds like a voice.
            const double inputScale = std::sqrt (1.0 - 0.92 * (double) fbNow * fbNow * shareNow);
            const float hissNow = (hiss + tp * (hissNext - hiss)) * (gate + tp * (gateNext - gate));
            const float hissA = uniform (hissRngA), hissB = uniform (hissRngB);
            const float hissTo[2] = { hissNow * hissA, hissNow * (hissCos * hissA + hissSin * hissB) };
            const float back0 = feedbackRing[0][(size_t) ringPos], back1 = feedbackRing[1][(size_t) ringPos];
            const float backTo[2] = { numLines == 2 ? back0 : (numOutSides == 2 ? 0.5f * (back0 + back1) : back0), back1 };

            // Into the delay lines
            ++writeIndex;
            writeIndex &= mask;
            for (int ch = 0; ch < numLines; ++ch)
            {
                auto& st = chan[(size_t) ch];
                const double x = (double) work[ch][i];
                // Exact inverse of the pre-emphasis.
                const double deY = (x + preA1 * st.deIn - preB1 * st.deOut) / preB0;
                st.deIn = x; st.deOut = deY;
                const double dcStandard = deY - st.dcX + dcR * st.dcY;
                // Clean's DC filter sits at 1 Hz, not 6: at 6 Hz it turns the phase of a low note enough to bring it in early
                // (4.6 samples at 100 Hz), which read as 0.13 and 0.22 sample of delay error on the two lowest sung takes.
                const double dcClean = deY - st.dcX + dcRClean * st.dcYClean;
                st.dcX = deY; st.dcY = dcStandard; st.dcYClean = dcClean;
                const float cleanNow = cleanAmt + tp * (cleanAmtNext - cleanAmt);
                const double dc = cleanNow > 0.0f ? dcStandard + (double) cleanNow * (dcClean - dcStandard) : dcStandard;
                const float onTape = (float) (inputScale * (dc + (double) hissTo[ch]) + (double) (fbNow * backTo[ch]));
                line[(size_t) ch][(size_t) writeIndex] = onTape;
                tapeMost = std::max (tapeMost, std::abs (onTape));
            }

            std::array<float, 2> wet { 0.0f, 0.0f };
            for (int v = 0; v < maxVoices; ++v)
            {
                const auto vi = (size_t) v;
                if (voiceGain[vi] == 0.0f && voiceGainNext[vi] == 0.0f) continue;
                for (int s = 0; s < numOutSides; ++s)
                {
                    const auto si = (size_t) s;
                    const float gain = sideGain[si][vi] + tp * (sideGainNext[si][vi] - sideGain[si][vi]);
                    const double d = delayCur[si][vi] + delayStep[si][vi] * (tickPos + i);
                    const auto& l = line[(size_t) (numLines == 2 ? s : 0)];
                    wet[si] += gain * readInterpolated (l, d);
                }
            }

            const float tg = toneG + tp * (toneGNext - toneG);
            const float dg = dryGain + tp * (dryGainNext - dryGain);
            const float wg = wetGain + tp * (wetGainNext - wetGain);
            const float og = outGain + tp * (outGainNext - outGain);
            const float lowNow = lowAmt + tp * (lowAmtNext - lowAmt), highNow = highAmt + tp * (highAmtNext - highAmt);
            double outMost = 0.0;
            for (int s = 0; s < numOutSides; ++s)
            {
                auto& st = chan[(size_t) s];
                double x = wet[(size_t) s];

                // Low Cut and High Cut are inside the feedback loop, so each pass loses a little more.
                const double hp = st.low.high (x, lowCoef);
                x += (double) lowNow * (hp - x);
                const double lpOut = st.high.low (x, highCoef);
                x += (double) highNow * (lpOut - x);

                // The sum of the voices over the sum of their gains: even if every copy lines up, the loop's gain cannot pass 1.
                const float sumNow = tapSum[(size_t) s] + tp * (tapSumNext[(size_t) s] - tapSum[(size_t) s]);
                double tap = x / (double) std::max (sumNow, 0.05f);
                st.loopLp += loopLpA * (tap - st.loopLp);
                st.loopHp += loopHpA * (st.loopLp - st.loopHp);
                tap = softLimit (st.loopLp - st.loopHp);
                feedbackRing[(size_t) s][(size_t) ringPos] = (float) tap;

                const double vv = (x - st.toneZ) * toneA;
                const double lp = vv + st.toneZ;
                st.toneZ = lp + vv;
                const float w = (float) (lp + tg * (x - lp));
                const float dr = dry[numLines == 2 ? s : 0][i];
                const float mixed = juce::jlimit (-16.0f, 16.0f, (dg * dr + wg * w) * og);
                io[s][offset + i] = mixed;
                outMost = std::max (outMost, std::abs ((double) mixed));
            }
            outPeak = std::max (outMost, outPeak * meterDecay);
            if (++ringPos >= ringLen) ringPos = 0;
        }
        tickPos += n;
    }

    static float lerpTick (float a, float b, int pos) { return a + (b - a) * (float) pos / (float) tickLen; }

    static float uniform (uint32_t& st)
    {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        return (float) ((double) st * (2.0 / 4294967295.0) - 1.0);
    }

    // Straight up to 0.9, then bends over and never passes 1.5. Only loud feedback reaches the bend.
    static double softLimit (double x)
    {
        const double a = std::abs (x);
        if (a <= 0.9) return x;
        const double y = 0.9 + 0.6 * std::tanh ((a - 0.9) / 0.6);
        return x < 0.0 ? -y : y;
    }

    // Filters here are the only state with feedback of their own. Input is clean and the loop is limited, so this is a last guard.
    void clearIfBad()
    {
        for (auto& st : chan)
            if (! st.low.isFinite() || ! st.high.isFinite() || ! std::isfinite (st.loopLp) || ! std::isfinite (st.loopHp))
            {
                st.low = {}; st.high = {}; st.loopLp = st.loopHp = 0.0;
                for (auto& side : feedbackRing) side.fill (0.0f);
            }
    }

    // 6 point Lagrange: keeps the top end steady as the read head moves between samples. delay must be at least 3 samples.
    float readInterpolated (const std::vector<float>& l, double delaySamples) const
    {
        const double pos = (double) writeIndex - delaySamples;
        const double fl = std::floor (pos);
        const float x = (float) (pos - fl);
        const int i = (int) fl;
        const auto at = [&] (int k) { return l[(size_t) ((i + k) & mask)]; };
        const float a = x + 2.0f, b = x + 1.0f, c = x, d = x - 1.0f, e = x - 2.0f, f = x - 3.0f;
        return -b * c * d * e * f / 120.0f * at (-2)
             +  a * c * d * e * f /  24.0f * at (-1)
             -  a * b * d * e * f /  12.0f * at (0)
             +  a * b * c * e * f /  12.0f * at (1)
             -  a * b * c * d * f /  24.0f * at (2)
             +  a * b * c * d * e / 120.0f * at (3);
    }

    // State variable filter: keeps steady while its frequency moves.
    struct CutCoefs
    {
        double a1 = 1, a2 = 0, a3 = 0;
        void set (double hz, double fs)
        {
            const double g = std::tan (juce::MathConstants<double>::pi * std::min (hz, 0.47 * fs) / fs);
            a1 = 1.0 / (1.0 + g * (g + juce::MathConstants<double>::sqrt2));
            a2 = g * a1;
            a3 = g * a2;
        }
    };
    struct Cut
    {
        double ic1 = 0, ic2 = 0;
        double low (double x, const CutCoefs& c) { double v1, v2; run (x, c, v1, v2); return v2; }
        double high (double x, const CutCoefs& c) { double v1, v2; run (x, c, v1, v2); return x - juce::MathConstants<double>::sqrt2 * v1 - v2; }
        bool isFinite() const { return std::isfinite (ic1) && std::isfinite (ic2); }
    private:
        void run (double x, const CutCoefs& c, double& v1, double& v2)
        {
            const double v3 = x - ic2;
            v1 = c.a1 * ic1 + c.a2 * v3;
            v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
        }
    };

    struct ChannelState
    {
        double pre = 0, deIn = 0, deOut = 0, dcX = 0, dcY = 0, dcYClean = 0, toneZ = 0, loopLp = 0, loopHp = 0;
        Cut low, high;
    };

    static constexpr double gateOpenLevel = 0.00178, gateCloseLevel = 0.001; // -55 and -60 dBFS, so it does not chatter on a held quiet note

    double sr = 48000.0, tickSec = tickLen / 48000.0;
    int mask = 0, writeIndex = 0, tickLeft = 0, tickPos = 0;
    double wetLatency = 0.0, minDelayMs = 1.0;
    std::unique_ptr<FastOversampler> os;
    CleanOversampler cleanOs;
    float cleanAmt = 0.0f, cleanAmtNext = 0.0f;
    bool cleanRunning = false, standardRunning = true;
    int cleanWarm = 0;
    double wowSmoothC = 0.0, flutSmoothC = 0.0, wowSmoothA = 0.0, wowSmoothB = 0.0, flutSmoothA = 0.0, flutSmoothB = 0.0;
    std::array<std::vector<float>, 2> line;
    std::array<ChannelState, 2> chan;
    double preB0 = 1, preB1 = 0, preA1 = 0, dcR = 0.999, dcRClean = 0.9999, toneA = 0.1;
    double cSlow = 0.1, cMid = 0.1, cFast = 0.1, cVoice = 0.1, cVoiceSoft = 0.1, cVoiceFade = 0.1;
    float gainSlew = 0.1f, mixSlew = 0.1f;
    double wowNoiseC = 0, flutNoiseLoC = 0, flutNoiseHiC = 0;
    double lfo = 0, wowA = 0, wowB = 0, flutA = 0, flutB = 0, wowNoise = 0, flutLo = 0, flutHi = 0;
    uint32_t rngState = 0x1234567u, slowRng = 0x2bcd1357u, hissRngA = 0x3e7a9c11u, hissRngB = 0x51f2d4b9u;
    double wowRatio = 1.0, flutterRatio = 1.0, scaleNow = 1.0;
    float wowHz = defaultWowHz, flutterHz = defaultFlutterHz, randomHz = 1.0f, degradeHz = 2.0f;
    float random = 0.0f, degradeAmt = 0.0f, unsyncAmt = 0.0f, reach = 0.0f;
    static constexpr float unsyncSlew = 0.01f;
    SlowNoiseShape randomShape, degradeShape;
    std::array<std::array<SlowNoise, maxVoices>, 2> randomNoise {};
    std::array<std::array<double, maxVoices>, 2> randomVal {};
    std::array<SlowNoise, maxVoices> degradeNoise {};
    std::array<float, maxVoices> dip {};
    std::array<std::array<float, 256>, 2> feedbackRing {};
    int ringPos = 0, ringLen = 1;
    double loopLpA = 0.5, loopHpA = 0.01, meterDecay = 0.999, gateAttack = 0.01, gateRelease = 0.001, cGateUp = 0.1, cGateDown = 0.01;
    double gateEnv = 0.0, inPeak = 0.0, outPeak = 0.0;
    bool gateOpen = false;
    float inRaw = 1, outRaw = 1, inGain = 1, inGainNext = 1, outGain = 1, outGainNext = 1, fb = 0, fbNext = 0;
    float hiss = 0, hissNext = 0, gate = 1, gateNext = 1, hissCos = 1, hissSin = 0;
    float lowLog = 3, highLog = 10, lowAmt = 0, lowAmtNext = 0, highAmt = 0, highAmtNext = 0;
    CutCoefs lowCoef, highCoef;
    std::array<float, 2> tapSum {}, tapSumNext {};
    float tapShare = 1.0f, tapShareNext = 1.0f;
    std::atomic<float> readInPeak { 0.0f }, readOutPeak { 0.0f }, readPitchSwing { 0.0f };
    std::array<std::array<std::atomic<float>, maxVoices>, 2> readDelayMs {};
    std::unique_ptr<std::array<TraceRow, traceRows>> trace = std::make_unique<std::array<TraceRow, traceRows>>();
    std::atomic<uint32_t> traceCount { 0 };
    std::atomic<float> traceHz { 250.0f };
    int traceTicks = 6, traceTicksLeft = 6;
    float tapeMost = 0.0f;

    Params target;
    float lfoSideTurn = 0.25f, rate = 1, depth = 0, centre = 15, wow = 0, flutter = 0, spread = 0, drive = 0, driveNext = 0, toneG = 1, toneGNext = 1, mix = 0, on = 1, dryGain = 1, dryGainNext = 1, wetGain = 0, wetGainNext = 0;
    std::array<float, maxVoices> voiceOffset {}, voicePhase {}, voicePan {}, voiceGain {}, voiceGainNext {}, offsetAim {}, phaseAim {}, gainAim {};
    std::array<std::array<float, maxVoices>, 2> sideGain {}, sideGainNext {};
    std::array<std::array<double, maxVoices>, 2> delayCur {}, delayNext {}, delayStep {};
};
