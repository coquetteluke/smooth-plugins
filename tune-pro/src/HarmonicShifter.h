#pragma once
// High Quality's pitch shifter: each harmonic keeps its own level from the voice, and only the vocal tract's part is changed.
//
// Why: a grain shifter keeps the shape of each period as sung. A real voice at a new pitch keeps the shape of its pulse against the
// period instead, so its harmonics keep their levels by number and only the vocal tract, which stays put in frequency, changes them.
// Measured on 2026-11-08: even handed the true vocal tract, grains read 3.45 dB per harmonic against a real voice on held high notes.
//
// How: every output period starts a frame. The input around the matching point is read at a fixed number of samples per input period,
// four periods long, so each harmonic falls on one bin of an FFT. Each harmonic is scaled by how the vocal tract differs between where it
// was and where it is going, the frame is read back at the output period, and frames overlap one per output period. A harmonic too weak
// to trust next to the noise between harmonics borrows the harmonic now sitting where it is going, as grains would.
//
// The vocal tract comes from what each harmonic number was heard at, at each pitch (TractMemory below), or straight lines through this
// moment's harmonics where nothing was heard.
//
// Everything is allocated in prepare(), so the rest is real time safe.
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "PitchTracker.h"

namespace smoothtune
{
// Each harmonic number's level, kept against the pitch it was heard at. Harmonic k heard at the pitch it is being moved to gives the
// vocal tract's change exactly, with nothing to estimate. Where k was never there, another harmonic m that was heard at that frequency
// stands in, once the difference between k's and m's own levels is known: harmonics that were heard at the same frequency at different
// pitches give it. A frame waits settleSeconds before it is stored, so each new frame can be checked against what was heard at its pitch
// before: if it no longer matches, the vowel changed, and everything is forgotten. Nothing here blends one vowel into the next.
class TractMemory
{
public:
    static constexpr int maxK = 64, binsPerSemi = 8, bins = 60 * binsPerSemi, fifoSize = 128;
    static constexpr double lowHz = 60.0;
    static constexpr double forgetSeconds = 2.0;    // a level fades out over this if its pitch is not heard again
    static constexpr double settleSeconds = 0.05;
    static constexpr double changeDb = 6.0;         // spread of the difference that counts as a new vowel. Steady test vowels read up to 4.0, changes 12 to 20
    static constexpr double topHz = 5000.0;         // harmonics above this are not remembered
    static constexpr double solveEverySeconds = 0.01;
    static constexpr double confirmSeconds = 0.2;  // the memory is used only this soon after a moment matched it
    static constexpr double checkSnrDb = 10.0;      // harmonics this far over the noise take part in the check
    static constexpr int checkCount = 2;            // and at least this many of them
    static constexpr double reachDb = 2.0, slopeDbPerHz = 0.25;   // how far the memory may stray from the straight lines, for how far a harmonic moves
    // How much louder the voice itself is at a higher pitch, per dB of pitch ratio, as the memory reads it against the straight lines.
    // The memory reads one harmonic number at two pitches, so it also gets how much louder the source is at the higher pitch (each harmonic
    // of a glottal pulse rises with the pitch). The straight lines read across frequency at one moment, so they get the source's fall with
    // harmonic number instead. On the test voices the two differ by 2 times the pitch ratio in dB, fitted on glides of +/-2 to +/-7
    // semitones (1.73 to 2.12). On a move up the memory has nothing until the voice has been at the new pitch, so a glide starts on the lines
    // and the memory takes over later, louder, and the level trim, settled on the start, let the level drift: the third held out set read
    // 1.62 dB against 1.5. So on moves up the memory's extra loudness is taken out. 3, not the fitted 2, because the lines also miss the
    // extra level of a harmonic crossing a formant, which the trim settles on too: worst level change on the third and fourth held out sets
    // 1.15 and 1.32 dB at 2, 0.72 and 0.91 at 3 (harness), nothing else read changed by more than 0.2. Chosen on those sets, which were
    // already run. On moves down the memory has the lower pitches from the start; taking it out there made big shifts down read 9.8% against 8.
    static constexpr double sourceLoudnessPerDb = 3.0;

    void prepare()
    {
        level.assign ((size_t) maxK * bins, 0.0f);
        stored.assign ((size_t) maxK * bins, 0.0);
        fifo.resize (fifoSize);
        sigma.assign (maxK, 0.0);
        linked.assign (maxK, 0);
        group.assign (maxK, 0);
        pairDiff.assign ((size_t) maxK * 3, 0.0);
        pairWeight.assign ((size_t) maxK * 3, 0.0);
        band.assign ((size_t) maxK * 5, 0.0);
        rhs.assign (maxK, 0.0);
        reset();
    }

    void reset() noexcept
    {
        std::fill (stored.begin(), stored.end(), 0.0);
        std::fill (sigma.begin(), sigma.end(), 0.0);
        std::fill (linked.begin(), linked.end(), 0);
        for (int k = 0; k < maxK; ++k) group[(size_t) k] = k;
        fifoHead = fifoCount = 0;
        reference = now = 0.0;
        lastSolve = -1.0;
        anything = false;
        louder = 0.0;
        confirmed = -1.0;
    }

    // One frame: its harmonics' levels in dB (index 1 to count), the noise next to each, its pitch and time. Fills gains[1..outCount] in dB.
    void frame (const double* levelDb, const double* noiseDb, int count, double f0In, double f0Out, double scale, double time, double* gains, int outCount, bool remember) noexcept
    {
        now = time;
        if (now - reference > 20.0) renormalise();
        decay = std::exp ((reference - now) / forgetSeconds);
        const int top = std::min ({ count, maxK - 1, (int) (topHz / f0In) });

        // A new vowel?
        if (anything && remember)
        {
            double sum = 0.0, sum2 = 0.0;
            int n = 0;
            for (int k = 1; k <= top; ++k)
            {
                double was;
                if (levelDb[k] - noiseDb[k] < checkSnrDb || ! value (k, binOf (f0In), was, 1.0)) continue;
                const double d = levelDb[k] - was;
                sum += d; sum2 += d * d; ++n;
            }
            if (n >= checkCount)
            {
                const double mean = sum / n, spread = std::sqrt (std::max (0.0, sum2 / n - mean * mean));
                louder = mean;
                confirmed = time;
                if (spread > changeDb)
                {
                    confirmed = -1.0;
                    louder = 0.0;
                    std::fill (stored.begin(), stored.end(), 0.0);
                    fifoCount = 0;
                    anything = false;
                    std::fill (linked.begin(), linked.end(), 0);
                }
            }
        }

        // Queue this frame, store the ones that have settled.
        if (remember)
        {
            Pending& p = fifo[(size_t) ((fifoHead + fifoCount) % fifoSize)];
            p.f0 = f0In; p.time = time; p.count = std::max (0, top);
            for (int k = 1; k <= p.count; ++k) p.level[(size_t) k] = (float) levelDb[k];
            if (fifoCount < fifoSize) ++fifoCount; else fifoHead = (fifoHead + 1) % fifoSize;
        }
        while (fifoCount > 0 && time - fifo[(size_t) fifoHead].time >= settleSeconds)
        {
            store (fifo[(size_t) fifoHead]);
            fifoHead = (fifoHead + 1) % fifoSize;
            --fifoCount;
        }
        if (anything && (lastSolve < 0.0 || time - lastSolve >= solveEverySeconds))
        {
            solve();
            lastSolve = time;
        }

        const double louderAtNewPitch = sourceLoudnessPerDb * std::max (0.0, 20.0 * std::log10 (std::max (1.0e-3, f0Out / f0In)));
        for (int k = 1; k <= outCount; ++k)
        {
            const double want = k * f0Out / scale;
            double then, here;
            bool found = false;
            // Both ends at the loudness they were stored at, so a voice getting louder or softer since does not count as a change of shape:
            // this pitch as stored, or this moment's level less how much louder this moment is than what was stored.
            // Only while this moment has just been checked against what was stored at its pitch and matched it.
            if (anything && time - confirmed <= confirmSeconds && k <= count && k < maxK && want <= topHz && k * f0In <= topHz)
            {
                if (! value (k, binOf (f0In), here, 0.2, true))
                    here = levelDb[k] - louder;
                if (value (k, binOf (want / k), then, 0.2, true))
                    found = true;
                else if (linked[(size_t) k] && borrowed (want, k, then))
                    found = true;
            }
            const double straight = lines (levelDb, count, f0In, want) - levelDb[std::min (k, count)];
            if (found)
            {
                // No vocal tract changes level faster than its sharpest resonance allows, so the memory may differ from the straight line
                // only by so much for how far the harmonic actually moves. A small correction cannot bring a big change.
                const double reach = reachDb + slopeDbPerHz * std::abs (want - k * f0In);
                gains[k] = std::clamp (then - here - louderAtNewPitch, straight - reach, straight + reach);
            }
            else
                gains[k] = straight;
        }
    }

    // Straight lines in dB through one moment's harmonics.
    static double lines (const double* levelDb, int count, double f0, double hz) noexcept
    {
        const double x = hz / f0;
        if (x <= 1.0 || count < 2) return levelDb[1];
        const int i = (int) x;
        if (i >= count) return levelDb[count];
        return levelDb[i] + (x - i) * (levelDb[i + 1] - levelDb[i]);
    }

private:
    struct Pending { double f0 = 0.0, time = 0.0; int count = 0; std::array<float, maxK> level {}; };

    static double binOf (double hz) noexcept { return 12.0 * binsPerSemi * std::log2 (std::max (hz, 1.0) / lowHz); }

    // Weights are kept multiplied by exp((now - reference) / forget), so fading needs no pass over the table.
    double weightAt (size_t at) const noexcept { return stored[at] * decay; }

    void renormalise() noexcept
    {
        const double shrink = std::exp ((reference - now) / forgetSeconds);
        for (auto& w : stored) w *= shrink;
        reference = now;
    }

    void store (const Pending& p) noexcept
    {
        const double b = binOf (p.f0);
        const int i = (int) std::floor (b);
        const double t = b - i;
        if (i < 0 || i + 1 >= bins || p.count < 1) return;
        decay = std::exp ((reference - now) / forgetSeconds);
        for (int k = 1; k <= p.count; ++k)
            for (int j = 0; j < 2; ++j)
            {
                const double a = j ? t : 1.0 - t;
                const size_t at = (size_t) (k * bins + i + j);
                const double w = weightAt (at) + a;
                level[at] += (float) ((p.level[(size_t) k] - level[at]) * a / std::max (1.0e-6, w));
                stored[at] = std::min (w, 4.0) / decay;
            }
        anything = true;
    }

    // Harmonic k's level at fractional bin b. At the edge of what was heard, the nearer bin alone when the lookup needs it.
    bool value (int k, double b, double& out, double minWeight, bool edge = false) const noexcept
    {
        const int i = (int) std::floor (b);
        const double t = b - i;
        if (i < 0 || i + 1 >= bins) return false;
        const size_t at = (size_t) (k * bins + i);
        const bool ok0 = weightAt (at) >= minWeight, ok1 = weightAt (at + 1) >= minWeight;
        if (ok0 && ok1) { out = (1.0 - t) * level[at] + t * level[at + 1]; return true; }
        if (edge && ok0 && t < 0.5) { out = level[at]; return true; }
        if (edge && ok1 && t >= 0.5) { out = level[at + 1]; return true; }
        return false;
    }

    // Harmonic k's level at frequency hz, from other harmonics heard there, with the difference in their own levels taken out.
    bool borrowed (double hz, int k, double& out) const noexcept
    {
        double sum = 0.0, weights = 0.0, v;
        for (int m = 1; m < maxK && m * lowHz <= hz; ++m)
        {
            if (m == k || ! linked[(size_t) m] || group[(size_t) m] != group[(size_t) k]) continue;
            if (value (m, binOf (hz / m), v, 0.2, true))
            {
                const double w = 1.0 / (1.0 + std::abs (m - k));
                sum += w * (v - sigma[(size_t) m]);
                weights += w;
            }
        }
        if (weights <= 0.0) return false;
        out = sum / weights + sigma[(size_t) k];
        return true;
    }

    // Each harmonic's own level against the first's, from where neighbours one or two numbers apart were heard at the same frequency:
    // least squares on sigma_k - sigma_m = mean difference, solved as a banded system.
    void solve() noexcept
    {
        int top = 1;
        for (int k = 2; k < maxK; ++k)
        {
            bool any = false;
            for (int i = 0; i < bins && ! any; ++i) any = weightAt ((size_t) (k * bins + i)) >= 0.5;
            if (any) top = k;
        }
        std::fill (pairDiff.begin(), pairDiff.end(), 0.0);
        std::fill (pairWeight.begin(), pairWeight.end(), 0.0);
        for (int k = 1; k < top; ++k)
            for (int step = 1; step <= 2 && k + step <= top; ++step)
            {
                const int m = k + step;
                const double offset = 12.0 * binsPerSemi * std::log2 ((double) k / m);
                double sum = 0.0, count = 0.0, a, b;
                for (int i = 0; i + 1 < bins; ++i)
                    if (value (k, i, a, 0.5) && value (m, i + offset, b, 0.5)) { sum += a - b; count += 1.0; }
                if (count == 0.0 && step == 1)
                {
                    // An octave apart at most, harmonics 1 and 2 only touch at the ends of a glide: compare the nearest ends if they are close.
                    int highK = -1, lowM = -1;
                    for (int i = 0; i < bins; ++i) if (weightAt ((size_t) (k * bins + i)) >= 0.5) highK = i;
                    for (int i = bins - 1; i >= 0; --i) if (weightAt ((size_t) (m * bins + i)) >= 0.5) lowM = i;
                    if (highK >= 0 && lowM >= 0 && std::abs ((lowM - offset) - highK) <= 0.5 * binsPerSemi)
                    {
                        sum = level[(size_t) (k * bins + highK)] - level[(size_t) (m * bins + lowM)];
                        count = 0.3;
                    }
                }
                if (count > 0.0)
                {
                    pairDiff[(size_t) (k * 3 + step)] = count >= 1.0 ? sum / count : sum;
                    pairWeight[(size_t) (k * 3 + step)] = std::min (count, 20.0);
                }
            }

        // Groups linked by any pair, and the banded normal equations with sigma_1 = 0.
        for (int k = 0; k < maxK; ++k) { group[(size_t) k] = k; linked[(size_t) k] = 0; }
        std::fill (band.begin(), band.end(), 0.0);
        std::fill (rhs.begin(), rhs.end(), 0.0);
        const auto at = [this] (int row, int col) -> double& { return band[(size_t) (row * 5 + (col - row + 2))]; };
        for (int k = 1; k < top; ++k)
            for (int step = 1; step <= 2 && k + step <= top; ++step)
            {
                const double w = pairWeight[(size_t) (k * 3 + step)], d = pairDiff[(size_t) (k * 3 + step)];
                if (w <= 0.0) continue;
                const int m = k + step;
                int a = root (k), b = root (m);
                if (a != b) group[(size_t) std::max (a, b)] = std::min (a, b);
                linked[(size_t) k] = linked[(size_t) m] = 1;
                if (k >= 2) { at (k, k) += w; rhs[(size_t) k] += w * d; }
                at (m, m) += w; rhs[(size_t) m] -= w * d;
                if (k >= 2) { at (k, m) -= w; at (m, k) -= w; }
            }
        for (int k = 1; k < maxK; ++k) group[(size_t) k] = root (k);
        for (int k = 2; k <= top; ++k) at (k, k) += 1.0e-6;
        for (int c = 2; c <= top; ++c)
        {
            const double pivot = at (c, c);
            if (std::abs (pivot) < 1.0e-12) continue;
            for (int r = c + 1; r <= std::min (top, c + 2); ++r)
            {
                const double f = at (r, c) / pivot;
                if (f == 0.0) continue;
                for (int q = c; q <= std::min (top, c + 2); ++q) at (r, q) -= f * at (c, q);
                rhs[(size_t) r] -= f * rhs[(size_t) c];
            }
        }
        for (int c = top; c >= 2; --c)
        {
            double v = rhs[(size_t) c];
            for (int q = c + 1; q <= std::min (top, c + 2); ++q) v -= at (c, q) * sigma[(size_t) q];
            const double pivot = at (c, c);
            sigma[(size_t) c] = std::abs (pivot) > 1.0e-12 ? v / pivot : 0.0;
        }
        sigma[1] = 0.0;
    }

    int root (int a) noexcept
    {
        while (group[(size_t) a] != a) a = group[(size_t) a] = group[(size_t) group[(size_t) a]];
        return a;
    }

    std::vector<float> level;
    std::vector<double> stored, sigma, pairDiff, pairWeight, band, rhs;
    std::vector<int> linked, group;
    std::vector<Pending> fifo;
    int fifoHead = 0, fifoCount = 0;
    double reference = 0.0, now = 0.0, decay = 1.0, lastSolve = -1.0;
    double louder = 0.0, confirmed = -1.0;   // dB this moment is over what was stored at its pitch, from the last moment that could tell
    bool anything = false;
};

class HarmonicShifter
{
public:
    static constexpr int interpolatorReach = 6;
    // Periods each side of a frame's centre that it gives out. Two on a steady voice, so the harmonics stay clean; one where the level
    // moves, so a note's start or end is not smeared out in time.
    static constexpr double steadyHalf = 2.0, movingHalf = 1.0, movingDb = 1.5;
    static constexpr int planAhead = 2;
    static constexpr double steadyMaxSeconds = 0.01;
    static constexpr int maxPeriods = 4, maxFrames = 8, maxQ = 4096, minQ = 32;

    struct Control
    {
        bool voiced = false;
        double inputPeriod = 0.0;    // samples, the tracker's
        double outputPeriod = 0.0;   // samples
        double formantScale = 1.0;   // above 1 moves formants up
        bool noCorrection = true;
    };

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        channels = std::clamp (numChannels, 1, 2);
        size = 1;
        while (size < (int) (sr * 0.5)) size *= 2;
        for (auto& b : input) b.assign ((size_t) size, 0.0f);
        phase.assign ((size_t) size, 0.0);
        marked.assign ((size_t) size, 0);
        for (auto& f : frames)
            for (auto& b : f.buffer) b.assign ((size_t) (maxPeriods * maxQ), 0.0f);
        work.assign ((size_t) (maxPeriods * maxQ), 0.0f);
        for (auto& r : real) r.assign ((size_t) (maxPeriods * maxQ / 2), 0.0f);
        for (auto& i : imag) i.assign ((size_t) (maxPeriods * maxQ / 2), 0.0f);
        outReal.assign ((size_t) (maxPeriods * maxQ / 2), 0.0f);
        outImag.assign ((size_t) (maxPeriods * maxQ / 2), 0.0f);
        const int harmonics = maxPeriods * maxQ / 2 / maxPeriods + 2;
        levels.assign ((size_t) harmonics, -200.0);
        noise.assign ((size_t) harmonics, -200.0);
        gains.assign ((size_t) harmonics + 8192, 0.0);
        smoothGains.assign (gains.size(), 0.0);
        for (int o = 0; o < (int) setups.size(); ++o)
        {
            if (setups[(size_t) o] != nullptr) vDSP_destroy_fftsetup (setups[(size_t) o]);
            setups[(size_t) o] = o >= 6 ? vDSP_create_fftsetup ((vDSP_Length) o, kFFTRadix2) : nullptr;
        }
        buildTable();
        fadeStep = 1.0 / (0.002 * sr);
        memory.prepare();
        reset();
    }

    ~HarmonicShifter()
    {
        for (auto* s : setups) if (s != nullptr) vDSP_destroy_fftsetup (s);
    }

    void setLatency (int samples) noexcept { latency = samples; }

    void reset() noexcept
    {
        for (auto& b : input) std::fill (b.begin(), b.end(), 0.0f);
        std::fill (phase.begin(), phase.end(), 0.0);
        std::fill (marked.begin(), marked.end(), 0);
        written = 0;
        filled = -1;
        fillPhase = 0.0;
        spacing = sr / 200.0;
        nextMark = 0;
        markBefore = -1.0;
        markSpacing = spacing;
        outPhase = 0.0;
        outPeriod = spacing;
        memory.reset();
        restart();
    }

    // Drops all frames. The next voiced sample starts again.
    void restart() noexcept
    {
        for (auto& f : frames) f.live = false;
        running = false;
        firstGain = true;
        // What was heard belongs to this voiced stretch only: after a gap the next sound may be anything.
        memory.reset();
        corrected = false;
    }

    void write (int channel, float x) noexcept { input[(size_t) channel][(size_t) (written & (size - 1))] = x; }

    // After the tracker has seen the newest sample: extends the input's phase, in periods, from the tracker's marks.
    void advance (const PitchTracker& tracker) noexcept
    {
        ++written;
        const long long newest = written - 1;
        // Marks are known about a third of a period late, so the phase is drawn up to a period and a little more behind the newest sample, where the next mark is surely known.
        const long long target = newest - (long long) std::lround (spacing) - 128;
        if (tracker.lastMark() + 1 < nextMark) { nextMark = 0; markBefore = -1.0; } // the tracker started again
        if (tracker.hasMarks() && nextMark <= tracker.lastMark())
        {
            // When the tracker finds a voice it marks back to where it began, over samples already drawn with the old spacing: draw them again.
            const double p = tracker.mark (std::max (nextMark, tracker.firstMark())).position;
            const long long oldest = written - size + 16;
            if (p < (double) filled && p >= (double) oldest)
            {
                filled = (long long) std::floor (p);
                fillPhase = phase[(size_t) (filled & (size - 1))];
                ++redrawn;
            }
        }
        while (filled < target)
        {
            const long long n = filled + 1;
            bool between = false;
            // Use the mark interval that holds n, when the tracker has one.
            while (tracker.hasMarks() && nextMark <= tracker.lastMark())
            {
                const long long q = std::max (nextMark, tracker.firstMark());
                const auto& m = tracker.mark (q);
                if (m.position < (double) n) { // this mark is behind: it starts the next interval
                    if (markBefore >= 0.0)
                    {
                        const double gap = m.position - markBefore;
                        markSpacing = (gap > 0.6 * m.period && gap < 1.6 * m.period) ? gap : m.period;
                    }
                    gapBefore = markBefore >= 0.0 ? m.position - markBefore : -1.0;
                    markBefore = m.position;
                    nextMark = q + 1;
                    continue;
                }
                // n lies before mark q: the interval from markBefore to it
                if (markBefore >= 0.0)
                {
                    const double gap = m.position - markBefore;
                    spacing = (gap > 0.6 * m.period && gap < 1.6 * m.period) ? gap : m.period;
                    // Marks wobble by a sample or so. Read straight, each period would be stretched a little differently, which the frames
                    // carry as a wobble of their own. So the interval is averaged with its neighbours, either side, which follows a steady
                    // glide without lag.
                    const auto usable = [&] (double g) { return g > 0.8 * spacing && g < 1.25 * spacing; };
                    const double after = q + 1 <= tracker.lastMark() ? tracker.mark (q + 1).position - m.position : -1.0;
                    if (usable (gapBefore) && usable (after))
                        spacing = 0.25 * (gapBefore + after) + 0.5 * spacing;
                }
                else
                    spacing = m.period;
                between = markBefore >= 0.0;
                break;
            }
            if (! tracker.hasMarks() || nextMark > tracker.lastMark())
                spacing = std::clamp (markSpacing, sr / 1400.0, sr / 50.0); // past the last mark: hold the last spacing
            spacing = std::clamp (spacing, sr / 1400.0, sr / 50.0);
            fillPhase += 1.0 / spacing;
            phase[(size_t) (n & (size - 1))] = fillPhase;
            marked[(size_t) (n & (size - 1))] = between ? 1 : 0;
            filled = n;
        }
    }

    // One output sample per channel, for the input sample latency behind the newest. Returns false when it has nothing to give,
    // and then the caller should use the input at that delay.
    bool step (const Control& c, const float* dryNow, float* out) noexcept
    {
        const long long now = written - 1 - latency;
        out[0] = out[1] = 0.0f;
        if (c.voiced && c.outputPeriod > 0.0)
            outPeriod = std::clamp (c.outputPeriod, sr / 1400.0, sr / 12.0);
        const double inStep = (double) now - 1.0 >= 0.0 && (double) now <= (double) filled ? phaseAt ((double) now) - phaseAt ((double) now - 1.0) : 1.0 / spacing;
        // Until this voiced stretch is first corrected the input itself is given out, so the phase follows the input's exactly: the hand
        // over then lands with the frames right on the input's timing. After that it moves at exactly the pitch asked for.
        const bool following = c.voiced && c.noCorrection && ! corrected && (double) now <= (double) filled && now >= 1;
        if (following)
            outPhase += std::clamp (inStep, 1.0 / (sr / 50.0), 1.0 / (sr / 1400.0));
        else
            outPhase += 1.0 / outPeriod;
        // How fast the output's phase moves against the input's: from the tracker's period, which is steady, rather than one sample's
        // step of the drawn phase, which jumps where the phase is drawn again.
        if (following)
            lastRatio = 1.0;
        else if (c.voiced && c.inputPeriod > 0.0)
            lastRatio = std::clamp (c.inputPeriod / outPeriod, 0.2, 5.0);
        if (c.voiced)
        {
            if (! c.noCorrection) corrected = true;
            if (! running)
            {
                running = true;
                nextFrame = (long long) std::floor (outPhase) - 1;
                segmentStart = nextFrame + 2;
                firstFrame = true;
            }
            while ((double) nextFrame - steadyHalf <= outPhase)
            {
                makeFrame (nextFrame, c, now);
                ++nextFrame;
            }
        }
        else if (! running)
            return false;
        double sum[2] = { 0.0, 0.0 }, weight = 0.0;
        for (auto& f : frames)
        {
            if (! f.live) continue;
            const double u = outPhase - (double) f.index;
            if (u >= f.half) { f.live = false; continue; }
            if (u <= -f.half) continue;
            // Read four periods for clean harmonics, and give out the middle four or two.
            const double analysis = 0.5 + 0.5 * std::cos (2.0 * M_PI * u / f.periods);
            double w = 0.5 + 0.5 * std::cos (M_PI * u / f.half);
            if (f.fade < 1.0)
            {
                f.fade -= fadeStep;
                if (f.fade <= 0.0) { f.live = false; continue; }
                w *= f.fade;
            }
            weight += w;
            const double pos = 0.5 * maxPeriods * f.q + u * f.q;
            const double reshape = w / std::max (analysis, 1.0e-6);
            for (int ch = 0; ch < channels; ++ch)
                sum[ch] += reshape * readFrame (f.buffer[(size_t) ch].data(), f.length, pos);
        }
        if (weight <= 0.0)
        {
            // Not voiced and the last frames have run out: start again at the next voiced sample.
            if (! c.voiced) restart();
            return false;
        }
        // Frames of different lengths overlap unevenly, so the sum is divided by its weight; only below half an overlap, where frames
        // start or run out, does the input fill the rest.
        if (weight >= 0.5)
        {
            out[0] = (float) (sum[0] / weight);
            out[1] = channels > 1 ? (float) (sum[1] / weight) : out[0];
        }
        else
        {
            const double fill = 1.0 - 2.0 * weight;
            out[0] = (float) (2.0 * sum[0] + fill * dryNow[0]);
            out[1] = channels > 1 ? (float) (2.0 * sum[1] + fill * dryNow[1]) : out[0];
        }
        return true;
    }

    // True once this voiced stretch has been corrected: its frames may sit up to half a period from the input's timing, so it should
    // not hand back to the input until the voice stops.
    bool hasCorrected() const noexcept { return corrected; }

private:
    struct Frame
    {
        bool live = false, trusted = false;
        double half = 1.0, fade = 1.0;   // fade below 1 counts down to 0 over a couple of ms, then the frame is dropped
        long long index = 0;
        int periods = 4, q = 64, length = 256;
        std::array<std::vector<float>, 2> buffer;
    };

    double phaseAt (double t) const noexcept
    {
        if (t <= (double) filled)
        {
            const long long i = (long long) std::floor (t);
            const double f = t - (double) i;
            const double a = phase[(size_t) (i & (size - 1))], b = phase[(size_t) ((i + 1) & (size - 1))];
            return i + 1 <= filled ? a + f * (b - a) : a + f / spacing;
        }
        return fillPhase + (t - (double) filled) / spacing;
    }

    // The time at which the input's phase is p, searching forwards from a sample index known to be at or before it.
    double timeAt (double p, long long& from) const noexcept
    {
        long long i = from;
        while (i + 1 <= filled && phase[(size_t) ((i + 1) & (size - 1))] <= p) ++i;
        from = i;
        if (i + 1 <= filled)
        {
            const double a = phase[(size_t) (i & (size - 1))], b = phase[(size_t) ((i + 1) & (size - 1))];
            return (double) i + (b > a ? (p - a) / (b - a) : 0.0);
        }
        return (double) filled + (p - fillPhase) * spacing;
    }

    long long searchStart (double p) const noexcept
    {
        // Phase steps are at least 1/maxPeriod a sample, so stepping back in big strides finds a start before p.
        long long i = std::min (filled, written - 1);
        const long long oldest = std::max (0LL, written - size + 8);
        while (i > oldest && phase[(size_t) (i & (size - 1))] > p)
            i = std::max (oldest, i - std::max (1LL, (long long) ((phase[(size_t) (i & (size - 1))] - p) * spacing * 0.5)));
        return i;
    }

    void makeFrame (long long j, const Control& c, long long now) noexcept
    {
        Frame* slot = nullptr;
        for (auto& f : frames) if (! f.live) { slot = &f; break; }
        if (slot == nullptr) return;

        // Where this frame will sit in the output, and the point of the input it starts from: one input period further on for each output
        // period, plus or minus whole periods to stay within half a period of the output's time.
        // The input's phase when the output's reaches j: exact while the ratio holds, so with nothing asked for the frames sit exactly on the input.
        // Anchors are decided two frames ahead, so a frame knows when one of its neighbours jumps a whole input period and can give out a
        // shorter stretch: where the input is read a period apart, frames overlapping it would partly cancel.
        // After the phase was drawn again its numbers mean something else: plan afresh.
        if (! firstFrame && (planEpoch != redrawn || lastIndex < j - 1 || lastIndex > j + planAhead))
        {
            firstFrame = true;
            // Frames made before the phase was drawn again read another stretch of input than the ones to come: drop them, the input fills in.
            if (planEpoch != redrawn)
                for (auto& f : frames) if (f.live && f.fade >= 1.0) f.fade = 0.999;
        }
        if (firstFrame)
        {
            planEpoch = redrawn;
            for (long long i = j; i <= j + planAhead; ++i)
            {
                planned[(size_t) (i & 7)] = phaseAt ((double) now) + ((double) i - outPhase) / std::max (lastRatio, 0.05);
                slipped[(size_t) (i & 7)] = 0;
            }
            lastIndex = j + planAhead;
            lastAnchor = planned[(size_t) (lastIndex & 7)];
        }
        while (lastIndex < j + planAhead)
        {
            const long long i = lastIndex + 1;
            const double wanted = phaseAt ((double) now) + ((double) i - outPhase) / std::max (lastRatio, 0.05);
            const double ahead = lastAnchor + 1.0;
            const double anchorHere = ahead + std::round (wanted - ahead);
            planned[(size_t) (i & 7)] = anchorHere;
            slipped[(size_t) (i & 7)] = anchorHere != ahead ? 1 : 0;
            lastAnchor = anchorHere;
            lastIndex = i;
        }
        firstFrame = false;
        const double anchor = planned[(size_t) (j & 7)];
        bool nearSlip = false;
        for (long long i = j - planAhead + 1; i <= j + planAhead; ++i)
            nearSlip = nearSlip || slipped[(size_t) (i & 7)] != 0;

        long long search = searchStart (anchor - 0.5 * maxPeriods - 1.0);
        long long probe = search;
        const double before = timeAt (anchor - 0.5, probe), after = timeAt (anchor + 0.5, probe);
        const double inPeriod = std::clamp (after - before, sr / 1400.0, sr / 50.0);
        // Before the marks for this stretch arrive, the phase carries on at an old spacing. Such a frame would move the voice by the
        // wrong amount, so it is left out and the input fills in.
        if (c.inputPeriod > 0.0 && std::abs (std::log (inPeriod / c.inputPeriod)) > 0.2)
            return;
        const long long newest = written - 1;
        int periods = maxPeriods;
        while (periods > 2)
        {
            long long p2 = search;
            if (timeAt (anchor + 0.5 * periods, p2) + interpolatorReach + 1 <= (double) newest) break;
            --periods;
        }
        int q = minQ;
        while (q < maxQ && q < 1.25 * std::max (inPeriod, outPeriod)) q *= 2;
        const int length = maxPeriods * q;
        int order = 0;
        while ((1 << order) < length) ++order;
        FFTSetup setup = setups[(size_t) order];
        if (setup == nullptr) return;

        // Read the input at q samples per period, windowed over the frame's periods, into the FFT for each channel.
        const int half = length / 2;
        double earlier = 1.0e-12, later = 1.0e-12;
        for (int ch = 0; ch < channels; ++ch)
        {
            long long walk = search;
            for (int i = 0; i < length; ++i)
            {
                const double u = (double) (i - half) / q;
                if (std::abs (u) >= 0.5 * periods) { work[(size_t) i] = 0.0f; continue; }
                const double t = timeAt (anchor + u, walk);
                const double w = 0.5 + 0.5 * std::cos (2.0 * M_PI * u / periods);
                const float x = readInput (ch, t);
                work[(size_t) i] = (float) (w * x);
                if (ch == 0) (i < half ? earlier : later) += (double) x * x * w;
            }
            DSPSplitComplex split { real[(size_t) ch].data(), imag[(size_t) ch].data() };
            vDSP_ctoz ((const DSPComplex*) work.data(), 2, &split, 1, (vDSP_Length) length / 2);
            vDSP_fft_zrip (setup, &split, 1, (vDSP_Length) order, kFFTDirection_Forward);
        }

        // Harmonic k sits on bin k * maxPeriods. Levels from all channels together, in dB against the frame's size.
        const double f0In = sr / inPeriod, f0Out = sr / outPeriod;
        const int count = std::min (q / 2 - 1, (int) (0.47 * sr / f0In));
        const double norm = 20.0 * std::log10 ((double) periods * q);
        const auto power = [this] (int bin)
        {
            double s = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                s += (double) real[(size_t) ch][(size_t) bin] * real[(size_t) ch][(size_t) bin] + (double) imag[(size_t) ch][(size_t) bin] * imag[(size_t) ch][(size_t) bin];
            return s;
        };
        for (int k = 1; k <= count; ++k)
            levels[(size_t) k] = 10.0 * std::log10 (power (k * maxPeriods) + 1.0e-30) - norm;
        // The noise between harmonics: a window over four periods puts no harmonic energy half way between two.
        for (int k = 1; k <= count; ++k)
        {
            double s = 0.0;
            int n = 0;
            for (int h = std::max (1, k - 2); h <= std::min (count, k + 2); ++h)
                for (int side : { -maxPeriods / 2, maxPeriods / 2 })
                {
                    const int bin = h * maxPeriods + side;
                    if (bin > 0 && bin < length / 2) { s += power (bin); ++n; }
                }
            noise[(size_t) k] = 10.0 * std::log10 (s / std::max (1, n) + 1.0e-30) - norm;
        }
        // Only frames read wholly from the tracker's marks are remembered: before the marks arrive the pitch is a guess.
        bool trusted = true;
        {
            long long w = search;
            for (double u : { -0.5 * periods, 0.0, 0.5 * periods })
            {
                const double t = timeAt (anchor + u, w);
                const long long i = (long long) t;
                if (t > (double) filled || ! marked[(size_t) (i & (size - 1))]) trusted = false;
            }
        }
        const int outCount = std::max (1, (int) (0.47 * sr / f0Out));
        const double scale = std::clamp (c.formantScale, 0.4, 2.5);
        if (count >= 1)
        {
            const int n = std::min (outCount, (int) gains.size() - 1);
            // Only steady frames are remembered: one read while a note swells or fades would later stand for a level the voice no longer has.
            const bool steady = std::abs (10.0 * std::log10 (later / earlier)) < movingDb;
            memory.frame (levels.data(), noise.data(), count, f0In, f0Out, scale, (double) now / sr, gains.data(), n, trusted && steady);
            // Gains glide from frame to frame: a gain that jumps between frames would ring as sidebands below the voice.
            const double dt = std::max (0.0, (double) (now - lastFrameTime) / sr);
            const double follow = firstGain ? 1.0 : 1.0 - std::exp (-dt / gainSeconds);
            for (int k = 1; k <= n; ++k)
            {
                smoothGains[(size_t) k] += follow * (gains[(size_t) k] - smoothGains[(size_t) k]);
                gains[(size_t) k] = smoothGains[(size_t) k];
            }
            firstGain = false;
            lastFrameTime = now;
        }

        // Each output harmonic: its own, scaled, or when too weak next to the noise, the input harmonic now nearest where it is going.
        for (int ch = 0; ch < channels; ++ch)
        {
            const float* re = real[(size_t) ch].data();
            const float* im = imag[(size_t) ch].data();
            std::fill (outReal.begin(), outReal.begin() + length / 2, 0.0f);
            std::fill (outImag.begin(), outImag.begin() + length / 2, 0.0f);
            for (int k = 1; k <= outCount && count >= 1; ++k)
            {
                int from = k;
                double gainDb;
                if (k <= count && levels[(size_t) k] - noise[(size_t) k] >= trustDb)
                    gainDb = gains[(size_t) k];
                else
                {
                    // Too weak to trust: the level of the harmonic now where this one is going. Its own bins keep their phase, so frames
                    // still line up where a harmonic hovers around the noise; only past the input's top are another harmonic's bins borrowed.
                    const double target = k * f0Out / scale;
                    const int nearest = std::clamp ((int) std::lround (target / f0In), 1, count);
                    const double wantedDb = TractMemory::lines (levels.data(), count, f0In, target);
                    if (k <= count)
                        gainDb = std::min (wantedDb - levels[(size_t) k], weakBoostDb);
                    else
                    {
                        from = nearest;
                        gainDb = wantedDb - levels[(size_t) from];
                    }
                }
                const float g = (float) std::pow (10.0, std::clamp (gainDb, -40.0, 24.0) / 20.0);
                for (int d = -maxPeriods / 2; d < maxPeriods / 2; ++d)
                {
                    const int to = k * maxPeriods + d, source = from * maxPeriods + d;
                    if (to <= 0 || to >= length / 2 || source <= 0 || source >= length / 2) continue;
                    outReal[(size_t) to] = re[source] * g;
                    outImag[(size_t) to] = im[source] * g;
                }
            }
            for (int bin = 1; bin < maxPeriods / 2; ++bin) { outReal[(size_t) bin] = re[bin]; outImag[(size_t) bin] = im[bin]; }
            outReal[0] = re[0];
            outImag[0] = 0.0f;
            DSPSplitComplex split { outReal.data(), outImag.data() };
            vDSP_fft_zrip (setup, &split, 1, (vDSP_Length) order, kFFTDirection_Inverse);
            vDSP_ztoc (&split, 1, (DSPComplex*) slot->buffer[(size_t) ch].data(), 2, (vDSP_Length) length / 2);
            const float back = 1.0f / (2.0f * (float) length);
            vDSP_vsmul (slot->buffer[(size_t) ch].data(), 1, &back, slot->buffer[(size_t) ch].data(), 1, (vDSP_Length) length);
        }
        slot->live = true;
        slot->fade = 1.0;
        slot->trusted = trusted;
        // Long stretches only where they stay short in time: two periods a side is 20 ms on a 100 Hz voice, enough to smear a syllable.
        const bool shortPeriods = steadyHalf * std::max (inPeriod, outPeriod) <= steadyMaxSeconds * sr;
        slot->half = (shortPeriods && ! nearSlip && j - segmentStart >= 2 && periods >= 4 && std::abs (10.0 * std::log10 (later / earlier)) < movingDb) ? steadyHalf : movingHalf;
        slot->index = j;
        slot->periods = periods;
        slot->q = q;
        slot->length = length;
    }

    float readInput (int ch, double t) const noexcept
    {
        const double whole = std::floor (t);
        const long long i = (long long) whole;
        const float* row = tableRow (t - whole);
        const float* x = input[(size_t) ch].data();
        const int mask = size - 1;
        float total = 0.0f;
        for (int k = 0; k < tapCount; ++k)
            total += x[(size_t) ((i - (tapCount / 2 - 1) + k) & mask)] * row[k];
        return total;
    }

    float readFrame (const float* b, int length, double pos) const noexcept
    {
        const double whole = std::floor (pos);
        const int i = (int) whole;
        const float* row = tableRow (pos - whole);
        float total = 0.0f;
        for (int k = 0; k < tapCount; ++k)
        {
            const int at = i - (tapCount / 2 - 1) + k;
            if (at >= 0 && at < length) total += b[at] * row[k];
        }
        return total;
    }

    const float* tableRow (double fraction) const noexcept
    {
        const int p = std::clamp ((int) std::lround (fraction * phases), 0, phases);
        return table.data() + p * tapCount;
    }

    void buildTable()
    {
        table.assign ((size_t) (phases + 1) * tapCount, 0.0f);
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
                table[(size_t) (p * tapCount + t)] = (float) (row[(size_t) t] / total);
        }
    }

    static constexpr int tapCount = 12, phases = 1024;
    static constexpr double trustDb = 10.0;   // a harmonic this far over the noise between harmonics keeps its own level
    // A weaker one is raised at most this much towards the level it should have. On a glide the harmonics move within a frame, so high ones
    // spill into the bins between them and read as weak though they are not. At 12 dB a big shift down could not lift them far enough to
    // where they land: e at 165 Hz down 7 lost F4 entirely, 20 dB under a real voice, and big shifts read 6.7% against 8. At 24, 1.1%.
    static constexpr double weakBoostDb = 24.0;

    double sr = 48000.0;
    int channels = 2, size = 32768, latency = 0;
    long long written = 0, filled = -1, nextMark = 0, nextFrame = 0, lastIndex = 0;
    double fillPhase = 0.0, spacing = 240.0, markBefore = -1.0, markSpacing = 240.0, gapBefore = -1.0;
    double outPhase = 0.0, outPeriod = 240.0, lastAnchor = 0.0, lastRatio = 1.0;
    std::array<double, 8> planned {};   // anchors decided ahead, by frame index modulo 8
    std::array<std::uint8_t, 8> slipped {};
    long long redrawn = 0, planEpoch = 0;
    double fadeStep = 0.01;
    bool running = false, firstFrame = true, corrected = false;
    std::array<std::vector<float>, 2> input;
    std::vector<double> phase;
    std::vector<std::uint8_t> marked;   // 1 where the phase came from two of the tracker's marks
    std::array<Frame, maxFrames> frames;
    std::vector<float> work, outReal, outImag, table;
    std::array<std::vector<float>, 2> real, imag;
    std::vector<double> levels, noise, gains, smoothGains;
    long long lastFrameTime = 0, segmentStart = 0;
    bool firstGain = true;
    static constexpr double gainSeconds = 0.008;
    std::array<FFTSetup, 16> setups {};
    TractMemory memory;
};
} // namespace smoothtune
