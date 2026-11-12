#pragma once
// The effect modules of Smooth Tune FX, free of plug-in framework code. None of them has any delay of its own on the direct signal,
// so switching one never changes the plug-in's latency. Everything is allocated in prepare().
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tunefx
{
inline constexpr double twoPi = 6.283185307179586;

// Mix law used by Vocoder, Delay and Reverb: the dry side stays whole up to 50%, the wet side reaches whole at 50%.
inline float mixDry (float m) noexcept { return std::min (1.0f, 2.0f * (1.0f - m)); }
inline float mixWet (float m) noexcept { return std::min (1.0f, 2.0f * m); }

struct Smoother
{
    float value = 0.0f, coef = 1.0f;
    void prepare (double rate, double ms, float start) noexcept { coef = (float) (1.0 - std::exp (-1000.0 / (ms * rate))); value = start; }
    float next (float target) noexcept
    {
        value += coef * (target - value);
        if (std::abs (target - value) < 1.0e-6f) value = target;
        return value;
    }
};

// The On switch of a module. Reaches exactly 0, and then the module is left out of the signal altogether.
struct ModuleFade
{
    float gain = 0.0f, step = 0.001f;
    void prepare (double rate, double seconds, bool on) noexcept { step = (float) (1.0 / (seconds * rate)); gain = on ? 1.0f : 0.0f; }
    float next (bool on) noexcept
    {
        gain = on ? std::min (1.0f, gain + step) : std::max (0.0f, gain - step);
        return gain * gain * (3.0f - 2.0f * gain);
    }
    bool silent() const noexcept { return gain <= 0.0f; }
};

struct Random
{
    std::uint32_t state = 0x2545f491u;
    float bipolar() noexcept
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (float) ((double) state / 2147483648.0 - 1.0);
    }
};

// Holds a shifted voice at the level of the voice it came from. A formant-preserving shift goes up and down by a few dB as harmonics
// land on or off a formant, which would make a Pitch or Harmony setting louder on some notes than others. Slow, and limited to +/-6 dB.
struct LevelMatch
{
    float inPower = 0.0f, outPower = 0.0f, gain = 1.0f, powerCoef = 0.001f, gainCoef = 0.001f;
    void prepare (double rate) noexcept
    {
        powerCoef = (float) (1.0 - std::exp (-1.0 / (0.06 * rate)));
        gainCoef = (float) (1.0 - std::exp (-1.0 / (0.03 * rate)));
        inPower = outPower = 0.0f;
        gain = 1.0f;
    }
    float next (float in, float out) noexcept
    {
        inPower += powerCoef * (in * in - inPower);
        outPower += powerCoef * (out * out - outPower);
        const float wanted = std::clamp (std::sqrt ((inPower + 1.0e-8f) / (outPower + 1.0e-8f)), 0.5f, 2.0f);
        gain += gainCoef * (wanted - gain);
        return gain;
    }
};

// Holds Distortion's output at the level of the voice going in, slowly, so switching it on does not make the voice louder or quieter.
// The make-up gain for each Drive (PluginProcessor) is set for a -18 dBFS voice; a quieter one goes through the curve almost straight and
// came out up to 6.8 dB louder (Luke's takes, Drive 30: 3.5 dB). It only learns while there is sound going in (over -70 dBFS), so a pause
// leaves it where it was, and it is slow (0.3 s) so the curve's own squash within a note stays. Limited to -12 and +6 dB.
struct DistortionLevel
{
    float inPower = 0.0f, outPower = 0.0f, gain = 1.0f, powerCoef = 0.001f, gainCoef = 0.001f, fastCoef = 0.01f;
    int warm = 0, warmSamples = 960;
    void prepare (double rate) noexcept
    {
        powerCoef = (float) (1.0 - std::exp (-1.0 / (0.3 * rate)));
        gainCoef = (float) (1.0 - std::exp (-1.0 / (0.1 * rate)));
        fastCoef = (float) (1.0 - std::exp (-1.0 / (0.004 * rate)));
        warmSamples = (int) (0.02 * rate);
        reset();
    }
    void reset() noexcept { inPower = outPower = 0.0f; gain = 1.0f; warm = warmSamples; }
    // After a reset (Distortion switched on, or the plug-in prepared) it reads the level quickly for the first 20 ms of sound, under the On switch's 15 ms fade, and sets the hold
    // straight from that, starting from start (the curve's gain for a quiet voice turned round). Then slow as above. 1.2.0 began at 1,
    // no hold, and came in up to 7 dB high or low until the slow reading caught up.
    float next (float in, float out, float start) noexcept
    {
        if (warm > 0)
        {
            if (warm == warmSamples) gain = std::clamp (start, 0.25f, 2.0f);
            inPower += fastCoef * (in * in - inPower);
            if (inPower > 1.0e-7f) --warm;   // the quick reading is spent on sound, not on a pause before it
            outPower += fastCoef * (out * out - outPower);
            if (inPower > 1.0e-7f && outPower > 1.0e-9f)
                gain = std::clamp (std::sqrt (inPower / outPower), 0.25f, 2.0f);
            return gain;
        }
        inPower += powerCoef * (in * in - inPower);
        outPower += powerCoef * (out * out - outPower);
        if (inPower > 1.0e-7f && outPower > 1.0e-9f)
            gain += gainCoef * (std::clamp (std::sqrt (inPower / outPower), 0.25f, 2.0f) - gain);
        return gain;
    }
};

// ----------------------------------------------------------------------------------------------------------- Doubler line

// A short delay line with a moving read head, one per double.
class DoublerLine
{
public:
    void prepare (double rate, double maxSeconds)
    {
        size = 1;
        while (size < (int) (rate * maxSeconds) + 8) size *= 2;
        line.assign ((size_t) size, 0.0f);
        write = 0;
    }
    void reset() noexcept { std::fill (line.begin(), line.end(), 0.0f); write = 0; }
    float process (float x, float delaySamples) noexcept
    {
        line[(size_t) write] = x;
        const int mask = size - 1;
        const int whole = (int) delaySamples;
        const float f = delaySamples - (float) whole;
        const float xm = line[(size_t) ((write - whole + 1) & mask)], x0 = line[(size_t) ((write - whole) & mask)],
                    x1 = line[(size_t) ((write - whole - 1) & mask)], x2 = line[(size_t) ((write - whole - 2) & mask)];
        write = (write + 1) & mask;
        const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
        return x0 + f * (c1 + f * (c2 + f * c3));
    }
private:
    std::vector<float> line;
    int size = 1, write = 0;
};

// ----------------------------------------------------------------------------------------------------------- Vocoder tone

// A synth tone at the voice's output pitch, shaped by the voice's own spectrum through a bank of band pass filters.
// A running level match keeps the tone at the voice's level whatever the vowel, pitch or waveform.
class VocoderTone
{
public:
    static constexpr int numBands = 16;
    enum Wave { saw, square, super };

    void prepare (double rate) noexcept
    {
        sr = rate;
        const double top = std::min (8000.0, 0.4 * sr), bottom = 120.0;
        for (int b = 0; b < numBands; ++b)
        {
            const double hz = bottom * std::pow (top / bottom, (double) b / (numBands - 1));
            const double w = twoPi * hz / sr, alpha = std::sin (w) / (2.0 * 4.0), a0 = 1.0 + alpha;
            bands[(size_t) b].b0 = (float) (alpha / a0);
            bands[(size_t) b].a1 = (float) (-2.0 * std::cos (w) / a0);
            bands[(size_t) b].a2 = (float) ((1.0 - alpha) / a0);
        }
        attack = (float) (1.0 - std::exp (-1.0 / (0.002 * sr)));
        release = (float) (1.0 - std::exp (-1.0 / (0.025 * sr)));
        levelCoef = (float) (1.0 - std::exp (-1.0 / (0.08 * sr)));
        gainCoef = (float) (1.0 - std::exp (-32.0 / (0.03 * sr)));
        voicedCoef = (float) (1.0 - std::exp (-1.0 / (0.004 * sr)));
        phase = { 0.0, 0.31, 0.67 };
        inc = 0.005;
        gain = 8.0f;
        gainNoise = 8.0f;
        learntVoiced = learntNoise = false;
        entryStep = (float) (1.0 / (0.01 * sr));
        gainPhase = 0;
        voicePower = tonePower = voicedTarget = 0.0f;
        noise = {};
        reset();
    }

    void reset() noexcept
    {
        for (auto& b : bands) { b.z1 = b.z2 = b.y1 = b.y2 = 0.0f; b.env = 0.0f; }
        voicedNow = 0.0f; // the level match stays where it was, so coming back on starts near the right level
        settle = (int) (0.02 * sr); // and it waits for the filters to ring up before it believes the tone's level
        entry = 0.0f; // silent until then, then in over 10 ms
    }

    void setCarrier (double hz, bool voiced, int waveform) noexcept
    {
        if (hz > 0.0)
            inc = std::min (hz / sr, 0.45);
        voicedTarget = voiced ? 1.0f : 0.0f;
        wave = waveform;
    }

    void process (float voice, float& outL, float& outR) noexcept
    {
        voicedNow += voicedCoef * (voicedTarget - voicedNow);
        const float osc = oscillator();
        const float carrier = voicedNow * osc + (1.0f - voicedNow) * 0.6f * noise.bipolar();

        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < numBands; ++k)
        {
            auto& b = bands[(size_t) k];
            const float a = b.b0 * voice + b.z1;
            b.z1 = b.a1 * -a + b.z2;
            b.z2 = -b.b0 * voice - b.a2 * a;
            const float rect = std::abs (a);
            b.env += (rect > b.env ? attack : release) * (rect - b.env);
            const float y = b.b0 * carrier + b.y1;
            b.y1 = b.a1 * -y + b.y2;
            b.y2 = -b.b0 * carrier - b.a2 * y;
            // Every other band leans to one side, for a wide tone that still sums to mono.
            const float v = y * b.env;
            if ((k & 1) == 0) { l += v; r += 0.5f * v; } else { r += v; l += 0.5f * v; }
        }

        // Level match, feed forward: the gain is worked out from the tone before gain, so it cannot chase itself and overshoot.
        // It moves by ratio rather than by amount, so the start of a note, when the tone lags the voice for a moment, cannot make a burst.
        // Two gains, one learnt on voiced sound (the oscillator) and one on the rest (the noise carrier, consonants and the room), blended
        // as the carrier is. Up to 1.1.1 one gain learnt on both, so after an S or a pause over a room it was set for the noise and the
        // next note came in up to 7.8 dB over its own level for its first 100 ms (PluginTest vocoder; on Luke's takes the tone peaked up to
        // 11.7 dB over the voice). Each gain only learns while its own carrier is playing.
        const float tone = 0.5f * (l + r);
        voicePower += levelCoef * (voice * voice - voicePower);
        tonePower += levelCoef * (tone * tone - tonePower);
        // A gain that has never learnt starts where its first reading puts it, not from a guess: up to 1.1.1 it started at 8 whatever the
        // voice, and on Luke's take 06 the tone came in about 13 dB over its own level for its first 20 ms. Until a gain has a reading the
        // other one stands in, and with neither the tone stays silent. After the filters have rung up (settle) the tone comes in over 10 ms.
        if (settle > 0)
        {
            --settle;
            if (settle == (int) (0.01 * sr)) voicePower = tonePower = 0.0f;   // the readings start once the filters are half way up
        }
        if (++gainPhase >= 32)
        {
            gainPhase = 0;
            if (settle == 0 && voicePower > 1.0e-7f && tonePower > 1.0e-12f)
            {
                const float want = std::clamp (std::sqrt (voicePower / tonePower), 0.5f, 400.0f);
                if (voicedNow > 0.9f) { gain = learntVoiced ? gain * std::pow (want / gain, gainCoef) : want; learntVoiced = true; }
                else if (voicedNow < 0.1f) { gainNoise = learntNoise ? gainNoise * std::pow (want / gainNoise, gainCoef) : want; learntNoise = true; }
            }
        }
        const float gv = learntVoiced ? gain : gainNoise, gn = learntNoise ? gainNoise : gain;
        const bool ready = settle == 0 && (learntVoiced || learntNoise);
        entry = ready ? std::min (1.0f, entry + entryStep) : std::max (0.0f, entry - entryStep);
        const float g = entry * (voicedNow * gv + (1.0f - voicedNow) * gn);
        outL = l * g;
        outR = r * g;
    }

private:
    struct Band { float b0 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0, y1 = 0, y2 = 0, env = 0; };

    static float blep (double t, double dt) noexcept
    {
        if (t < dt) { const double x = t / dt; return (float) (x + x - x * x - 1.0); }
        if (t > 1.0 - dt) { const double x = (t - 1.0) / dt; return (float) (x * x + x + x + 1.0); }
        return 0.0f;
    }
    static float sawAt (double phase, double dt) noexcept { return (float) (2.0 * phase - 1.0) - blep (phase, dt); }

    float oscillator() noexcept
    {
        phase[0] += inc;
        if (phase[0] >= 1.0) phase[0] -= 1.0;
        if (wave == square)
        {
            double half = phase[0] + 0.5;
            if (half >= 1.0) half -= 1.0;
            return (phase[0] < 0.5 ? 0.7f : -0.7f) + 0.7f * blep (phase[0], inc) - 0.7f * blep (half, inc);
        }
        if (wave == super)
        {
            const double up = inc * 1.0046, down = inc * 0.9954; // +/-8 cents
            phase[1] += up;   if (phase[1] >= 1.0) phase[1] -= 1.0;
            phase[2] += down; if (phase[2] >= 1.0) phase[2] -= 1.0;
            return 0.5f * (sawAt (phase[0], inc) + sawAt (phase[1], up) + sawAt (phase[2], down));
        }
        return sawAt (phase[0], inc);
    }

    double sr = 48000.0, inc = 0.005;
    std::array<double, 3> phase { 0.0, 0.31, 0.67 };
    std::array<Band, numBands> bands {};
    float attack = 0, release = 0, levelCoef = 0, gainCoef = 0, voicedCoef = 0, voicePower = 0, tonePower = 0, gain = 8, gainNoise = 8, voicedNow = 0, voicedTarget = 0;
    float entry = 0.0f, entryStep = 0.002f;
    bool learntVoiced = false, learntNoise = false;
    int wave = saw, gainPhase = 0, settle = 0;
    Random noise;
};

// ----------------------------------------------------------------------------------------------------------------- Distortion

// One channel of waveshaping with first order anti-aliasing: the output is the average of the curve across each sample step,
// worked out from the curve's integral. No oversampling, so no delay. The curves crossfade, so changing Type does not click.
class Distortion
{
public:
    enum Type { tube, clip, fold, numTypes };

    void prepare (double rate) noexcept
    {
        dcCoef = (float) (1.0 - twoPi * 10.0 / rate);
        weightStep = (float) (1.0 / (0.005 * rate));
        toneRate = rate;
        reset();
    }
    void reset() noexcept { lastU = 0.0; dcIn = dcOut = toneState = 0.0f; weights = { 1.0f, 0.0f, 0.0f }; first = true; }

    void setTone (double hz) noexcept { toneCoef = hz >= 0.45 * toneRate ? 1.0f : (float) (1.0 - std::exp (-twoPi * hz / toneRate)); }

    float process (float x, float inGain, float outGain, int type) noexcept
    {
        if (first) { for (int t = 0; t < numTypes; ++t) weights[(size_t) t] = t == type ? 1.0f : 0.0f; first = false; }
        const double u = (double) x * inGain;
        double y = 0.0;
        for (int t = 0; t < numTypes; ++t)
        {
            auto& w = weights[(size_t) t];
            w = t == type ? std::min (1.0f, w + weightStep) : std::max (0.0f, w - weightStep);
            if (w > 0.0f)
                y += w * shape (t, u, lastU);
        }
        lastU = u;
        const float wet = (float) y * outGain;
        const float blocked = wet - dcIn + dcCoef * dcOut;
        dcIn = wet;
        dcOut = blocked;
        toneState += toneCoef * (blocked - toneState);
        return toneState;
    }

private:
    static constexpr double bias = 0.25;

    static double logCosh (double v) noexcept
    {
        const double a = std::abs (v);
        return a > 10.0 ? a - 0.693147180559945 : a - 0.693147180559945 + std::log1p (std::exp (-2.0 * a));
    }
    static double curve (int type, double u) noexcept
    {
        switch (type)
        {
            case tube: return std::tanh (u + bias) - std::tanh (bias);
            case clip: return std::clamp (u, -1.0, 1.0);
            default:   return std::sin (u);
        }
    }
    static double integral (int type, double u) noexcept
    {
        switch (type)
        {
            case tube: return logCosh (u + bias) - std::tanh (bias) * u;
            case clip: return std::abs (u) <= 1.0 ? 0.5 * u * u : std::abs (u) - 0.5;
            default:   return -std::cos (u);
        }
    }
    static double shape (int type, double u, double last) noexcept
    {
        const double d = u - last;
        return std::abs (d) < 1.0e-5 ? curve (type, 0.5 * (u + last)) : (integral (type, u) - integral (type, last)) / d;
    }

    double lastU = 0.0, toneRate = 48000.0;
    float dcIn = 0, dcOut = 0, dcCoef = 0.999f, toneState = 0, toneCoef = 1.0f, weightStep = 0.004f;
    std::array<float, numTypes> weights { 1.0f, 0.0f, 0.0f };
    bool first = true;
};

// --------------------------------------------------------------------------------------------------------------------- Filter

// State variable filter in the form that stays clean when the cutoff moves fast.
class SweepFilter
{
public:
    void prepare (double rate) noexcept { sr = rate; reset(); }
    void reset() noexcept { for (auto& c : ic) c = { 0.0f, 0.0f }; }

    void set (double hz, double q) noexcept
    {
        const double g = std::tan (M_PI * std::clamp (hz, 10.0, 0.45 * sr) / sr);
        k = (float) (1.0 / q);
        a1 = (float) (1.0 / (1.0 + g * (g + 1.0 / q)));
        a2 = (float) g * a1;
        a3 = (float) g * a2;
    }

    // Weights: low, band, high. Band is scaled to 0 dB at its peak.
    float process (int ch, float x, float low, float band, float high) noexcept
    {
        auto& c = ic[(size_t) ch];
        const float v3 = x - c[1];
        const float v1 = a1 * c[0] + a2 * v3;
        const float v2 = c[1] + a2 * c[0] + a3 * v3;
        c[0] = 2.0f * v1 - c[0];
        c[1] = 2.0f * v2 - c[1];
        return low * v2 + band * k * v1 + high * (x - k * v1 - v2);
    }

private:
    double sr = 48000.0;
    float k = 1.414f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    std::array<std::array<float, 2>, 2> ic {};
};

// ---------------------------------------------------------------------------------------------------------------------- Delay

// Stereo delay. A change of time crossfades to a second read head, so a new note value never bends the pitch or clicks.
class TempoDelay
{
public:
    void prepare (double rate, double maxSeconds)
    {
        sr = rate;
        size = 1;
        while (size < (int) (rate * maxSeconds) + 8) size *= 2;
        for (auto& l : lines) l.assign ((size_t) size, 0.0f);
        fadeStep = (float) (1.0 / (0.03 * rate));
        highCoef = (float) std::exp (-twoPi * 120.0 / rate);
        current = 0;
        reset();
    }
    void reset() noexcept
    {
        for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
        write = 0;
        moving = false;
        fade = 0.0f;
        for (auto& s : lowState) s = 0.0f;
        for (auto& s : highState) s = 0.0f;
    }
    void setDelay (int samples) noexcept
    {
        samples = std::clamp (samples, 1, size - 4);
        if (current == 0) current = samples;
        if (! moving && samples != current)
        {
            next = samples;
            moving = true;
            fade = 0.0f;
        }
    }
    void setTone (double hz) noexcept { lowCoef = hz >= 0.45 * sr ? 1.0f : (float) (1.0 - std::exp (-twoPi * hz / sr)); }

    // pingPong is 0 to 1. Returns the delayed signal, before the feedback's filters.
    void process (float inL, float inR, float feedback, float pingPong, float& wetL, float& wetR) noexcept
    {
        const int mask = size - 1;
        wetL = lines[0][(size_t) ((write - current) & mask)];
        wetR = lines[1][(size_t) ((write - current) & mask)];
        if (moving)
        {
            const float a = lines[0][(size_t) ((write - next) & mask)], b = lines[1][(size_t) ((write - next) & mask)];
            wetL += fade * (a - wetL);
            wetR += fade * (b - wetR);
            fade += fadeStep;
            if (fade >= 1.0f)
            {
                current = next;
                moving = false;
            }
        }

        const float loopL = shape (0, wetL + pingPong * (wetR - wetL)), loopR = shape (1, wetR + pingPong * (wetL - wetR));
        const float centre = 0.5f * (inL + inR);
        lines[0][(size_t) write] = limit ((inL + pingPong * (centre - inL)) + feedback * loopL);
        lines[1][(size_t) write] = limit ((1.0f - pingPong) * inR + feedback * loopR);
        write = (write + 1) & mask;
    }

private:
    // Tone low pass, plus a gentle fixed high pass, so repeats do not build up mud.
    float shape (int ch, float x) noexcept
    {
        lowState[(size_t) ch] += lowCoef * (x - lowState[(size_t) ch]);
        highState[(size_t) ch] += (1.0f - highCoef) * (lowState[(size_t) ch] - highState[(size_t) ch]);
        return lowState[(size_t) ch] - highState[(size_t) ch];
    }
    // Straight for normal levels, bounded at 2, so the loop can never run away.
    static float limit (float x) noexcept { return x / std::sqrt (1.0f + 0.25f * x * x); }

    double sr = 48000.0;
    std::array<std::vector<float>, 2> lines;
    int size = 1, write = 0, current = 0, next = 0;
    bool moving = false;
    float fade = 0.0f, fadeStep = 0.001f, lowCoef = 1.0f, highCoef = 0.98f;
    std::array<float, 2> lowState {}, highState {};
};
} // namespace tunefx
