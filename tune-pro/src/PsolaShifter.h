#pragma once
// Pitch-synchronous grain shifter. Written to be copied into other plug-ins: it knows nothing about keys or scales.
//
// Each grain is a short windowed stretch of the delayed input, centred on one of the tracker's pitch marks. Grains are laid
// out at the spacing of the wanted output period, so pitch moves while the shape of each period, and so the formants, stays.
//
// Three things it does for sound quality:
// - A falling grain and the next rising grain share one raised cosine, so the windows sum to exactly 1. With no correction,
//   or on an unvoiced sound, grains read the input at the reported delay and the output is the delayed input, to rounding.
//   So consonants and breath really are untouched, and there is no separate path to switch to.
// - Marks are chosen in order, with a margin, so a sustained note does not flip between two candidate periods.
// - The grain read point is fixed for the grain, with a 12 point sinc for the fractional part, so high end stays.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "PitchTracker.h"

namespace smoothtune
{
class PsolaShifter
{
public:
    static constexpr int interpolatorReach = 6;

    struct Control
    {
        bool voiced = false;         // false: pass the input through at the reported delay
        double inputPeriod = 0.0;    // samples, from the tracker
        double outputPeriod = 0.0;   // samples, wanted spacing of the grains
        bool noCorrection = false;   // the output pitch is the input pitch: lock to the marks for a clean pass
        double grainRate = 1.0;      // reading speed inside a grain: above 1 moves formants up
        bool plainGrains = true;     // the grains carry the voice itself, not a whitened voice, so their formants need help
        bool centreOnPulses = false; // line grains up with the pulses in a whitened voice
        double pulseLowestHz = 0.0;  // no lining up below this pitch
        double downReadPower = 0.2;  // whitened grains read slower by the shift to this power when going down
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
    int getLatency() const noexcept { return latency; }

    void reset() noexcept
    {
        for (auto& b : delay) std::fill (b.begin(), b.end(), 0.0f);
        written = 0;
        restart();
    }

    // Drops all grains and starts again as a clean pass of the delayed input. Call only while the output is faded out.
    void restart() noexcept
    {
        numActive = 0;
        havePending = false;
        lastMarkUsed = -1;
        lastOffset = 0.0;
        plannedOffset = 0.0;
        needDecision = true;
        firstGrain = true;
        pulseOffset = 0.0;
        pulseGrains = 0;
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
        const long long newest = written - 1;
        const long long n = newest - latency;

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
                sum[ch] += scaled * read (ch, h, n, t, newest);
        }
        numActive = keep;

        // Plain grains are the voice itself, so overlaps are kept to its level. Whitened grains hold one pulse each and their overlaps are nearly empty.
        const double divide = weight > 1.0 && plain ? 1.0 / weight : 1.0;
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

    float read (int ch, const Grain& g, long long n, double t, long long newest) const noexcept
    {
        const float* d = delay[(size_t) ch].data();
        const int mask = size - 1;
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

    // A pitch mark sits on the loudest point of the voice, not on the pulse that drives it. In a whitened voice the pulse stands out, so
    // grains centre on it. Then each grain holds one pulse, not parts of two, which would land at two different places in the new period.
    void alignToPulse (const PitchTracker& tracker, const Control& c, double period) noexcept
    {
        // Above about 400 Hz there is too little between pulses to tell them apart from what the model left behind.
        if (! tracker.hasMarks() || period < sr / 400.0 || period * c.pulseLowestHz > sr)
        {
            pulseOffset = 0.0;
            pulseGrains = 0;
            return;
        }
        const long long newest = written - 1;
        const long long q = std::min (tracker.lastMark(), lastMarkUsed >= 0 ? lastMarkUsed : tracker.lastMark());
        const double m = tracker.mark (q).position;
        const int halfSpan = (int) (0.5 * period), width = std::max (2, (int) (0.06 * period));
        const long long end = std::min ((long long) m + halfSpan, newest);
        const long long start = end - 2 * halfSpan;
        if (start < 0)
            return;
        const int mask = size - 1;
        double running = 0.0, best = -1.0, total = 0.0;
        long long bestAt = start;
        for (long long i = start - width; i < end; ++i)
        {
            const double e0 = delay[0][(size_t) (i & mask)];
            const double e1 = channels > 1 ? delay[1][(size_t) (i & mask)] : 0.0;
            running += e0 * e0 + e1 * e1;
            if (i >= start) total += e0 * e0 + e1 * e1;
            const double o0 = delay[0][(size_t) ((i - width) & mask)];
            const double o1 = channels > 1 ? delay[1][(size_t) ((i - width) & mask)] : 0.0;
            running -= o0 * o0 + o1 * o1;
            if (i >= start && running > best) { best = running; bestAt = i - width / 2; }
        }
        // No clear pulse, as on a nearly pure high note: nothing to line up with.
        if (best * 2.0 * halfSpan < 4.0 * total * width)
            return;
        double found = (double) bestAt - m;
        found -= period * std::round (found / period);
        double difference = found - pulseOffset;
        difference -= period * std::round (difference / period);
        // The pulse sits at a fixed place against the marks, so once found this hardly moves. Peak picking is noisy and any movement here is jitter.
        if (pulseGrains == 0)
        {
            // A new note: go straight there, so the first grains are already in line.
            pulseOffset = found;
            pulseGrains = 1;
        }
        else if (std::abs (difference) < 0.2 * period)
        {
            pulseOffset += 0.01 * difference;
        }
    }

    // Called when the newest grain reaches its centre: fixes its fall and schedules the next grain.
    void decide (const PitchTracker& tracker, const Control& c) noexcept
    {
        plain = c.plainGrains;
        if (plain || ! c.centreOnPulses)
            pulseOffset = 0.0;
        else if (c.voiced && c.inputPeriod > 0.0 && ! c.noCorrection)
            alignToPulse (tracker, c, c.inputPeriod);
        else
            pulseGrains = 0;
        Grain* current = newestGrain();
        const double s = peakOfNewest;
        Grain next;
        next.rate = 1.0;

        double spacing;
        planned = Plan::pass;

        if (! c.voiced || c.inputPeriod <= 0.0)
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
            const double wanted = std::clamp (c.outputPeriod > 0.0 ? c.outputPeriod : period, 0.2 * period, 5.0 * period);
            spacing = wanted;
            planned = Plan::mapped;

            if (c.noCorrection && tracker.hasMarks() && lastMarkUsed >= 0)
            {
                // Output pitch is input pitch: step mark to mark at the same delay, so the signal just passes.
                const long long following = lastMarkUsed + 1;
                if (following <= tracker.lastMark() && following >= tracker.firstMark())
                {
                    // The grain before may have read its mark from somewhere else than the reported delay: every corrected grain does, and
                    // so does one whose mark was not known in time. Up to 1.5.1 that offset was then kept for as long as nothing was
                    // corrected, so the "untouched" voice came out up to 0.6 of a period early or late (62 samples early on a real take),
                    // which combs against the dry signal at any Mix under 100. Now it is walked back to the reported delay, by at most
                    // 0.3% of the period a grain: 5 cents, for 200 periods at the very most (about a second), the same thing the grains
                    // do while correcting a note by 5 cents. Going back in one grain would make one period up to 60% too long or short.
                    double offset = lastOffset;
                    if (walkLockedOffset && offset != 0.0)
                    {
                        const double most = lockedWalkShare * period;
                        offset -= std::clamp (offset, -most, most);
                    }
                    const double m = tracker.mark (following).position + pulseOffset - offset;
                    const double gap = m - s;
                    if (gap > 0.5 * period && gap < 1.6 * period)
                    {
                        spacing = gap;
                        planned = Plan::locked;
                        plannedOffset = offset;
                    }
                }
            }
            else if (lastMarkUsed < 0 && tracker.hasMarks())
            {
                // Coming in from a pass: land the first grain exactly on a mark, at the reported delay, for a clean entry.
                const double aim = s + period;
                long long found = -1;
                double foundGap = 1.0e12;
                for (long long q = tracker.lastMark(); q >= tracker.firstMark(); --q)
                {
                    const double gap = tracker.mark (q).position + pulseOffset - s;
                    if (gap < 0.5 * period)
                        break;
                    if (std::abs (s + gap - aim) < foundGap) { foundGap = std::abs (s + gap - aim); found = q; }
                }
                if (found >= 0)
                {
                    spacing = tracker.mark (found).position + pulseOffset - s;
                    lastMarkUsed = found - 1;
                    lastOffset = 0.0;
                    plannedOffset = 0.0;
                    planned = Plan::locked;
                }
                else
                {
                    planned = Plan::pass;
                    spacing = std::min ((double) fallback, std::max (16.0, period));
                }
            }

            // Windows are one input period each side of the mark, the classic choice that keeps exactly one glottal pulse per
            // grain. Longer ones were measured and only flattened the formants further. The rise never takes longer than the
            // spacing, because the next grain cannot start before it has been decided.
            const double shortest = period, longest = period;
            const double half = std::clamp (spacing, shortest, longest);
            if (current != nullptr) { current->fall = half; current->fallIsFinal = true; }
            next.rise = std::min (spacing, longest);
            next.mark = s + spacing;

            if (planned != Plan::pass)
            {
                const double ratio = period / spacing;
                if (planned == Plan::locked || std::abs (ratio - 1.0) < 1.0e-4)
                    next.gain = 1.0;
                else
                {
                    // Pulses get denser or sparser, so scale for equal power. Once shifted, overlapping grains are no longer
                    // in phase and their crossfade loses a little energy, which the last factor gives back.
                    if (c.plainGrains)
                    {
                        const double power = spacing >= shortest ? 1.0 / std::sqrt (ratio) : std::sqrt (ratio);
                        next.gain = power * (1.0 + 0.09 * std::min (1.0, std::abs (std::log2 (ratio)) * 12.0 / 1.5));
                    }
                    else
                    {
                        // Pulses at a new spacing: each one's strength goes with the spacing's square root for the same power.
                        next.gain = 1.0 / std::sqrt (ratio);
                    }
                }
                next.rate = c.grainRate;
                // The grains' harmonics drag the formants along with a shift, by up to a third of it upwards and less downwards,
                // measured against a voice with the same vocal tract. Reading the grain a little slower cancels most of that.
                // Tuned on the test voices: see the formant test and README.
                if (c.plainGrains && planned != Plan::locked && ratio > 1.0 + 1.0e-4)
                    next.rate *= std::pow (ratio, -0.25);
                else if (c.plainGrains && planned != Plan::locked && ratio < 1.0 - 1.0e-4)
                    next.rate *= std::pow (ratio, 0.1);
                // Whitened grains still pull the first formant up a little when going down. Measured on the test voices, as for plain grains.
                // High Quality's sweep model does not, so it turns this off.
                else if (! c.plainGrains && planned != Plan::locked && ratio < 1.0 - 1.0e-4)
                    next.rate *= std::pow (ratio, c.downReadPower);
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

        if (planned == Plan::pass || ! c.voiced || ! tracker.hasMarks() || lastMarkUsed < 0)
        {
            g.offset = 0.0;
            g.rate = 1.0;
            g.gain = 1.0;
            lastMarkUsed = -1;
            lastOffset = 0.0;
        }
        else if (planned == Plan::locked)
        {
            const long long following = std::clamp (lastMarkUsed + 1, tracker.firstMark(), tracker.lastMark());
            g.offset = plannedOffset;
            lastOffset = plannedOffset;
            lastMarkUsed = following;
        }
        else
        {
            const double period = c.inputPeriod;
            // In order, one mark per grain, unless the output has moved over 0.6 periods from where the input is.
            long long q = std::clamp (lastMarkUsed + 1, tracker.firstMark(), tracker.lastMark());
            while (q < tracker.lastMark() && g.mark - (tracker.mark (q).position + pulseOffset) > 0.6 * period)
                ++q;
            while (q > tracker.firstMark() && tracker.mark (q).position + pulseOffset - g.mark > 0.6 * period)
                --q;
            const double offset = tracker.mark (q).position + pulseOffset - g.mark;
            if (std::abs (offset) > 1.6 * period || offset > (double) latency - interpolatorReach)
            {
                // No usable mark near here. Fall back to a plain read for one grain rather than jump far.
                g.offset = 0.0;
                g.rate = 1.0;
                g.gain = 1.0;
                lastMarkUsed = -1;
                lastOffset = 0.0;
            }
            else
            {
                g.offset = offset;
                lastMarkUsed = q;
                lastOffset = offset;
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
    double peakOfNewest = 0.0, lastOffset = 0.0, plannedOffset = 0.0, pulseOffset = 0.0;
    static constexpr bool walkLockedOffset = false;
    static constexpr double lockedWalkShare = 0.003;
    int pulseGrains = 0;
    bool plain = true;
    long long lastMarkUsed = -1;
};
} // namespace smoothtune
