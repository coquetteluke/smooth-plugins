#pragma once
// The vocal tract between one moment's harmonics, for the harmonic shifter on a held note.
//
// Why: on a dead steady note nothing sweeps the harmonics, so where a resonance sits between two harmonics its height is not in the
// input. Straight lines through the harmonics miss it, and on high notes, where harmonics are far apart, that cost about 4 dB per
// harmonic against a real voice. A vocal tract is a chain of resonances, each with a bandwidth that real voices keep within a known
// range for where it sits, so the levels that are heard say a lot about where the peaks between them must be.
//
// How: the input's harmonic levels are fitted as voice source plus vocal tract. The source is a straight slope in dB against the log of
// the harmonic number, held near a typical voice's slope; it stays with each harmonic number in the shifter, so its own ripple from one
// harmonic to the next does no harm. The tract is five resonances with bandwidths from their frequency, plus one gentle low pass for a
// darker voice. A harmonic lost in the noise only says the tract is no louder than the noise there. The fit starts from a spread of vowel
// shapes and walks downhill from each, and the shapes it settles on are averaged by how well each fits, so where two shapes explain the
// harmonics about equally, the change is a blend of both rather than a bet on one.
//
// When: only while the pitch holds (HeldTract below). Where the pitch moves, the shifter's memory of what each harmonic was heard at
// does better, and the fit fades out. Levels are averaged over 40 ms, refitted every 40 ms from the last shapes and every 200 ms from
// all the starting shapes, and the change glides over 100 ms. A new vowel on the same pitch starts the average again. Full strength up
// to 7 semitones of shift, none from 12. Assumes a voice whose glottis closes in a usual way: the slope prior is a textbook voice's.
//
// Measured with true pitch in tests/prototype/HeldLab.cpp and in the engine with tests/prototype/EngineHarness.cpp (rand, styled,
// heldvowel), on dev voices only.
//
// Real time safe: fixed size arrays, no allocation.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace heldenv
{
struct Settings
{
    double topHz = 5000.0;          // harmonics fitted up to here
    double snrNeedDb = 3.0;         // a harmonic this far over the noise between harmonics counts as heard
    double boundMarginDb = 3.0;     // one that is not may still be this far over the noise
    double tiltDb = -6.0;           // source slope, dB per doubling of harmonic number, that the fit is held near: the usual -12 dB of the glottal flow plus 6 from the lips
    double tiltWeight = 5.0;
    double bandwidthScale = 1.0;
    double spreadDb2 = 3.0;         // how much worse, in squared dB summed over harmonics, a shape may fit and still count
    int highStarts = 3;             // how many placings of the fourth and fifth resonance each start is tried with
};

class FormantFit
{
public:
    static constexpr int maxH = 32, gridN = 256, lowPasses = 5, maxMinima = 32, formants = 5;
    static constexpr double gridLowHz = 150.0, gridHighHz = 5600.0, minGapHz = 150.0;

    Settings settings;
    struct Shape { std::array<int, formants> j {}; int lp = 0, bw = 1; double cost = 1.0e30; };
    struct Trig
    {
        double c1 = 1.0, s1 = 0.0, c2 = 1.0, s2 = 0.0;
        void set (double w) noexcept { c1 = std::cos (w); s1 = std::sin (w); c2 = c1 * c1 - s1 * s1; s2 = 2.0 * s1 * c1; }
    };
    // A shape's filters, worked out once a fit, so the change for each harmonic costs no exponentials or cosines. power() is log10 of
    // the tract's power gain.
    struct Coefficients
    {
        std::array<double, formants> c {}, rr {}, b0 {};
        double a = 0.0;
        double power (const Trig& t) const noexcept
        {
            double num = 1.0, den = 1.0;
            for (int i = 0; i < formants; ++i)
            {
                const double re = 1.0 - c[(size_t) i] * t.c1 + rr[(size_t) i] * t.c2, im = c[(size_t) i] * t.s1 - rr[(size_t) i] * t.s2;
                num *= b0[(size_t) i] * b0[(size_t) i];
                den *= re * re + im * im;
            }
            if (a > 0.0)
            {
                const double re = 1.0 - a * t.c1, im = a * t.s1;
                num *= (1.0 - a) * (1.0 - a);
                den *= re * re + im * im;
            }
            return std::log10 (num / std::max (den, 1.0e-300));
        }
        // The first resonance alone.
        double powerOne (const Trig& t) const noexcept
        {
            const double re = 1.0 - c[0] * t.c1 + rr[0] * t.c2, im = c[0] * t.s1 - rr[0] * t.s2;
            return std::log10 (b0[0] * b0[0] / std::max (re * re + im * im, 1.0e-300));
        }
    };

    FormantFit()
    {
        for (int j = 0; j < gridN; ++j) gridHz[(size_t) j] = gridLowHz * std::pow (gridHighHz / gridLowHz, (double) j / (gridN - 1));
        costs.resize ((size_t) costSlots);
        descents.resize ((size_t) descentSlots);
    }

    double bandwidth (double hz) const noexcept
    {
        return settings.bandwidthScale * (hz < 1000.0 ? 60.0 + 0.03 * hz : 50.0 + 0.04 * hz);
    }

    static double resonatorDb (double f, double F, double B, double sr) noexcept
    {
        const double r = std::exp (-M_PI * B / sr), c = 2.0 * r * std::cos (2.0 * M_PI * F / sr);
        const double w = 2.0 * M_PI * f / sr;
        const double re = 1.0 - c * std::cos (w) + r * r * std::cos (2.0 * w), im = c * std::sin (w) - r * r * std::sin (2.0 * w);
        return 20.0 * std::log10 ((1.0 - c + r * r) / std::sqrt (re * re + im * im));
    }
    static double lowPassDb (double f, double fc, double sr) noexcept
    {
        if (fc <= 0.0) return 0.0;
        const double a = std::exp (-2.0 * M_PI * fc / sr), w = 2.0 * M_PI * f / sr;
        const double re = 1.0 - a * std::cos (w), im = a * std::sin (w);
        return 20.0 * std::log10 ((1.0 - a) / std::sqrt (re * re + im * im));
    }

    // lev[1..K], noise[1..K] in dB at k * f0. Returns false when there is too little to fit.
    // full: also walk down from every starting shape, not only from the shapes found last time.
    bool fit (const double* lev, const double* noise, int K, double f0, double sampleRate, bool full = true) noexcept
    {
        sr = sampleRate;
        n = 0;
        int heard = 0;
        for (int k = 1; k <= K && k <= maxH && k * f0 <= settings.topHz; ++k)
        {
            db[(size_t) k] = lev[k];
            x[(size_t) k] = std::log2 ((double) k);
            good[(size_t) k] = lev[k] - noise[k] >= settings.snrNeedDb;
            cap[(size_t) k] = std::max (lev[k], noise[k] + settings.boundMarginDb);
            heard += good[(size_t) k] ? 1 : 0;
            n = k;
        }
        // The shapes found last time start again first, so a steady voice keeps the same answer from one fit to the next.
        previousCount = minima;
        for (int m = 0; m < minima; ++m) previous[(size_t) m] = found[(size_t) m];
        minima = 0;
        if (heard < 3) return false;
        // The sums over the heard harmonics that every shape's cost shares, in the order cost() always added them.
        fitSx = fitSxx = fitCnt = 0.0;
        for (int h = 1; h <= n; ++h)
        {
            if (! good[(size_t) h]) continue;
            fitSx += x[(size_t) h]; fitSxx += x[(size_t) h] * x[(size_t) h]; fitCnt += 1.0;
        }
        // What was worked out for the last fit belongs to its levels: start both caches again.
        if (++generation >= (1u << generationBits)) { generation = 1; for (auto& c : costs) c.tag = 0; for (auto& d : descents) d.tag = 0; }
        // Each resonance's level at each harmonic, for every place on the grid. Kept while the pitch holds within 0.2%: a harmonic
        // misplaced by that is well inside any resonance's bandwidth.
        if (n != tableCount || sr != tableRate || settings.bandwidthScale != tableScale || std::abs (f0 / tableF0 - 1.0) > 0.002)
        {
            std::array<Trig, maxH + 1> trig {};
            for (int h = 1; h <= n; ++h) trig[(size_t) h].set (2.0 * M_PI * h * f0 / sr);
            for (int j = 0; j < gridN; ++j)
            {
                const double F = gridHz[(size_t) j], B = bandwidth (F), r = std::exp (-M_PI * B / sr);
                Coefficients one;
                one.c[0] = 2.0 * r * std::cos (2.0 * M_PI * F / sr);
                one.rr[0] = r * r;
                one.b0[0] = 1.0 - one.c[0] + r * r;
                for (int h = 1; h <= n; ++h)
                    table[(size_t) j][(size_t) h] = 10.0 * one.powerOne (trig[(size_t) h]);
            }
            for (int l = 0; l < lowPasses; ++l)
                for (int h = 1; h <= n; ++h) lowTable[(size_t) l][(size_t) h] = lowPassDb (h * f0, lowPassHz[(size_t) l], sr);
            tableCount = n; tableRate = sr; tableScale = settings.bandwidthScale; tableF0 = f0;
        }

        for (int m = 0; m < previousCount; ++m)
        {
            Shape s = previous[(size_t) m];
            s.cost = cachedCost (s);
            descend (s);
            keep (s);
        }
        // Starting shapes across the vowel space, each walked downhill.
        static constexpr double starts1[] = { 250.0, 320.0, 400.0, 480.0, 570.0, 670.0, 780.0, 900.0 };
        static constexpr double starts2[] = { 800.0, 1000.0, 1250.0, 1550.0, 1900.0, 2300.0 };
        static constexpr double startsHigh[][2] = { { 3500.0, 4500.0 }, { 3300.0, 4200.0 }, { 3900.0, 4900.0 } };
        const int highs = (full || previousCount == 0) ? std::clamp (settings.highStarts, 1, 3) : 0;
        for (int hi = 0; hi < highs; ++hi)
            for (double f1 : starts1)
                for (double f2 : starts2)
                {
                    if (f2 < f1 + 250.0) continue;
                    Shape s;
                    s.j = { index (f1), index (f2), index (std::max (2600.0, f2 + 400.0)), index (startsHigh[hi][0]), index (startsHigh[hi][1]) };
                    if (s.j[3] <= s.j[2] + 8) s.j[3] = std::min (gridN - 2, s.j[2] + 10);
                    if (s.j[4] <= s.j[3] + 8) s.j[4] = std::min (gridN - 1, s.j[3] + 10);
                    s.lp = 0;
                    s.cost = cachedCost (s);
                    descend (s);
                    keep (s);
                }
        for (int m = 0; m < minima; ++m)
        {
            const Shape& sh = found[(size_t) m];
            Coefficients& co = coefficients[(size_t) m];
            for (int i = 0; i < formants; ++i)
            {
                const double F = gridHz[(size_t) sh.j[(size_t) i]], B = bandwidth (F);
                const double r = std::exp (-M_PI * B / sr);
                co.c[(size_t) i] = 2.0 * r * std::cos (2.0 * M_PI * F / sr);
                co.rr[(size_t) i] = r * r;
                co.b0[(size_t) i] = 1.0 - co.c[(size_t) i] + r * r;
            }
            co.a = lowPassHz[(size_t) sh.lp] > 0.0 ? std::exp (-2.0 * M_PI * lowPassHz[(size_t) sh.lp] / sr) : 0.0;
        }
        return minima > 0;
    }

    // Vocal tract change for output harmonic k: from k * fin to k * fout, averaged over the shapes found.
    void gains (double fin, double fout, int K, double* g) const noexcept
    {
        double best = 1.0e30;
        for (int m = 0; m < minima; ++m) best = std::min (best, found[(size_t) m].cost);
        std::array<int, maxMinima> use {};
        std::array<double, maxMinima> w {};
        int used = 0;
        double wsum = 0.0;
        for (int m = 0; m < minima; ++m)
        {
            const double weight = std::exp (-(found[(size_t) m].cost - best) / (2.0 * settings.spreadDb2));
            if (weight < 1.0e-3) continue;
            use[(size_t) used] = m;
            w[(size_t) used++] = weight;
            wsum += weight;
        }
        for (int k = 1; k <= K; ++k)
        {
            Trig at, to;
            at.set (2.0 * M_PI * k * fin / sr);
            to.set (2.0 * M_PI * k * fout / sr);
            double sum = 0.0;
            for (int u = 0; u < used; ++u)
            {
                const Coefficients& c = coefficients[(size_t) use[(size_t) u]];
                sum += w[(size_t) u] * (c.power (to) - c.power (at));
            }
            g[k] = 10.0 * sum / wsum;
        }
    }

    // Tests: walk and cost exactly as up to 1.8.1 (no caches, cost() as it was written), to prove the faster fit gives the same answer.
    bool asBefore = false;
    long shapesCostedDifferently (int count) const noexcept
    {
        long differ = 0;
        unsigned r = 12345u;
        for (int k = 0; k < count; ++k)
        {
            Shape t;
            for (int i = 0; i < formants; ++i) { r = r * 1103515245u + 12345u; t.j[(size_t) i] = (int) ((r >> 8) % gridN); }
            std::sort (t.j.begin(), t.j.end());
            r = r * 1103515245u + 12345u;
            t.lp = (int) ((r >> 8) % lowPasses);
            const double a = cost (t), b = costAsBefore (t);
            if (std::memcmp (&a, &b, sizeof a) != 0) ++differ;
        }
        return differ;
    }
    const Shape& shapeFound (int m) const noexcept { return found[(size_t) m]; }

    const Shape& bestShape() const noexcept
    {
        int b = 0;
        for (int m = 1; m < minima; ++m) if (found[(size_t) m].cost < found[(size_t) b].cost) b = m;
        return found[(size_t) b];
    }
    double hzOf (int j) const noexcept { return gridHz[(size_t) j]; }
    double lowPassOf (int l) const noexcept { return lowPassHz[(size_t) l]; }
    int count() const noexcept { return minima; }
    void forget() noexcept { minima = 0; }

    double tractDb (const Shape& s, double f) const noexcept
    {
        double t = lowPassDb (f, lowPassHz[(size_t) s.lp], sr);
        for (int i = 0; i < formants; ++i) t += resonatorDb (f, gridHz[(size_t) s.j[(size_t) i]], bandwidth (gridHz[(size_t) s.j[(size_t) i]]), sr);
        return t;
    }

private:
    int index (double hz) const noexcept
    {
        const double t = std::log (hz / gridLowHz) / std::log (gridHighHz / gridLowHz) * (gridN - 1);
        return std::clamp ((int) std::lround (t), 0, gridN - 1);
    }

    bool valid (const Shape& s) const noexcept
    {
        for (int i = 0; i < formants; ++i) if (s.j[(size_t) i] < 0 || s.j[(size_t) i] >= gridN) return false;
        for (int i = 1; i < formants; ++i) if (gridHz[(size_t) s.j[(size_t) i]] < gridHz[(size_t) s.j[(size_t) i - 1]] + minGapHz) return false;
        return true;
    }

    // Least squares over the heard harmonics for the source's level and slope, the slope held near the typical one, plus a penalty for
    // any unheard harmonic the shape would make louder than the noise allows.
    double cost (const Shape& s) const noexcept
    {
        if (! valid (s)) return 1.0e30;
        // The same operations as up to 1.8.1, in the same order, so the same answer to the last bit; laid out so the processor can do the
        // per harmonic parts side by side (Phase 2, 2026-11-13). sx, sxx and cnt do not depend on the shape: the same sums in the same
        // order are worked out once a fit (fit()). Only the sums that run across the harmonics stay one after the other.
        const double* lo = lowTable[(size_t) s.lp].data();
        const double* r0 = table[(size_t) s.j[0]].data();
        const double* r1 = table[(size_t) s.j[1]].data();
        const double* r2 = table[(size_t) s.j[2]].data();
        const double* r3 = table[(size_t) s.j[3]].data();
        const double* r4 = table[(size_t) s.j[4]].data();
        double y[maxH + 1];
        for (int h = 1; h <= n; ++h)
        {
            double m = lo[h];
            m += r0[h]; m += r1[h]; m += r2[h]; m += r3[h]; m += r4[h];
            y[h] = db[(size_t) h] - m;
        }
        double sy = 0.0, sxy = 0.0;
        for (int h = 1; h <= n; ++h)
        {
            if (! good[(size_t) h]) continue;
            sy += y[h]; sxy += x[(size_t) h] * y[h];
        }
        const double sx = fitSx, sxx = fitSxx, cnt = fitCnt;
        const double tw = settings.tiltWeight, t0 = settings.tiltDb;
        const double a11 = cnt, a12 = sx, a22 = sxx + tw, b1 = sy, b2 = sxy + tw * t0;
        const double det = a11 * a22 - a12 * a12;
        const double slope = std::clamp ((a11 * b2 - a12 * b1) / std::max (1.0e-9, det), -24.0, 6.0);
        const double level = (sy - slope * sx) / std::max (1.0, cnt);
        double e[maxH + 1];
        for (int h = 1; h <= n; ++h)
            e[h] = y[h] - level - slope * x[(size_t) h];
        double c = tw * (slope - t0) * (slope - t0);
        for (int h = 1; h <= n; ++h)
        {
            if (good[(size_t) h]) c += e[h] * e[h];
            else
            {
                const double over = db[(size_t) h] - e[h] - cap[(size_t) h];
                if (over > 0.0) c += over * over;
            }
        }
        return c;
    }

    // cost() as it was written up to 1.8.1, kept to prove the one above gives the same answer (PluginTest fitexact).
    double costAsBefore (const Shape& s) const noexcept
    {
        if (! valid (s)) return 1.0e30;
        std::array<double, maxH + 1> y {};
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, cnt = 0.0;
        for (int h = 1; h <= n; ++h)
        {
            double m = lowTable[(size_t) s.lp][(size_t) h];
            for (int i = 0; i < formants; ++i) m += table[(size_t) s.j[(size_t) i]][(size_t) h];
            y[(size_t) h] = db[(size_t) h] - m;
            if (! good[(size_t) h]) continue;
            sx += x[(size_t) h]; sy += y[(size_t) h]; sxx += x[(size_t) h] * x[(size_t) h]; sxy += x[(size_t) h] * y[(size_t) h]; cnt += 1.0;
        }
        const double tw = settings.tiltWeight, t0 = settings.tiltDb;
        const double a11 = cnt, a12 = sx, a22 = sxx + tw, b1 = sy, b2 = sxy + tw * t0;
        const double det = a11 * a22 - a12 * a12;
        const double slope = std::clamp ((a11 * b2 - a12 * b1) / std::max (1.0e-9, det), -24.0, 6.0);
        const double level = (sy - slope * sx) / std::max (1.0, cnt);
        double c = tw * (slope - t0) * (slope - t0);
        for (int h = 1; h <= n; ++h)
        {
            const double e = y[(size_t) h] - level - slope * x[(size_t) h];
            if (good[(size_t) h]) c += e * e;
            else
            {
                const double over = db[(size_t) h] - e - cap[(size_t) h];
                if (over > 0.0) c += over * over;
            }
        }
        return c;
    }

    // Walks downhill from s. The same walk as up to 1.8.1, step for step, with the same answer; only work already done in this fit is not
    // done again (Phase 2, 2026-11-13). A full fit walked from about 120 starting shapes and costed up to 34,000 shapes, half of them more
    // than once, and the walks from different starts run into each other after a few moves: that one fit took about 0.8 ms inside one
    // output sample (AuBench's worst single block 37% of a 128 sample block, in High Quality on a held note). Now each shape is costed
    // once a fit (cachedCost), and a walk that reaches a place another walk of this fit has already gone on from, at the same step size
    // and with as many moves left, takes that walk's end at once: from there the rest of the walk is the same whichever way it came.
    void descend (Shape& s) noexcept
    {
        static constexpr int steps[4] = { 8, 4, 2, 1 };
        struct Visit { std::uint64_t key; int stepIndex, guard; };
        std::array<Visit, 4 * 64> path {};
        std::array<int, 4> lastGuard { 0, 0, 0, 0 };
        std::array<bool, 4> cut { false, false, false, false };
        int visits = 0, jumpStep = -1;
        bool jumpClean = true;
        for (int stepIndex = 0; stepIndex < 4 && jumpStep < 0; ++stepIndex)
        {
            const int step = steps[stepIndex];
            bool moved = true;
            int guard = 0;
            while (moved && ++guard < 64)
            {
                const std::uint64_t here = keyOf (s);
                if (const auto* d = asBefore ? nullptr : findDescent (here, stepIndex); d != nullptr && guard + d->moves - 1 < 64)
                {
                    // The walk from here was walked before in this fit and ended normally within the guard it has left.
                    s = d->result;
                    lastGuard[(size_t) stepIndex] = guard + d->moves - 1;
                    jumpStep = stepIndex;
                    jumpClean = true;
                    moved = false;
                    break;
                }
                path[(size_t) visits++] = { here, stepIndex, guard };
                lastGuard[(size_t) stepIndex] = guard;
                moved = false;
                for (int i = 0; i < formants; ++i)
                    for (int dir : { -1, 1 })
                    {
                        Shape t = s;
                        t.j[(size_t) i] += dir * step;
                        const double c = cachedCost (t);
                        if (c < s.cost) { t.cost = c; s = t; moved = true; }
                    }
                for (int l = 0; l < lowPasses; ++l)
                {
                    if (l == s.lp) continue;
                    Shape t = s;
                    t.lp = l;
                    const double c = cachedCost (t);
                    if (c < s.cost) { t.cost = c; s = t; moved = true; }
                }
            }
            if (moved) cut[(size_t) stepIndex] = true;   // stopped by the guard: what follows depends on how it got here
        }
        // Every place this walk went on from, with where the walk ended and how many moves of its own step it took from there.
        bool cleanAfter = jumpClean;
        int v = visits - 1;
        for (int stepIndex = jumpStep >= 0 ? jumpStep : 3; stepIndex >= 0; --stepIndex)
        {
            cleanAfter = cleanAfter && ! cut[(size_t) stepIndex];
            for (; v >= 0 && path[(size_t) v].stepIndex == stepIndex; --v)
                if (cleanAfter && ! asBefore)
                    storeDescent (path[(size_t) v].key, stepIndex, lastGuard[(size_t) stepIndex] - path[(size_t) v].guard + 1, s);
        }
    }

    static std::uint64_t keyOf (const Shape& s) noexcept
    {
        std::uint64_t k = 0;
        for (int i = 0; i < formants; ++i) k = (k << 8) | (std::uint64_t) (std::uint8_t) s.j[(size_t) i];
        return (k << 3) | (std::uint64_t) s.lp;
    }
    static std::uint64_t slotOf (std::uint64_t key) noexcept { return (key * 0x9E3779B97F4A7C15ull) >> 40; }

    // cost(), worked out once per shape per fit. A shape off the grid is never kept (its key would not be its own).
    double cachedCost (const Shape& s) noexcept
    {
        if (asBefore) return costAsBefore (s);
        if (! valid (s)) return 1.0e30;
        const std::uint64_t key = keyOf (s), tag = (key << generationBits) | generation;
        std::uint64_t at = slotOf (key) & (costSlots - 1);
        for (int probe = 0; probe < 8; ++probe, at = (at + 1) & (costSlots - 1))
        {
            auto& slot = costs[(size_t) at];
            if (slot.tag == tag) return slot.cost;
            if ((slot.tag & generationMask) != generation)
            {
                const double c = cost (s);
                slot.tag = tag;
                slot.cost = c;
                return c;
            }
        }
        return cost (s);
    }

    struct Descent { std::uint64_t tag = 0; int moves = 0; Shape result; };
    const Descent* findDescent (std::uint64_t key, int stepIndex) const noexcept
    {
        const std::uint64_t full = (key << 2) | (std::uint64_t) stepIndex, tag = (full << generationBits) | generation;
        std::uint64_t at = slotOf (full) & (descentSlots - 1);
        for (int probe = 0; probe < 8; ++probe, at = (at + 1) & (descentSlots - 1))
        {
            const auto& d = descents[(size_t) at];
            if (d.tag == tag) return &d;
            if ((d.tag & generationMask) != generation) return nullptr;
        }
        return nullptr;
    }
    void storeDescent (std::uint64_t key, int stepIndex, int moves, const Shape& result) noexcept
    {
        const std::uint64_t full = (key << 2) | (std::uint64_t) stepIndex, tag = (full << generationBits) | generation;
        std::uint64_t at = slotOf (full) & (descentSlots - 1);
        for (int probe = 0; probe < 8; ++probe, at = (at + 1) & (descentSlots - 1))
        {
            auto& d = descents[(size_t) at];
            if (d.tag == tag) return;
            if ((d.tag & generationMask) != generation) { d.tag = tag; d.moves = moves; d.result = result; return; }
        }
    }

    void keep (const Shape& s) noexcept
    {
        if (s.cost >= 1.0e29) return;
        for (int m = 0; m < minima; ++m)
            if (found[(size_t) m].j == s.j && found[(size_t) m].lp == s.lp) return;
        if (minima < maxMinima) { found[(size_t) minima++] = s; return; }
        int worst = 0;
        for (int m = 1; m < minima; ++m) if (found[(size_t) m].cost > found[(size_t) worst].cost) worst = m;
        if (s.cost < found[(size_t) worst].cost) found[(size_t) worst] = s;
    }

    std::array<double, gridN> gridHz {};
    std::array<double, lowPasses> lowPassHz { 0.0, 1500.0, 2200.0, 3200.0, 4500.0 };
    std::array<std::array<double, maxH + 1>, gridN> table {};
    int tableCount = -1;
    double tableRate = 0.0, tableScale = 0.0, tableF0 = 1.0;
    std::array<std::array<double, maxH + 1>, lowPasses> lowTable {};
    std::array<double, maxH + 1> db {}, x {}, cap {};
    std::array<bool, maxH + 1> good {};
    std::array<Shape, maxMinima> found {}, previous {};
    std::array<Coefficients, maxMinima> coefficients {};
    int n = 0, minima = 0, previousCount = 0;
    double sr = 48000.0;
    // The caches. A key is 43 bits (five grid places and a low pass), 45 with the step size; the generation fills the rest of the tag.
    static constexpr int generationBits = 18, costSlots = 1 << 15, descentSlots = 1 << 13;
    double fitSx = 0.0, fitSxx = 0.0, fitCnt = 0.0;
    static constexpr std::uint64_t generationMask = (std::uint64_t (1) << generationBits) - 1;
    struct CostSlot { std::uint64_t tag = 0; double cost = 0.0; };
    std::vector<CostSlot> costs;
    std::vector<Descent> descents;
    std::uint64_t generation = 0;
};

// Straight lines in dB through the harmonics, flat outside them.
inline double linesAt (const double* lev, int K, double x)
{
    if (x <= 1.0 || K < 2) return lev[1];
    const int i = (int) x;
    if (i >= K) return lev[K];
    return lev[i] + (x - i) * (lev[i + 1] - lev[i]);
}

// What the harmonic shifter asks of the fit, frame by frame: levels averaged over a few periods while the pitch holds, so the noise
// between harmonics does not shake the fit, refitted every refitSeconds, and the change for each output harmonic from the shapes found.
class HeldTract
{
public:
    static constexpr double averageSeconds = 0.04, refitSeconds = 0.04, fullSeconds = 0.2, moveSemitones = 0.5, gapSeconds = 0.1;
    static constexpr double glideSeconds = 0.1;   // the change follows a new fit over this, so a fit that wavers does not ring
    // Only on a held pitch: where the pitch moves, the memory of what was heard does better, so the fit fades in only while the pitch
    // stays within steadySemitones of where it has been over the last steadySeconds, and fades out again over blendSeconds.
    static constexpr double steadySeconds = 0.05, steadySemitones = 0.1, blendSeconds = 0.05;
    static constexpr double clearDb = 15.0, reshapeDb = 6.0;  // a frame whose clear harmonics differ in shape from the average by this starts it again
    static constexpr int maxH = FormantFit::maxH;

    FormantFit fitter;

    void reset() noexcept { have = false; ok = false; fresh = true; smoothCount = 0; blend = 0.0; pitchKnown = false; lastFit = lastFull = -1.0; lastTime = 0.0; fitter.forget(); }

    // Fills gains[1..outCount] with tract(k f0Out / scale) - tract(k f0In) in dB, blended with straight lines while the pitch settles.
    // Returns false when it has nothing to add, and then gains are left alone.
    bool frame (const double* lev, const double* noise, int count, double f0In, double f0Out, double scale, double time, double* gains, int outCount) noexcept
    {
        const double topHz = fitter.settings.topHz;
        if (count < 3 || topHz / f0In > maxH) { reset(); return false; }
        const int top = std::min (count, maxH);
        const double dt = time - lastTime;
        const bool gap = dt > gapSeconds || dt < 0.0;
        const bool moved = have && std::abs (12.0 * std::log2 (f0In / avgF0)) > moveSemitones;
        // A new vowel on the same pitch: the harmonics that stand clear of the noise no longer match their average in shape.
        bool reshaped = false;
        if (have && ! moved && ! gap)
        {
            double sum = 0.0, sum2 = 0.0;
            int n = 0;
            for (int k = 1; k <= std::min (top, avgTop); ++k)
            {
                if (lev[k] - noise[k] < clearDb) continue;
                const double d = lev[k] - 10.0 * std::log10 (avg[(size_t) k] + 1.0e-30);
                sum += d; sum2 += d * d; ++n;
            }
            if (n >= 3) reshaped = std::sqrt (std::max (0.0, sum2 / n - (sum / n) * (sum / n))) > reshapeDb;
        }
        if (! have || moved || gap || reshaped)
        {
            for (int k = 1; k <= top; ++k) { avg[(size_t) k] = std::pow (10.0, 0.1 * lev[k]); avgNoise[(size_t) k] = std::pow (10.0, 0.1 * noise[k]); }
            avgF0 = f0In;
            avgTop = top;
            have = true;
            lastFit = -1.0;
            if (reshaped) lastFull = -1.0;
            fresh = true;
        }
        else
        {
            const double a = 1.0 - std::exp (-dt / averageSeconds);
            for (int k = 1; k <= top; ++k)
            {
                const double p = std::pow (10.0, 0.1 * lev[k]), q = std::pow (10.0, 0.1 * noise[k]);
                if (k > avgTop) { avg[(size_t) k] = p; avgNoise[(size_t) k] = q; }
                else { avg[(size_t) k] += a * (p - avg[(size_t) k]); avgNoise[(size_t) k] += a * (q - avgNoise[(size_t) k]); }
            }
            avgTop = top;
            avgF0 += a * (f0In - avgF0);
        }
        lastTime = time;

        // Is the pitch held?
        const double here = std::log2 (f0In);
        if (! pitchKnown || gap) { slowPitch = here; pitchKnown = true; blend = 0.0; }
        else slowPitch += (1.0 - std::exp (-std::max (0.0, dt) / steadySeconds)) * (here - slowPitch);
        const bool steady = std::abs (12.0 * (here - slowPitch)) < steadySemitones;
        // Big shifts carry the fit far from the harmonics it was fitted on: full up to 7 semitones, none from 12.
        const double want = f0Out / std::max (scale, 1.0e-3);
        const double reach = std::clamp ((12.0 - std::abs (12.0 * std::log2 (want / f0In))) / 5.0, 0.0, 1.0);
        if (gap) blend = 0.0;
        else blend += ((steady ? reach : 0.0) - blend) * (1.0 - std::exp (-std::max (0.0, dt) / blendSeconds));
        if (blend < 0.01) { fresh = true; return false; }

        if (lastFit < 0.0 || time - lastFit >= refitSeconds || time < lastFit)
        {
            for (int k = 1; k <= avgTop; ++k) { avgDb[(size_t) k] = 10.0 * std::log10 (avg[(size_t) k] + 1.0e-30); avgNoiseDb[(size_t) k] = 10.0 * std::log10 (avgNoise[(size_t) k] + 1.0e-30); }
            const bool full = lastFull < 0.0 || time - lastFull >= fullSeconds || time < lastFull;
            ok = fitter.fit (avgDb.data(), avgNoiseDb.data(), avgTop, avgF0, sampleRate, full);
            lastFit = time;
            if (full) lastFull = time;
        }
        if (! ok) { fresh = true; return false; }
        int inside = 0;
        while (inside + 1 <= std::min (outCount, maxOut) && (inside + 1) * std::max (want, f0In) <= topHz && inside + 1 <= count) ++inside;
        if (inside >= 1) fitter.gains (f0In, want, inside, gains);
        const double follow = fresh ? 1.0 : 1.0 - std::exp (-std::max (0.0, dt) / glideSeconds);
        for (int k = 1; k <= inside; ++k)
        {
            if (k > smoothCount) smooth[(size_t) k] = gains[k];
            smooth[(size_t) k] += follow * (gains[k] - smooth[(size_t) k]);
            const double straight = linesAt (lev, count, k * want / f0In) - lev[k];
            gains[k] = blend * smooth[(size_t) k] + (1.0 - blend) * straight;
        }
        smoothCount = inside;
        fresh = false;
        for (int k = inside + 1; k <= outCount; ++k)
            gains[k] = linesAt (lev, count, k * want / f0In) - lev[std::min (k, count)];
        return true;
    }

    double sampleRate = 48000.0;

private:
    static constexpr int maxOut = 64;
    std::array<double, maxOut + 1> smooth {};
    int smoothCount = 0;
    bool fresh = true, pitchKnown = false;
    std::array<double, maxH + 1> avg {}, avgNoise {}, avgDb {}, avgNoiseDb {};
    double avgF0 = 0.0, lastTime = 0.0, lastFit = -1.0, lastFull = -1.0, slowPitch = 0.0, blend = 0.0;
    int avgTop = 0;
    bool have = false, ok = false;
};
} // namespace heldenv
