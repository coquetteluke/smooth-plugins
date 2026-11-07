#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

// The whole signal path, free of plug-in framework code.
//
// Wet path, per input channel:
//   pre-emphasis > 4x (2x at high rates) oversampled soft clip > de-emphasis > DC block > tape delay line
// Then up to 4 voices read each delay line through a modulated, interpolated read head.
// Each voice has a left and a right read head. Spread moves them apart in time and sweep, and places the voices
// across the stereo field. The voices are summed,
// Tone shapes the top end of the sum, and Mix blends it with the untouched dry signal.
//
// The oversampling filters' delay is taken off the read head's delay, so the delay you set is the delay you get,
// and the plug-in itself has zero latency. The dry path therefore needs no delay at all.
class TapeChorusEngine
{
public:
    static constexpr int maxVoices = 4;
    static constexpr float maxWowMs = 2.5f;      // peak delay swing of Wow at 100 percent
    static constexpr float maxFlutterMs = 0.25f; // peak delay swing of Flutter at 100 percent
    static constexpr float maxSpreadOffsetMs = 1.5f;  // each side moves this far from centre at Spread 100
    static constexpr float maxSatDrive = 6.0f;

    struct Params
    {
        int voices = 2;
        float rateHz = 0.8f, depthMs = 1.5f, delayMs = 15.0f, wowPct = 15.0f, flutterPct = 10.0f;
        float satPct = 20.0f, toneDb = -2.0f, spreadPct = 70.0f, mixPct = 35.0f;
        bool bypass = false;
    };

    void prepare (double sampleRate, const Params& p)
    {
        sr = sampleRate;
        // At 96 kHz, 2x already puts the clip stage at 192 kHz.
        const size_t stages = sr < 60000.0 ? 2u : 1u;
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, stages, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        os->initProcessing ((size_t) tickLen);
        os->reset();
        wetLatency = (double) os->getLatencyInSamples();
        minDelayMs = std::max (1.0, (wetLatency + 4.0) * 1000.0 / sr);

        const auto maxMs = 40.0 + 8.0 + maxWowMs + maxFlutterMs + 4.5 + maxSpreadOffsetMs + 2.0;
        const int needed = (int) std::ceil (maxMs * 0.001 * sr + wetLatency) + 8;
        int size = 1024;
        while (size < needed) size *= 2;
        mask = size - 1;
        for (auto& l : line) l.assign ((size_t) size, 0.0f);
        writeIndex = 0;

        tickSec = tickLen / sr;
        // Smoothers run once per tick.
        cSlow = coef (0.12); cMid = coef (0.06); cFast = coef (0.03); cVoice = coef (0.25);
        gainSlew = (float) (tickSec / 0.03);   // a voice fades in or out in 30 ms
        mixSlew = (float) (tickSec / 0.02);

        // Fixed filters
        {
            const double K = std::tan (juce::MathConstants<double>::pi * 3000.0 / sr), V = 2.0;
            preB0 = (V + K) / (1.0 + K); preB1 = (K - V) / (1.0 + K); preA1 = (K - 1.0) / (1.0 + K);
        }
        dcR = std::exp (-juce::MathConstants<double>::twoPi * 6.0 / sr);
        { const double K = std::tan (juce::MathConstants<double>::pi * 2500.0 / sr); toneA = K / (1.0 + K); }
        wowNoiseC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 0.9 * tickSec);
        flutNoiseLoC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 16.0 * tickSec);
        flutNoiseHiC = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 5.0 * tickSec);

        for (auto& s : chan) s = {};
        lfo = wowA = wowB = flutA = flutB = 0.0;
        wowNoise = flutLo = flutHi = 0.0;
        rngState = 0x1234567u;

        depth = p.depthMs; rate = p.rateHz; centre = p.delayMs; wow = p.wowPct * 0.01f * maxWowMs; flutter = p.flutterPct * 0.01f * maxFlutterMs;
        spread = p.spreadPct * 0.01f; drive = p.satPct * 0.01f * maxSatDrive; toneG = juce::Decibels::decibelsToGain (p.toneDb);
        mix = p.mixPct * 0.01f;
        on = p.bypass ? 0.0f : 1.0f;
        setBlendGains (dryGainNext, wetGainNext);
        dryGain = dryGainNext; wetGain = wetGainNext;
        const int n = juce::jlimit (1, maxVoices, p.voices);
        lfoSideTurn = 0.5f / (float) n;
        for (int v = 0; v < maxVoices; ++v)
        {
            voiceOffset[(size_t) v] = offsetFor (n, v);
            voicePhase[(size_t) v] = phaseFor (n, v);
            voicePan[(size_t) v] = panFor (n, v);
            voiceGain[(size_t) v] = voiceGainNext[(size_t) v] = v < n ? 1.0f / std::sqrt ((float) n) : 0.0f;
        }
        computeSideGains();
        sideGain = sideGainNext;
        toneGNext = toneG; driveNext = drive;
        target = p;
        computeDelays (delayNext);
        delayCur = delayNext;
        for (auto& side : delayStep) side.fill (0.0);
        tickLeft = 0;
    }

    double getWetLatencySamples() const { return wetLatency; }
    double getMinDelayMs() const { return minDelayMs; }

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
    }

private:
    static constexpr int tickLen = 32;

    double coef (double seconds) const { return 1.0 - std::exp (-tickSec / seconds); }

    // Copies sit evenly around the centre delay, so the average delay is always the one you set.
    static float offsetFor (int n, int v)
    {
        static constexpr float table[maxVoices][maxVoices] = {
            { 0.0f, 0.0f, 0.0f, 0.0f },
            { -2.0f, 2.0f, 0.0f, 0.0f },
            { -3.0f, 0.0f, 3.0f, 0.0f },
            { -4.5f, -1.5f, 1.5f, 4.5f } };
        return table[n - 1][v];
    }
    static float phaseFor (int n, int v) { return (float) v / (float) n; }
    // Where each copy sits at full Spread, -1 left to +1 right. Always balanced around the centre.
    static float panFor (int n, int v)
    {
        static constexpr float table[maxVoices][maxVoices] = {
            { 0.0f, 0.0f, 0.0f, 0.0f },
            { -0.7f, 0.7f, 0.0f, 0.0f },
            { -0.7f, 0.7f, 0.0f, 0.0f },
            { -0.7f, 0.7f, -0.7f, 0.7f } };
        return table[n - 1][v];
    }

    // Equal power, and exactly 1 on both sides at the centre, so Spread 0 leaves left and right identical.
    void computeSideGains()
    {
        for (size_t v = 0; v < maxVoices; ++v)
        {
            const double th = (double) voicePan[v] * spread * juce::MathConstants<double>::pi * 0.25;
            const double c = std::cos (th), sn = std::sin (th);
            sideGainNext[0][v] = voiceGainNext[v] * (float) (c - sn);
            sideGainNext[1][v] = voiceGainNext[v] * (float) (c + sn);
        }
    }

    static double sinTurns (double t) { return std::sin (juce::MathConstants<double>::twoPi * t); }

    uint32_t nextRandom()
    {
        rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
        return rngState;
    }

    void computeDelays (std::array<std::array<double, maxVoices>, 2>& dest) const
    {
        double maxOff = 0.0;
        for (auto o : voiceOffset) maxOff = std::max (maxOff, (double) std::abs (o));
        const double excursion = maxOff + spread * maxSpreadOffsetMs + depth + wow + flutter;
        const double room = centre - minDelayMs;
        // Everything shrinks together when the delay is too short to hold it, so the read head never reaches the write head.
        const double scale = excursion > room ? std::max (room, 0.0) / excursion : 1.0;

        for (int side = 0; side < 2; ++side)
        {
            const double sideTurn = side * spread;
            const double sideOffset = (side == 0 ? -1.0 : 1.0) * spread * maxSpreadOffsetMs;
            for (int v = 0; v < maxVoices; ++v)
            {
                const double w = 0.62 * sinTurns (wowA + 0.37 * v + 0.25 * sideTurn)
                               + 0.26 * sinTurns (wowB + 0.61 * v + 0.40 * sideTurn)
                               + 0.12 * wowNoise;
                const double f = 0.55 * sinTurns (flutA + 0.23 * v + 0.25 * sideTurn)
                               + 0.28 * sinTurns (flutB + 0.71 * v + 0.40 * sideTurn)
                               + 0.17 * (flutLo - flutHi);
                const double l = sinTurns (lfo + voicePhase[(size_t) v] + (double) lfoSideTurn * sideTurn);
                const double ms = centre + scale * (voiceOffset[(size_t) v] + sideOffset + depth * l + wow * w + flutter * f);
                dest[(size_t) side][(size_t) v] = ms * 0.001 * sr - wetLatency;
            }
        }
    }

    void startTick()
    {
        tickLeft = tickLen;

        const auto smooth = [] (float& v, float t, double c)
        {
            v += (float) c * (t - v);
            if (std::abs (t - v) < 1.0e-5f) v = t;
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

        // Moving phases
        const auto advance = [this] (double& ph, double hz) { ph += hz * tickSec; if (ph >= 1.0) ph -= 1.0; };
        advance (lfo, rate);
        advance (wowA, 0.53); advance (wowB, 0.87);
        advance (flutA, 7.1); advance (flutB, 11.3);

        // Slow and fast irregular motion, so the transport never sounds like a pure sine wave.
        const auto rnd = [this] { return (double) nextRandom() * (2.0 / 4294967295.0) - 1.0; };
        wowNoise += wowNoiseC * (rnd() * 6.0 - wowNoise);
        wowNoise = juce::jlimit (-1.0, 1.0, wowNoise);
        const double r = rnd() * 4.0;
        flutLo += flutNoiseLoC * (r - flutLo);
        flutHi += flutNoiseHiC * (flutLo - flutHi);
        flutLo = juce::jlimit (-1.0, 1.0, flutLo);
        flutHi = juce::jlimit (-1.0, 1.0, flutHi);

        // Voices
        const int n = juce::jlimit (1, maxVoices, target.voices);
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
                voiceOffset[i] = offT;
                voicePhase[i] = phT;
                voicePan[i] = panT;
            }
            else
            {
                smooth (voiceOffset[i], offT, cVoice);
                smooth (voicePhase[i], phT, cVoice);
                smooth (voicePan[i], panT, cVoice);
            }
            voiceGain[i] = voiceGainNext[i];
            slew (voiceGainNext[i], gainT, gainSlew);
        }

        sideGain = sideGainNext;
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
        tickPos = 0;
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
                dry[ch][i] = in[i];
                const double x = (double) in[i];
                const double y = preB0 * x + st.pre;
                st.pre = preB1 * x - preA1 * y;
                work[ch][i] = (float) y;
            }
        }

        // Soft clip, oversampled. The filters always run, so nothing changes in level or timing when drive starts.
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
                    for (int i = 0; i < upN; ++i, g += gStep)
                        if (g > 1.0e-4f)
                            d[i] = saturate (d[i], g, driveDenominator (g));
                }
            }
            os->processSamplesDown (block);
        }

        const int numOutSides = numOut >= 2 ? 2 : 1;

        for (int i = 0; i < n; ++i)
        {
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
                const double dc = deY - st.dcX + dcR * st.dcY;
                st.dcX = deY; st.dcY = dc;
                line[(size_t) ch][(size_t) writeIndex] = (float) dc;
            }

            const float tp = (float) (tickPos + i) / (float) tickLen;
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
            for (int s = 0; s < numOutSides; ++s)
            {
                auto& st = chan[(size_t) s];
                const double x = wet[(size_t) s];
                const double vv = (x - st.toneZ) * toneA;
                const double lp = vv + st.toneZ;
                st.toneZ = lp + vv;
                const float w = (float) (lp + tg * (x - lp));
                const float dr = dry[numLines == 2 ? s : 0][i];
                io[s][offset + i] = dg * dr + wg * w;
            }
        }
        tickPos += n;
    }

    static float lerpTick (float a, float b, int pos) { return a + (b - a) * (float) pos / (float) tickLen; }

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

    struct ChannelState
    {
        double pre = 0, deIn = 0, deOut = 0, dcX = 0, dcY = 0, toneZ = 0;
    };

    double sr = 48000.0, tickSec = tickLen / 48000.0;
    int mask = 0, writeIndex = 0, tickLeft = 0, tickPos = 0;
    double wetLatency = 0.0, minDelayMs = 1.0;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    std::array<std::vector<float>, 2> line;
    std::array<ChannelState, 2> chan;
    double preB0 = 1, preB1 = 0, preA1 = 0, dcR = 0.999, toneA = 0.1;
    double cSlow = 0.1, cMid = 0.1, cFast = 0.1, cVoice = 0.1;
    float gainSlew = 0.1f, mixSlew = 0.1f;
    double wowNoiseC = 0, flutNoiseLoC = 0, flutNoiseHiC = 0;
    double lfo = 0, wowA = 0, wowB = 0, flutA = 0, flutB = 0, wowNoise = 0, flutLo = 0, flutHi = 0;
    uint32_t rngState = 0x1234567u;

    Params target;
    float lfoSideTurn = 0.25f, rate = 1, depth = 0, centre = 15, wow = 0, flutter = 0, spread = 0, drive = 0, driveNext = 0, toneG = 1, toneGNext = 1, mix = 0, on = 1, dryGain = 1, dryGainNext = 1, wetGain = 0, wetGainNext = 0;
    std::array<float, maxVoices> voiceOffset {}, voicePhase {}, voicePan {}, voiceGain {}, voiceGainNext {};
    std::array<std::array<float, maxVoices>, 2> sideGain {}, sideGainNext {};
    std::array<std::array<double, maxVoices>, 2> delayCur {}, delayNext {}, delayStep {};
};
