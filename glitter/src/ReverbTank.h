#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// One reverb space: stereo early reflections plus a 16 line feedback delay network.
//
// Each line runs: delay (moved by Size, plus a very slow wobble) -> short fixed all-pass -> high shelf and loss -> mixing matrix.
// The matrix is lossless, so all the decay comes from the per line loss, which is worked out from each line's own length.
// That is what makes the decay time follow Decay at every Size and Type.
// No memory is allocated after prepare().
namespace smooth
{
    constexpr int kNumLines = 16;
    constexpr int kNumDiffusers = 4;
    constexpr int kNumTaps = 8;
    constexpr int kNumTypes = 3;
    constexpr float kMaxScale = 2.0f;   // Size 100%
    constexpr float kMinScale = 0.5f;   // Size 0%
    constexpr float kRefHz = 700.0f;    // frequency where the decay time equals Decay exactly
    constexpr float kMaxWobble = 4.0f;  // Mod Depth 100% against the voicing's own depth, which is Mod Depth 25%

    struct Voice
    {
        float lineMs[kNumLines];            // lengths at Size 50%. Ascending, and each index no longer than the Hall one
        float loopAllpassMs[kNumLines];     // fixed length, not moved by Size
        float loopAllpassCoeff;
        float diffuserMs[2][kNumDiffusers]; // left, right. Fixed length
        float diffuserCoeff;
        float tapMs[2][kNumTaps];           // early reflections at Size 50%. Tap 0 stays at 0 ms so the wet onset equals Pre-Delay.
                                            // All end by 90 ms at Size 100%, so none stands alone once the tail is dense
        float tapGain[kNumTaps];
        float shelfHz;                      // where Damping starts to bite
        float wobbleMs;                     // depth of the slow line wobble at Mod Depth 25%
        float earlyLevel, tankLevel;
    };

    inline const Voice& voiceFor (int type) noexcept
    {
        static const Voice voices[kNumTypes] =
        {
            // Plate: dense from the first moment, weak early reflections, bright
            { { 13.1f, 14.7f, 16.3f, 18.1f, 19.7f, 21.7f, 23.9f, 26.3f, 28.9f, 31.3f, 34.1f, 37.1f, 40.7f, 44.3f, 48.1f, 52.7f },
              { 1.13f, 2.39f, 1.61f, 3.07f, 1.97f, 3.53f, 1.37f, 2.71f, 4.13f, 1.79f, 3.31f, 2.17f, 4.57f, 1.49f, 2.93f, 3.79f }, 0.55f,
              { { 3.7f, 5.3f, 8.9f, 12.1f }, { 4.1f, 5.9f, 9.7f, 13.3f } }, 0.70f,
              { { 0.0f, 2.9f, 5.3f, 8.3f, 11.7f, 15.1f, 19.3f, 23.9f }, { 0.0f, 3.3f, 6.1f, 9.1f, 12.7f, 16.3f, 20.9f, 25.3f } },
              { 0.40f, 0.80f, 0.42f, 0.32f, 0.24f, 0.18f, 0.13f, 0.09f },
              4000.0f, 0.15f, 0.69f, 1.26f },
            // Room: short lines, strong and close early reflections, darker
            { { 9.7f, 10.9f, 12.1f, 13.3f, 14.9f, 16.1f, 17.9f, 19.3f, 21.1f, 22.7f, 24.7f, 26.9f, 29.3f, 31.7f, 34.3f, 37.3f },
              { 0.73f, 1.37f, 0.97f, 1.79f, 1.13f, 2.11f, 0.83f, 1.61f, 2.39f, 1.03f, 1.97f, 1.27f, 2.63f, 0.89f, 1.73f, 2.23f }, 0.50f,
              { { 1.9f, 3.1f, 4.7f, 6.1f }, { 2.3f, 3.4f, 5.1f, 6.7f } }, 0.60f,
              { { 0.0f, 4.3f, 7.9f, 11.3f, 15.7f, 19.1f, 24.7f, 29.3f }, { 0.0f, 5.1f, 8.9f, 12.7f, 16.9f, 21.1f, 26.3f, 31.7f } },
              { 0.40f, 1.00f, 0.72f, 0.56f, 0.45f, 0.35f, 0.27f, 0.20f },
              3000.0f, 0.10f, 0.57f, 0.71f },
            // Hall: long lines, wide spaced reflections, a slower build
            { { 29.3f, 32.1f, 35.3f, 38.9f, 41.9f, 46.1f, 50.3f, 54.7f, 59.3f, 64.7f, 70.1f, 76.3f, 83.1f, 89.9f, 97.1f, 104.9f },
              { 2.11f, 4.33f, 3.07f, 5.59f, 3.67f, 6.29f, 2.57f, 4.91f, 7.13f, 3.31f, 5.87f, 4.01f, 7.73f, 2.83f, 5.23f, 6.71f }, 0.55f,
              { { 3.1f, 5.3f, 8.3f, 12.1f }, { 3.5f, 5.9f, 9.1f, 13.3f } }, 0.60f,
              { { 0.0f, 9.1f, 14.3f, 19.7f, 25.3f, 31.1f, 37.3f, 44.1f }, { 0.0f, 10.3f, 15.7f, 21.1f, 27.1f, 33.1f, 38.9f, 43.3f } },
              { 0.32f, 0.90f, 0.62f, 0.50f, 0.40f, 0.32f, 0.25f, 0.18f },
              3500.0f, 0.35f, 0.70f, 1.08f },
        };
        return voices[std::clamp (type, 0, kNumTypes - 1)];
    }

    class ReverbTank
    {
    public:
        static constexpr int kChunk = 16; // control rate. process() is never handed more than this

        void prepare (double sampleRate)
        {
            fs = (float) sampleRate;
            const auto& hall = voiceFor (2);
            const auto ceilingSamples = [this] (float ms) { return nextPow2 ((int) std::ceil (ms * 0.001f * fs) + 16); };

            size_t total = 0;
            int lineLen[kNumLines], loopLen[kNumLines], diffLen[2][kNumDiffusers], tapLen = 0;
            for (int k = 0; k < kNumLines; ++k)
            {
                lineLen[k] = ceilingSamples (hall.lineMs[k] * kMaxScale + 2.0f * hall.wobbleMs * kMaxWobble + 1.0f);
                float apMax = 0.0f;
                for (int t = 0; t < kNumTypes; ++t) apMax = std::max (apMax, voiceFor (t).loopAllpassMs[k]);
                loopLen[k] = ceilingSamples (apMax);
                total += (size_t) lineLen[k] + (size_t) loopLen[k];
            }
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < kNumDiffusers; ++k)
                {
                    float dMax = 0.0f;
                    for (int t = 0; t < kNumTypes; ++t) dMax = std::max (dMax, voiceFor (t).diffuserMs[c][k]);
                    diffLen[c][k] = ceilingSamples (dMax);
                    total += (size_t) diffLen[c][k];
                }
            float tapMax = 0.0f;
            for (int t = 0; t < kNumTypes; ++t)
                for (int c = 0; c < 2; ++c)
                    tapMax = std::max (tapMax, voiceFor (t).tapMs[c][kNumTaps - 1]);
            tapLen = ceilingSamples (tapMax * kMaxScale + 1.0f);
            total += 2u * (size_t) tapLen;

            memory.assign (total, 0.0f);
            auto* p = memory.data();
            const auto take = [&p] (Delay& d, int len) { d.buf = p; d.mask = (uint32_t) len - 1u; p += len; };
            for (int k = 0; k < kNumLines; ++k) { take (line[k], lineLen[k]); take (loopAp[k], loopLen[k]); }
            for (int c = 0; c < 2; ++c)
            {
                for (int k = 0; k < kNumDiffusers; ++k) take (diff[c][k], diffLen[c][k]);
                take (tap[c], tapLen);
            }

            lastScale = lastDecay = lastRatio = -1.0f;
            configure (type, 1.0f, 2.0f, 1.0f);
        }

        // Starts this tank on a Type: all state cleared, every length snapped. Only called while this tank is silent.
        void configure (int newType, float scale, float decaySeconds, float hfRatio) noexcept
        {
            type = std::clamp (newType, 0, kNumTypes - 1);
            clear();
            const auto& v = voiceFor (type);
            for (int k = 0; k < kNumLines; ++k)
            {
                loopApLen[k] = std::max (1, (int) std::lround (v.loopAllpassMs[k] * 0.001f * fs));
                lineDelay[k] = v.lineMs[k] * 0.001f * fs * scale;
                lineInc[k] = 0.0f;
                wobblePhase[k] = 0.37f * (float) k;
                wobbleInc[k] = 6.2831853f * (0.11f + 0.043f * (float) k) * (float) kChunk / fs;
            }
            for (int c = 0; c < 2; ++c)
            {
                for (int k = 0; k < kNumDiffusers; ++k)
                    diffLenS[c][k] = std::max (1, (int) std::lround (v.diffuserMs[c][k] * 0.001f * fs));
                for (int t = 0; t < kNumTaps; ++t)
                {
                    tapDelay[c][t] = v.tapMs[c][t] * 0.001f * fs * scale;
                    tapInc[c][t] = 0.0f;
                }
            }
            lastScale = lastDecay = lastRatio = -1.0f;
            controlUpdate (scale, decaySeconds, hfRatio);
            for (int k = 0; k < kNumLines; ++k) { lineDelay[k] = target[k]; lineInc[k] = 0.0f; }
        }

        void clear() noexcept
        {
            if (! memory.empty()) std::memset (memory.data(), 0, memory.size() * sizeof (float));
            for (auto& s : shelf) s = {};
            writePos = 0u;
            inGain = outGain = 1.0f;
            inTarget = outTarget = 1.0f;
            quietSamples = 0;
            sleeping = false;
        }

        // Once per chunk. scale = Size as a length factor, decaySeconds = Decay, hfRatio = high decay time / mid decay time.
        // wobbleRate and wobbleDepth are 1 at the voicing's own modulation.
        void controlUpdate (float scale, float decaySeconds, float hfRatio, float wobbleRate = 1.0f, float wobbleDepth = 1.0f) noexcept
        {
            const auto& v = voiceFor (type);
            const float toSamples = 0.001f * fs;
            const float perSample = 1.0f / (float) kChunk;

            for (int k = 0; k < kNumLines; ++k)
            {
                wobblePhase[k] += wobbleInc[k] * wobbleRate;
                if (wobblePhase[k] > 6.2831853f) wobblePhase[k] -= 6.2831853f;
                const float wobble = (1.0f + std::sin (wobblePhase[k])) * v.wobbleMs * wobbleDepth * toSamples;
                target[k] = v.lineMs[k] * toSamples * scale + wobble;
                lineInc[k] = (target[k] - lineDelay[k]) * perSample;
            }
            if (scale != lastScale)
                for (int c = 0; c < 2; ++c)
                    for (int t = 0; t < kNumTaps; ++t)
                        tapInc[c][t] = (v.tapMs[c][t] * toSamples * scale - tapDelay[c][t]) * perSample;
            else
                for (int c = 0; c < 2; ++c)
                    for (int t = 0; t < kNumTaps; ++t)
                        tapInc[c][t] = 0.0f;

            if (scale != lastScale || decaySeconds != lastDecay || hfRatio != lastRatio)
            {
                lastScale = scale; lastDecay = decaySeconds; lastRatio = hfRatio;
                updateLoss (v, scale, decaySeconds, hfRatio);
            }
        }

        // Adds this tank's output to outL and outR.
        void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
        {
            const auto& v = voiceFor (type);
            const float inStep = 1.0f / (0.02f * fs), outStep = 1.0f / (0.03f * fs);
            const float apG = v.loopAllpassCoeff, dG = v.diffuserCoeff;
            const float early = v.earlyLevel, tank = v.tankLevel;
            float peak = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                inGain = approach (inGain, inTarget, inStep);
                outGain = approach (outGain, outTarget, outStep);
                const float xL = inL[i] * inGain, xR = inR[i] * inGain;

                // Early reflections. Tap 0 is the sample just written, so this path has no delay of its own.
                float eL = 0.0f, eR = 0.0f;
                tap[0].buf[writePos & tap[0].mask] = xL;
                tap[1].buf[writePos & tap[1].mask] = xR;
                for (int t = 0; t < kNumTaps; ++t)
                {
                    eL += v.tapGain[t] * readLinear (tap[0], tapDelay[0][t]);
                    eR += v.tapGain[t] * readLinear (tap[1], tapDelay[1][t]);
                    tapDelay[0][t] += tapInc[0][t];
                    tapDelay[1][t] += tapInc[1][t];
                }

                // Input diffusion
                float dL = xL, dR = xR;
                for (int k = 0; k < kNumDiffusers; ++k)
                {
                    dL = allpass (diff[0][k], diffLenS[0][k], dL, dG);
                    dR = allpass (diff[1][k], diffLenS[1][k], dR, dG);
                }

                // The lines
                float m[kNumLines];
                for (int k = 0; k < kNumLines; ++k)
                {
                    float y = readCubic (line[k], lineDelay[k]);
                    lineDelay[k] += lineInc[k];
                    y = allpass (loopAp[k], loopApLen[k], y, apG);
                    auto& s = shelf[k];
                    const float o = s.b0 * y + s.z1;
                    s.z1 = s.b1 * y - s.a1 * o + s.z2;
                    s.z2 = s.b2 * y - s.a2 * o;
                    m[k] = o;
                }

                // Lossless mixing (fast 16 point Hadamard)
                for (int h = 8; h >= 1; h >>= 1)
                    for (int a = 0; a < kNumLines; a += 2 * h)
                        for (int b = a; b < a + h; ++b)
                        {
                            const float p = m[b], q = m[b + h];
                            m[b] = p + q;
                            m[b + h] = p - q;
                        }

                constexpr float kScale = 0.25f;           // makes the matrix energy preserving
                constexpr float kFeed = 0.35355339f;      // one input over 8 lines
                // Each output hears all 16 lines at equal strength with its own sign pattern. The two patterns are orthogonal,
                // which decorrelates left from right, and each is chosen so no single line's delay stands out as a repeat.
                const float wetL = (m[1] + m[3] + m[5] - m[7]) * 0.125f;
                const float wetR = (m[8] + m[10] + m[12] - m[14]) * 0.125f;
                for (int k = 0; k < kNumLines; ++k)
                {
                    const float feed = ((k & 1) == 0 ? dL : dR) * (((k >> 1) & 1) == 0 ? kFeed : -kFeed);
                    line[k].buf[writePos & line[k].mask] = m[k] * kScale + feed;
                }
                ++writePos;

                const float l = (early * eL + tank * wetL) * outGain;
                const float r = (early * eR + tank * wetR) * outGain;
                outL[i] += l;
                outR[i] += r;
                peak = std::max (peak, std::max (std::abs (l), std::abs (r)));
            }

            if (peak < 1.0e-6f) quietSamples = std::min (quietSamples + n, 1 << 30); else quietSamples = 0;
            // Once a tank no longer gets input and has rung out it stops using any processor time
            if (inGain <= 0.0f && inTarget <= 0.0f && quietSamples > (int) (0.3f * fs))
                sleeping = true;
        }

        void startInput() noexcept  { inTarget = 1.0f; }
        void stopInput() noexcept   { inTarget = 0.0f; }
        void fadeOutput() noexcept  { outTarget = 0.0f; }
        void snapInput (float g) noexcept { inGain = inTarget = g; }
        void sleep() noexcept { sleeping = true; }
        bool isSleeping() const noexcept { return sleeping; }
        bool isFadedOut() const noexcept { return outGain <= 0.0f; }
        int getType() const noexcept { return type; }

    private:
        struct Delay { float* buf = nullptr; uint32_t mask = 0; };
        struct Shelf { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f; };

        static int nextPow2 (int n) noexcept { int p = 1; while (p < n) p <<= 1; return p; }
        static float approach (float v, float t, float step) noexcept { return v < t ? std::min (v + step, t) : std::max (v - step, t); }

        float readLinear (const Delay& d, float delay) const noexcept
        {
            const int whole = (int) delay;
            const float f = delay - (float) whole;
            const uint32_t i = writePos - (uint32_t) whole;
            const float a = d.buf[i & d.mask], b = d.buf[(i - 1u) & d.mask];
            return a + f * (b - a);
        }

        // Third order Lagrange: keeps the highs through the modulated lines, so interpolation does not act as hidden damping.
        // The line is read before this sample is written, so a delay of D means D samples back from the write position.
        float readCubic (const Delay& d, float delay) const noexcept
        {
            const int whole = (int) delay;
            const float f = delay - (float) whole;
            const uint32_t i = writePos - (uint32_t) whole;
            const float xm = d.buf[(i + 1u) & d.mask], x0 = d.buf[i & d.mask], x1 = d.buf[(i - 1u) & d.mask], x2 = d.buf[(i - 2u) & d.mask];
            const float fm = f - 1.0f, fp = f + 1.0f, f2 = f - 2.0f;
            return -f * fm * f2 * (1.0f / 6.0f) * xm + fp * fm * f2 * 0.5f * x0 - fp * f * f2 * 0.5f * x1 + fp * f * fm * (1.0f / 6.0f) * x2;
        }

        float allpass (Delay& d, int len, float x, float g) noexcept
        {
            const float z = d.buf[(writePos - (uint32_t) len) & d.mask];
            const float w = x - g * z;
            d.buf[writePos & d.mask] = w;
            return z + g * w;
        }

        // Per line loss: the loop loses exactly 60 dB per Decay at the reference frequency. A second order shelf with a
        // fixed corner adds the extra loss at the top, so Damping leaves the mids and lows alone.
        void updateLoss (const Voice& v, float scale, float decaySeconds, float hfRatio) noexcept
        {
            const float nyquistSafe = 0.45f * fs;
            const float K = std::tan (3.14159265f * std::min (v.shelfHz, nyquistSafe) / fs);
            const float xRef = std::tan (3.14159265f * kRefHz / fs) / K;
            const float x2 = xRef * xRef, x4 = x2 * x2;
            const float K2 = K * K, root2 = 1.41421356f;
            const float a0 = 1.0f + root2 * K + K2;
            const float a1 = 2.0f * (K2 - 1.0f) / a0, a2 = (1.0f - root2 * K + K2) / a0;

            for (int k = 0; k < kNumLines; ++k)
            {
                const float samples = v.lineMs[k] * 0.001f * fs * scale + (float) loopApLen[k];
                const float lossDb = 60.0f * samples / (decaySeconds * fs);
                const float hfLossDb = std::min (lossDb / std::max (hfRatio, 0.02f), 90.0f);
                const float g = std::pow (10.0f, -0.05f * lossDb);
                const float r = std::pow (10.0f, -0.05f * std::max (hfLossDb - lossDb, 0.0f)); // high frequency gain against low
                const float q = std::sqrt (r);
                // |H|^2 = (1 + q^4 x^4) / (1 + x^4). Dividing by it at the reference keeps Decay exact there.
                const float atRef = std::sqrt ((1.0f + r * r * x4) / (1.0f + x4));
                const float gain = std::min (g / atRef, std::sqrt (g));
                auto& s = shelf[k];
                s.b0 = gain * (r + root2 * q * K + K2) / a0;
                s.b1 = gain * 2.0f * (K2 - r) / a0;
                s.b2 = gain * (r - root2 * q * K + K2) / a0;
                s.a1 = a1;
                s.a2 = a2;
            }
        }

        float fs = 48000.0f;
        int type = 0;
        std::vector<float> memory;
        Delay line[kNumLines], loopAp[kNumLines], diff[2][kNumDiffusers], tap[2];
        int loopApLen[kNumLines] {}, diffLenS[2][kNumDiffusers] {};
        float lineDelay[kNumLines] {}, lineInc[kNumLines] {}, target[kNumLines] {};
        float tapDelay[2][kNumTaps] {}, tapInc[2][kNumTaps] {};
        float wobblePhase[kNumLines] {}, wobbleInc[kNumLines] {};
        Shelf shelf[kNumLines];
        uint32_t writePos = 0u;
        float inGain = 1.0f, outGain = 1.0f, inTarget = 1.0f, outTarget = 1.0f;
        float lastScale = -1.0f, lastDecay = -1.0f, lastRatio = -1.0f;
        int quietSamples = 0;
        bool sleeping = false;
    };
}
