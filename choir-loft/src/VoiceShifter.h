#pragma once
// One harmony voice: pitch-synchronous grains read from the shared input history. Starts from the grain shifter in Smooth Tune, changed
// so several voices share one input and one tracker, and so a voice can sit later than the lead without any extra delay line.
//
// Each grain is a short windowed stretch of the input, centred on one of the tracker's pitch marks, laid out at the spacing of the wanted
// output period. Pitch moves while the shape of each period, and so the formants, stays.
//
// Timing: a grain picks the mark nearest its own output position minus the voice's lag, so a voice lags by choosing an older period, not
// by delaying audio. That cannot bend its pitch, because pitch only comes from the spacing of the grains.
// Consonants and breath: grains are read at a fixed offset with windows that sum to 1, so the voice is the input at that lag. No grain
// spacing ever touches an unvoiced sound, which is what keeps consonants from turning into buzz.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "PitchTracker.h"

namespace smoothharmony
{
// The input, kept once for all voices: the mono mix for the grains, and left and right for the delay matched lead and dry paths.
class InputHistory
{
public:
    enum { mono = 0, left = 1, right = 2 };

    void prepare (double sampleRate)
    {
        size = 1;
        while (size < (int) (sampleRate * 0.5)) size *= 2;
        for (auto& b : data) b.assign ((size_t) size * 2, 0.0f);
        written = 0;
    }

    void clear() noexcept
    {
        for (auto& b : data) std::fill (b.begin(), b.end(), 0.0f);
        written = 0;
    }

    void push (float l, float r, float m) noexcept
    {
        const int at = (int) (written & (long long) (size - 1));
        data[left][(size_t) at] = data[left][(size_t) (at + size)] = l;
        data[right][(size_t) at] = data[right][(size_t) (at + size)] = r;
        data[mono][(size_t) at] = data[mono][(size_t) (at + size)] = m;
        ++written;
    }

    float delayed (int channel, int samples) const noexcept
    {
        const long long index = written - 1 - samples;
        return index < 0 ? 0.0f : data[(size_t) channel][(size_t) (index & (long long) (size - 1))];
    }

    const float* grains() const noexcept { return data[mono].data(); }
    int mask() const noexcept { return size - 1; }
    long long newest() const noexcept { return written - 1; }

private:
    std::array<std::vector<float>, 3> data;
    int size = 1;
    long long written = 0;
};

class VoiceShifter
{
public:
    static constexpr int interpolatorReach = 6;

    struct Control
    {
        bool voiced = false;         // false: read the input at the voice's lag, untouched
        double inputPeriod = 0.0;    // samples, from the tracker
        double outputPeriod = 0.0;   // samples, wanted spacing of the grains
        double lag = 0.0;            // samples later than the lead
        double grainRate = 1.0;      // reading speed inside a grain: above 1 moves formants up
        double jitter = 0.0;         // fraction of the period, random per grain
        double shimmer = 0.0;        // fraction of the level, random per grain
    };

    void prepare (double sampleRate, unsigned seed)
    {
        sr = sampleRate;
        fallback = std::max (16, (int) std::lround (sr * 0.005));
        randomSeed = seed;
        buildTables();
        restart();
    }

    void setLatency (int samples) noexcept { latency = samples; }

    // Drops all grains and starts again as a plain read of the input. Call only while the output is faded out.
    void restart() noexcept
    {
        numActive = 0;
        havePending = false;
        lastMarkUsed = -1;
        needDecision = true;
        firstGrain = true;
        inPlainRead = false;
        plainOffset = 0.0;
        random = randomSeed;
        planned = Plan::pass;
    }

    // One output sample. Call after the newest input has gone into history and the tracker.
    float step (const PitchTracker& tracker, const Control& control, const InputHistory& history) noexcept
    {
        const long long newest = history.newest();
        const long long n = newest - latency;

        if (firstGrain)
        {
            Grain g;
            g.mark = (double) n;
            g.fall = fallback;
            g.rise = 1.0;
            g.offset = plainOffset = -control.lag;
            inPlainRead = true;
            splitOffset (g);
            addGrain (g);
            peakOfNewest = (double) n;
            firstGrain = false;
        }

        if (needDecision || (double) n >= peakOfNewest)
        {
            decide (tracker, control);
            needDecision = false;
        }
        if (havePending && (double) n >= pending.mark - pending.rise)
        {
            activate (tracker, control);
            havePending = false;
        }

        double sum = 0.0, weight = 0.0;
        int keep = 0;
        for (int k = 0; k < numActive; ++k)
        {
            const double t = (double) n - active[(size_t) k].mark;
            const double rise = active[(size_t) k].rise, fall = active[(size_t) k].fall;
            double w;
            if (t < 0.0)
            {
                const double x = 1.0 + t / rise;
                w = x <= 0.0 ? 0.0 : rising (x);
            }
            else
            {
                const double x = t / fall;
                if (x >= 1.0 && active[(size_t) k].fallIsFinal)
                    continue; // finished
                w = x >= 1.0 ? 0.0 : 1.0 - rising (x);
            }
            if (keep != k)
                active[(size_t) keep] = active[(size_t) k];
            const Grain& g = active[(size_t) keep];
            ++keep;
            if (w <= 0.0)
                continue;
            weight += w;
            sum += w * g.gain * read (g, n, t, newest, history);
        }
        numActive = keep;

        return (float) (weight > 1.0 ? sum / weight : sum);
    }

private:
    struct Grain
    {
        double mark = 0.0;          // centre, in input samples at the reported delay
        double rise = 1.0;          // length of the rising half
        double fall = 1.0;          // length of the falling half
        bool fallIsFinal = false;   // false until the next grain has been decided, so the fall length is known
        double offset = 0.0;        // read position minus output position, fixed for the grain
        double rate = 1.0;
        double gain = 1.0;
        long long whole = 0;        // offset split up for the fixed rate case
        double fraction = 0.0;
        std::array<float, 12> coefficients {};
    };

    static constexpr int tapCount = 12, phases = 256, windowSize = 2048, maxGrains = 16;

    void buildTables()
    {
        interp.assign ((size_t) (phases + 1) * tapCount, 0.0f);
        const auto bessel = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 32; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
            return sum;
        };
        const double beta = 8.0;
        for (int p = 0; p <= phases; ++p)
        {
            const double f = (double) p / phases;
            double total = 0.0;
            std::array<double, tapCount> row {};
            for (int t = 0; t < tapCount; ++t)
            {
                const double x = (double) (t - (tapCount / 2 - 1)) - f;
                const double u = x / (double) (tapCount / 2);
                const double kaiser = std::abs (u) >= 1.0 ? 0.0 : bessel (beta * std::sqrt (1.0 - u * u)) / bessel (beta);
                const double sinc = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (M_PI * x) / (M_PI * x);
                row[(size_t) t] = kaiser * sinc;
                total += row[(size_t) t];
            }
            for (int t = 0; t < tapCount; ++t)
                interp[(size_t) (p * tapCount + t)] = (float) (row[(size_t) t] / total);
        }
        win.resize (windowSize + 2);
        for (int i = 0; i < windowSize + 2; ++i)
            win[(size_t) i] = 0.5 - 0.5 * std::cos (M_PI * (double) i / windowSize);
    }

    double rising (double x) const noexcept
    {
        const double at = x * windowSize;
        const int i = (int) at;
        return win[(size_t) i] + (at - i) * (win[(size_t) i + 1] - win[(size_t) i]);
    }

    void splitOffset (Grain& g) noexcept
    {
        const double whole = std::floor (g.offset);
        g.whole = (long long) whole;
        g.fraction = g.offset - whole;
        if (g.fraction < 1.0e-9) { g.fraction = 0.0; }
        else if (g.fraction > 1.0 - 1.0e-9) { g.fraction = 0.0; ++g.whole; }
        if (g.fraction > 0.0)
            tableRow (g.fraction, g.coefficients.data());
    }

    void tableRow (double fraction, float* out) const noexcept
    {
        const double at = fraction * phases;
        const int p = std::min ((int) at, phases - 1);
        const float a = (float) (at - p);
        const float* r0 = interp.data() + p * tapCount;
        const float* r1 = r0 + tapCount;
        for (int t = 0; t < tapCount; ++t)
            out[t] = r0[t] + a * (r1[t] - r0[t]);
    }

    float read (const Grain& g, long long n, double t, long long newest, const InputHistory& history) const noexcept
    {
        const float* d = history.grains();
        const int mask = history.mask();
        if (g.rate == 1.0)
        {
            const long long index = n + g.whole;
            if (g.fraction == 0.0)
                return index < 0 ? 0.0f : d[index & mask];
            const float* x = d + ((index - (tapCount / 2 - 1)) & mask);
            float total = 0.0f;
            for (int i = 0; i < tapCount; ++i)
                total += x[i] * g.coefficients[(size_t) i];
            return total;
        }
        // The grain is read at a different speed, so its formants move. Never read past the newest sample.
        double pos = (double) n + g.offset + (g.rate - 1.0) * t;
        pos = std::min (pos, (double) (newest - interpolatorReach));
        const double whole = std::floor (pos);
        std::array<float, tapCount> row;
        tableRow (pos - whole, row.data());
        const float* x = d + (((long long) whole - (tapCount / 2 - 1)) & mask);
        float total = 0.0f;
        for (int i = 0; i < tapCount; ++i)
            total += x[i] * row[(size_t) i];
        return total;
    }

    void addGrain (const Grain& g) noexcept
    {
        if (numActive < maxGrains)
            active[(size_t) numActive++] = g;
    }

    Grain* newestGrain() noexcept { return numActive > 0 ? &active[(size_t) (numActive - 1)] : nullptr; }

    // -1 to 1, the same sequence every run, so a bounce always comes out the same.
    double nextRandom() noexcept
    {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return (double) (random & 0xFFFFFFu) / 8388607.5 - 1.0;
    }

    // Called when the newest grain reaches its centre: fixes its fall and schedules the next grain.
    void decide (const PitchTracker& tracker, const Control& c) noexcept
    {
        Grain* current = newestGrain();
        const double s = peakOfNewest;
        Grain next;
        next.rate = 1.0;
        double spacing;

        if (! c.voiced || c.inputPeriod <= 0.0 || ! tracker.hasMarks())
        {
            planned = Plan::pass;
            spacing = fallback;
            next.rise = spacing;
            if (current != nullptr) { current->fall = spacing; current->fallIsFinal = true; }
            next.mark = s + spacing;
        }
        else
        {
            planned = Plan::mapped;
            const double period = c.inputPeriod;
            const double wanted = std::clamp (c.outputPeriod > 0.0 ? c.outputPeriod : period, 0.25 * period, 4.0 * period);
            spacing = wanted * (1.0 + c.jitter * nextRandom());

            // Windows are one input period each side of the mark, the classic choice that keeps exactly one glottal pulse per grain.
            // The rise never takes longer than the spacing, because the next grain cannot start before it has been decided.
            if (current != nullptr) { current->fall = period; current->fallIsFinal = true; }
            next.rise = std::min (spacing, period);
            next.mark = s + spacing;

            const double ratio = period / spacing;
            // Pulses get denser or sparser, so scale for equal power. Once shifted, overlapping grains are no longer in phase and
            // their crossfade loses a little energy, which the last factor gives back.
            const double power = spacing >= period ? 1.0 / std::sqrt (ratio) : std::sqrt (ratio);
            next.gain = power * (1.0 + 0.09 * std::min (1.0, std::abs (std::log2 (ratio)) * 12.0 / 1.5));
            next.gain *= 1.0 + c.shimmer * nextRandom();
            // The grains' harmonics drag the formants along with a shift. Reading the grain a little slower cancels that. Tuned at +/-7 semitones on
            // the test voices, see the formant test. Smooth Tune's larger values, tuned at +/-3, overshoot here and put formants 10% low.
            next.rate = c.grainRate;
            if (ratio > 1.0 + 1.0e-4) next.rate *= std::pow (ratio, -0.08);
            else if (ratio < 1.0 - 1.0e-4) next.rate *= std::pow (ratio, 0.05);
        }

        pending = next;
        havePending = true;
        peakOfNewest = next.mark;
    }

    // Called when the grain begins, the last moment to pick which part of the input it reads.
    void activate (const PitchTracker& tracker, const Control& c) noexcept
    {
        Grain g = pending;
        bool plain = planned == Plan::pass || ! c.voiced || ! tracker.hasMarks();

        if (! plain)
        {
            const double period = c.inputPeriod;
            const double aim = g.mark - c.lag;
            // In order, one mark per grain, unless the output has moved over 0.6 periods from where the input is.
            long long q = std::clamp (lastMarkUsed < 0 ? tracker.lastMark() : lastMarkUsed + 1, tracker.firstMark(), tracker.lastMark());
            while (q < tracker.lastMark() && aim - tracker.mark (q).position > 0.6 * period)
                ++q;
            while (q > tracker.firstMark() && tracker.mark (q).position - aim > 0.6 * period)
                --q;
            const double position = tracker.mark (q).position;
            const double offset = position - g.mark;
            if (std::abs (position - aim) > 1.6 * period || offset > (double) latency)
            {
                plain = true; // no usable mark near here: a plain read for one grain rather than a far jump
            }
            else
            {
                g.offset = offset;
                lastMarkUsed = q;
            }
        }

        if (plain)
        {
            // The lag is held for the whole consonant. Grains at the same offset sum to exactly the input. Grains at different offsets
            // would lose level and comb on noise, because their crossfades are no longer in phase.
            if (! inPlainRead)
                plainOffset = -c.lag;
            inPlainRead = true;
            g.offset = plainOffset;
            g.rate = 1.0;
            g.gain = 1.0;
            lastMarkUsed = -1;
        }
        else
        {
            inPlainRead = false;
        }
        splitOffset (g);
        addGrain (g);
    }

    enum class Plan { pass, mapped };

    double sr = 48000.0;
    int latency = 0, fallback = 240;
    std::vector<float> interp;
    std::vector<double> win;

    std::array<Grain, maxGrains> active {};
    int numActive = 0;
    Grain pending;
    bool havePending = false, needDecision = true, firstGrain = true, inPlainRead = false;
    Plan planned = Plan::pass;
    double peakOfNewest = 0.0, plainOffset = 0.0;
    long long lastMarkUsed = -1;
    std::uint32_t random = 1, randomSeed = 1;

};
} // namespace smoothharmony
