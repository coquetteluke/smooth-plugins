#pragma once
// Pitch-synchronous grain shifter, copied from Smooth Tune and changed for a voice changer. It runs on the whitened excitation,
// not on the voice itself: the vocal tract filter goes back on afterwards in MorphEngine, so grains only have to carry pulses.
//
// Each grain is a short windowed stretch of the delayed excitation, centred on a glottal pulse found near one of the tracker's
// pitch marks. Grains are laid out at the spacing of the wanted output period.
//
// - A falling grain and the next rising grain share one raised cosine, so the windows sum to exactly 1 when grains read at the
//   reported delay. With no shift, or on an unvoiced sound, the output is the delayed input, to rounding. Consonants and breath
//   really are untouched, and there is no separate path to switch to.
// - The read point is fixed for the grain, with a 12 point sinc for the fractional part, so high end stays.
// - Changes from Smooth Tune: marks can be ahead of the excitation (the tracker hears the input before whitening is done), grains
//   are centred on the pulse rather than the mark, Monster's alternating spacing, jitter and gain are added, and the small
//   reading speed correction Tune needed for formants is gone because the filter takes care of them.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "PitchTracker.h"

namespace smoothmorph
{
class PsolaShifter
{
public:
    static constexpr int interpolatorReach = 6;
    static constexpr double shortestScale = 0.125, longestScale = 8.0; // the output period against the input's, 36 semitones either way

    struct Control
    {
        bool voiced = false;         // false: pass the input through at the reported delay
        double inputPeriod = 0.0;    // samples, from the tracker
        double periodScale = 1.0;    // output period over input period, from Pitch and Character
        double robot = 0.0;          // 0 to 1: how far the output period is pulled to robotPeriod, on a log scale
        double robotPeriod = 0.0;    // samples
        double vibratoScale = 1.0;   // on the output period, last of all
        double vibratoReach = 1.0;   // how far the vibrato swings the period either way, as a ratio of 1 or more
        bool noCorrection = false;   // the output pitch is the input pitch: pass straight through
        bool neutral = false;        // nothing at all is changed: no pitch, formant, character, vibrato or whisper (level match off)
        double alternate = 0.0;      // every other grain spacing is this fraction longer, and the next shorter: a subharmonic
        double gainAlternate = 0.0;  // every other grain is this fraction louder, the next quieter
        double jitter = 0.0;         // fraction added to this grain's spacing, chosen fresh by the caller
    };

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        channels = std::clamp (numChannels, 1, 2);
        size = 1;
        while (size < (int) (sr * 0.25)) size *= 2;
        for (auto& b : delay) b.assign ((size_t) size * 2, 0.0f);
        fallback = std::max (16, (int) std::lround (sr * 0.005));
        buildTables();
        reset();
    }

    void setLatency (int samples) noexcept { latency = samples; }

    // For the tests only, never read by audio: how far past the newest pulse the tracker has marked a grain has sat while it read a
    // pulse, in periods, since reset (1.2.6).
    double furthestPastNewestPulse() const noexcept { return pastNewest; }
    int getLatency() const noexcept { return latency; }

    void reset() noexcept
    {
        for (auto& b : delay) std::fill (b.begin(), b.end(), 0.0f);
        written = 0;
        pastNewest = 0.0;
        restart();
    }

    // Drops all grains and starts again as a clean pass of the delayed input. Call only while the output is faded out.
    void restart() noexcept
    {
        numActive = 0;
        havePending = false;
        lastMarkUsed = -1;
        needDecision = true;
        firstGrain = true;
        flip = false;
        pulseShift = 0.0;
        pulseKnown = false;
    }

    // Store one input sample for one channel.
    void write (int channel, float x) noexcept
    {
        const int at = (int) (written & (long long) (size - 1));
        delay[(size_t) channel][(size_t) at] = x;
        delay[(size_t) channel][(size_t) (at + size)] = x;
    }
    void advance() noexcept { ++written; }

    // The input sample that lines up with the current output sample, for the bypass path.
    float dry (int channel) const noexcept
    {
        const long long index = written - 1 - latency;
        return index < 0 ? 0.0f : delay[(size_t) channel][(size_t) (index & (long long) (size - 1))];
    }

    // One output sample per channel. tracker indexes count the same input samples as written.
    void step (const PitchTracker& tracker, const Control& control, float* out) noexcept
    {
        const long long n = written - 1 - latency;

        if (firstGrain)
        {
            Grain g;
            g.mark = (double) n;
            g.fall = fallback;
            g.rise = 1.0;
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

        double sum[2] = { 0.0, 0.0 };
        double weight = 0.0;
        int keep = 0;
        for (int k = 0; k < numActive; ++k)
        {
            Grain& g = active[(size_t) k];
            const double t = (double) n - g.mark;
            double w;
            if (t < 0.0)
            {
                const double x = 1.0 + t / g.rise;
                w = x <= 0.0 ? 0.0 : rising (x);
            }
            else
            {
                const double x = t / g.fall;
                if (x >= 1.0 && g.fallIsFinal)
                    continue; // finished
                w = x >= 1.0 ? 0.0 : 1.0 - rising (x);
            }
            active[(size_t) keep] = g;
            Grain& h = active[(size_t) keep];
            ++keep;
            if (w <= 0.0)
                continue;
            weight += w;
            const double scaled = w * h.gain;
            for (int ch = 0; ch < channels; ++ch)
                sum[ch] += scaled * read (ch, h, n);
        }
        numActive = keep;

        const double divide = weight > 1.0 ? 1.0 / weight : 1.0;
        out[0] = (float) (sum[0] * divide);
        if (channels > 1)
            out[1] = (float) (sum[1] * divide);
    }

private:
    struct Grain
    {
        double mark = 0.0;          // centre, in input samples at the reported delay
        double rise = 1.0;          // length of the rising half
        double fall = 1.0;          // length of the falling half
        bool fallIsFinal = false;   // false until the next grain has been decided, so the fall length is known
        double offset = 0.0;        // read position minus output position, fixed for the grain
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

    float read (int ch, const Grain& g, long long n) const noexcept
    {
        const float* d = delay[(size_t) ch].data();
        const int mask = size - 1;
        const long long index = n + g.whole;
        if (g.fraction == 0.0)
            return index < 0 ? 0.0f : d[index & mask];
        const float* x = d + ((index - (tapCount / 2 - 1)) & mask);
        float total = 0.0f;
        for (int i = 0; i < tapCount; ++i)
            total += x[i] * g.coefficients[(size_t) i];
        return total;
    }

    // A mark's position with its own jitter averaged out, using the marks either side that are already known.
    double steadyMark (const PitchTracker& tracker, long long q) const noexcept
    {
        const int reach = (int) std::min ({ 2LL, tracker.lastMark() - q, q - tracker.firstMark() });
        double sum = 0.0;
        for (int k = -reach; k <= reach; ++k)
            sum += tracker.mark (q + k).position;
        return sum / (2 * reach + 1);
    }

    // Where the glottal pulse sits against a mark, in samples. Marks stay on one point of the waveform, but that point can be anywhere
    // in the cycle, and when grains spread out, a pulse near the edge of the window would be lost. It also creeps as a voice glides.
    // First read: the loudest point in a whole period. After that the centre of energy close to where it was, which, unlike the
    // loudest point, cannot flip between two near equal peaks. Following the loudest point measured as tens of cents of wobble.
    void updatePulse (double markPosition, double period) noexcept
    {
        const double centre = markPosition + (pulseKnown ? pulseShift : 0.0);
        const double reach = pulseKnown ? 0.25 * period : 0.5 * period;
        const long long from = (long long) std::floor (centre - reach), to = (long long) std::ceil (centre + reach);
        if (to >= written || from < 0 || written - from >= size)
            return;
        const int mask = size - 1;
        double best = -1.0, sum = 0.0, moment = 0.0;
        long long at = from;
        for (long long i = from; i <= to; ++i)
        {
            double e = 0.0;
            for (int ch = 0; ch < channels; ++ch)
            {
                const double v = delay[(size_t) ch][(size_t) (i & mask)];
                e += v * v;
            }
            if (e > best) { best = e; at = i; }
            e *= e;
            sum += e;
            moment += e * (double) (i - from);
        }
        if (! pulseKnown)
        {
            pulseShift = (double) at - markPosition;
            pulseKnown = true;
        }
        else if (sum > 1.0e-30)
            pulseShift += 0.003 * ((double) from + moment / sum - markPosition - pulseShift);
    }

    void addGrain (const Grain& g) noexcept
    {
        if (numActive < maxGrains)
            active[(size_t) numActive++] = g;
    }

    Grain* newestGrain() noexcept { return numActive > 0 ? &active[(size_t) (numActive - 1)] : nullptr; }

    // Called when the newest grain reaches its centre: fixes its fall and schedules the next grain.
    void decide (const PitchTracker& tracker, const Control& c) noexcept
    {
        Grain* current = newestGrain();
        const double s = peakOfNewest;
        Grain next;

        double spacing;
        planned = Plan::pass;

        if (! c.voiced || c.noCorrection || c.inputPeriod <= 0.0)
        {
            // Pass: integer spacing and a read point at the reported delay.
            spacing = fallback;
            next.rise = spacing;
            if (current != nullptr) { current->fall = spacing; current->fallIsFinal = true; }
            next.mark = s + spacing;
        }
        else
        {
            const double period = c.inputPeriod;
            double want = period * c.periodScale;
            if (c.robot > 0.0 && c.robotPeriod > 0.0)
                want = std::exp ((1.0 - c.robot) * std::log (want) + c.robot * std::log (c.robotPeriod));
            // Grains may be packed down to an eighth of the input period and spread to 8 times it: 36 semitones either way, which covers
            // Pitch and any Character together (Pitch +24 with Child is 31 up; to 1.2.5 the limit was a fifth of the period, 27.9 semitones,
            // and past it the voice came out flat). The move is held inside that before the vibrato, so the vibrato keeps both its halves;
            // Range Match on top can ask for more, and is held there too (1.2.6). Inside the limits this changes nothing.
            want = std::clamp (want, shortestScale * period * c.vibratoReach, longestScale * period / c.vibratoReach);
            const double wanted = std::clamp (want * c.vibratoScale, shortestScale * period, longestScale * period);
            flip = ! flip;
            const double bend = 1.0 + (flip ? c.alternate : -c.alternate) + c.jitter;
            spacing = wanted * bend;
            planned = Plan::mapped;

            if (lastMarkUsed < 0)
            {
                // Coming in from a pass: land the first grain exactly on a mark, at the reported delay, for a clean entry.
                planned = Plan::pass;
                spacing = std::min ((double) fallback, std::max (16.0, period));
                if (tracker.hasMarks())
                {
                    const double aim = s + period;
                    long long found = -1;
                    double foundGap = 1.0e12;
                    for (long long q = tracker.lastMark(); q >= tracker.firstMark(); --q)
                    {
                        const double gap = tracker.mark (q).position - s;
                        if (gap < 0.5 * period)
                            break;
                        if (std::abs (s + gap - aim) < foundGap) { foundGap = std::abs (s + gap - aim); found = q; }
                    }
                    if (found >= 0)
                    {
                        spacing = tracker.mark (found).position - s;
                        lastMarkUsed = found - 1;
                        planned = Plan::locked;
                    }
                }
            }

            // Windows are one input period each side at most, so one glottal pulse per grain. When grains are packed closer than
            // that they shrink to the spacing, so the falling and rising halves still sum to 1.
            const double half = planned == Plan::mapped ? std::min (spacing, period) : spacing;
            if (current != nullptr) { current->fall = half; current->fallIsFinal = true; }
            next.rise = half;
            next.mark = s + spacing;

            if (planned == Plan::mapped)
            {
                // Pulses get denser or sparser, so scale for equal power.
                next.gain = std::sqrt (spacing / period) * (1.0 + (flip ? c.gainAlternate : -c.gainAlternate));
            }
        }

        pending = next;
        havePending = true;
        peakOfNewest = next.mark;
    }

    // Called when the grain begins, the last moment to pick which part of the input it reads.
    void activate (const PitchTracker& tracker, const Control& c) noexcept
    {
        Grain g = pending;
        const auto plainRead = [&]
        {
            g.offset = 0.0;
            g.gain = 1.0;
            lastMarkUsed = -1;
            pulseKnown = false;
        };

        if (planned == Plan::pass || ! c.voiced || ! tracker.hasMarks() || lastMarkUsed < 0)
        {
            plainRead();
        }
        else if (planned == Plan::locked)
        {
            g.offset = 0.0;
            lastMarkUsed = std::clamp (lastMarkUsed + 1, tracker.firstMark(), tracker.lastMark());
        }
        else
        {
            const double period = c.inputPeriod;
            const double shift = pulseShift;
            // In order, one mark per grain, unless the output has moved over 0.6 periods from where the input is.
            long long q = std::clamp (lastMarkUsed + 1, tracker.firstMark(), tracker.lastMark());
            while (q < tracker.lastMark() && g.mark - (tracker.mark (q).position + shift) > 0.6 * period)
                ++q;
            while (q > tracker.firstMark() && tracker.mark (q).position + shift - g.mark > 0.6 * period)
                --q;
            // Reading ahead is limited by the reported delay. A grain from a period earlier is as good.
            const double furthest = (double) (latency - interpolatorReach);
            while (q > tracker.firstMark() && tracker.mark (q).position + shift - g.mark > furthest)
                --q;
            const double steady = steadyMark (tracker, q);
            updatePulse (steady, period);
            double offset = steady + pulseShift - g.mark;
            if (offset > furthest)
                offset -= period;
            if (std::abs (offset) > 2.1 * period || offset > furthest)
            {
                // No usable mark near here. Fall back to a plain read for one grain rather than jump far.
                plainRead();
            }
            else
            {
                g.offset = offset;
                lastMarkUsed = q;
                pastNewest = std::max (pastNewest, (g.mark - (tracker.mark (tracker.lastMark()).position + shift)) / period);
            }
        }
        splitOffset (g);
        addGrain (g);
    }

    enum class Plan { pass, mapped, locked };

    double sr = 48000.0;
    int channels = 2, size = 16384, latency = 0, fallback = 240;
    long long written = 0;
    std::array<std::vector<float>, 2> delay;
    std::vector<float> interp;
    std::vector<double> win;

    std::array<Grain, maxGrains> active {};
    int numActive = 0;
    Grain pending;
    bool havePending = false, needDecision = true, firstGrain = true;
    Plan planned = Plan::pass;
    double peakOfNewest = 0.0, pulseShift = 0.0, pastNewest = 0.0;
    long long lastMarkUsed = -1;
    bool flip = false, pulseKnown = false;
};
} // namespace smoothmorph
