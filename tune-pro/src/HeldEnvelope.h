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
// Real time safe: fixed size arrays, no allocation.
#include <algorithm>
#include <array>
#include <cmath>

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
};

class FormantFit
{
public:
    static constexpr int maxH = 32, gridN = 256, lowPasses = 5, maxMinima = 32, formants = 5;
    static constexpr double gridLowHz = 150.0, gridHighHz = 5600.0, minGapHz = 150.0;

    Settings settings;
    struct Shape { std::array<int, formants> j {}; int lp = 0; double cost = 1.0e30; };

    FormantFit()
    {
        for (int j = 0; j < gridN; ++j) gridHz[(size_t) j] = gridLowHz * std::pow (gridHighHz / gridLowHz, (double) j / (gridN - 1));
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
    bool fit (const double* lev, const double* noise, int K, double f0, double sampleRate) noexcept
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
        for (int j = 0; j < gridN; ++j)
        {
            const double F = gridHz[(size_t) j], B = bandwidth (F);
            for (int h = 1; h <= n; ++h) table[(size_t) j][(size_t) h] = resonatorDb (h * f0, F, B, sr);
        }
        for (int l = 0; l < lowPasses; ++l)
            for (int h = 1; h <= n; ++h) lowTable[(size_t) l][(size_t) h] = lowPassDb (h * f0, lowPassHz[(size_t) l], sr);

        for (int m = 0; m < previousCount; ++m)
        {
            Shape s = previous[(size_t) m];
            s.cost = cost (s);
            descend (s);
            keep (s);
        }
        // Starting shapes across the vowel space, each walked downhill.
        static constexpr double starts1[] = { 280.0, 400.0, 550.0, 750.0, 950.0 };
        static constexpr double starts2[] = { 900.0, 1300.0, 1800.0, 2300.0 };
        for (double f1 : starts1)
            for (double f2 : starts2)
            {
                if (f2 < f1 + 300.0) continue;
                Shape s;
                s.j = { index (f1), index (f2), index (std::max (2600.0, f2 + 400.0)), index (3500.0), index (4500.0) };
                if (s.j[3] <= s.j[2]) s.j[3] = std::min (gridN - 2, s.j[2] + 10);
                if (s.j[4] <= s.j[3]) s.j[4] = std::min (gridN - 1, s.j[3] + 10);
                s.lp = 0;
                s.cost = cost (s);
                descend (s);
                keep (s);
            }
        return minima > 0;
    }

    // Vocal tract change for output harmonic k: from k * fin to k * fout, averaged over the shapes found.
    void gains (double fin, double fout, int K, double* g) const noexcept
    {
        double best = 1.0e30;
        for (int m = 0; m < minima; ++m) best = std::min (best, found[(size_t) m].cost);
        std::array<double, maxMinima> w {};
        double wsum = 0.0;
        for (int m = 0; m < minima; ++m)
        {
            w[(size_t) m] = std::exp (-(found[(size_t) m].cost - best) / (2.0 * settings.spreadDb2));
            wsum += w[(size_t) m];
        }
        for (int k = 1; k <= K; ++k) g[k] = 0.0;
        for (int m = 0; m < minima; ++m)
        {
            if (w[(size_t) m] < 1.0e-4 * wsum) continue;
            const Shape& s = found[(size_t) m];
            for (int k = 1; k <= K; ++k) g[k] += w[(size_t) m] / wsum * (tractDb (s, k * fout) - tractDb (s, k * fin));
        }
    }

    const Shape& bestShape() const noexcept
    {
        int b = 0;
        for (int m = 1; m < minima; ++m) if (found[(size_t) m].cost < found[(size_t) b].cost) b = m;
        return found[(size_t) b];
    }
    double hzOf (int j) const noexcept { return gridHz[(size_t) j]; }
    double lowPassOf (int l) const noexcept { return lowPassHz[(size_t) l]; }
    int count() const noexcept { return minima; }

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

    void descend (Shape& s) const noexcept
    {
        for (int step : { 8, 4, 2, 1 })
        {
            bool moved = true;
            int guard = 0;
            while (moved && ++guard < 64)
            {
                moved = false;
                for (int i = 0; i < formants; ++i)
                    for (int dir : { -1, 1 })
                    {
                        Shape t = s;
                        t.j[(size_t) i] += dir * step;
                        const double c = cost (t);
                        if (c < s.cost) { t.cost = c; s = t; moved = true; }
                    }
                for (int l = 0; l < lowPasses; ++l)
                {
                    if (l == s.lp) continue;
                    Shape t = s;
                    t.lp = l;
                    const double c = cost (t);
                    if (c < s.cost) { t.cost = c; s = t; moved = true; }
                }
            }
        }
    }

    void keep (const Shape& s) noexcept
    {
        if (s.cost >= 1.0e29) return;
        for (int m = 0; m < minima; ++m)
            if (found[(size_t) m].j == s.j && found[(size_t) m].lp == s.lp) return;
        if (minima < maxMinima) found[(size_t) minima++] = s;
    }

    std::array<double, gridN> gridHz {};
    std::array<double, lowPasses> lowPassHz { 0.0, 1500.0, 2200.0, 3200.0, 4500.0 };
    std::array<std::array<double, maxH + 1>, gridN> table {};
    std::array<std::array<double, maxH + 1>, lowPasses> lowTable {};
    std::array<double, maxH + 1> db {}, x {}, cap {};
    std::array<bool, maxH + 1> good {};
    std::array<Shape, maxMinima> found {}, previous {};
    int n = 0, minima = 0, previousCount = 0;
    double sr = 48000.0;
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
    static constexpr double averageSeconds = 0.04, refitSeconds = 0.02, moveSemitones = 0.5, gapSeconds = 0.1;
    static constexpr double glideSeconds = 0.05;   // the change follows a new fit over this, so a fit that wavers does not ring
    static constexpr int maxH = FormantFit::maxH;

    FormantFit fitter;

    void reset() noexcept { have = false; ok = false; fresh = true; smoothCount = 0; }

    // Fills gains[1..outCount] with tract(k f0Out / scale) - tract(k f0In) in dB. Returns false when there is no fit to give, and then
    // gains are left alone.
    bool frame (const double* lev, const double* noise, int count, double f0In, double f0Out, double scale, double time, double* gains, int outCount) noexcept
    {
        const double topHz = fitter.settings.topHz;
        if (count < 3 || topHz / f0In > maxH) { reset(); return false; }
        const int top = std::min (count, maxH);
        const bool moved = have && std::abs (12.0 * std::log2 (f0In / avgF0)) > moveSemitones;
        if (! have || moved || time - lastTime > gapSeconds || time < lastTime)
        {
            for (int k = 1; k <= top; ++k) { avg[(size_t) k] = std::pow (10.0, 0.1 * lev[k]); avgNoise[(size_t) k] = std::pow (10.0, 0.1 * noise[k]); }
            avgF0 = f0In;
            avgTop = top;
            have = true;
            lastFit = -1.0;
            fresh = true;
        }
        else
        {
            const double a = 1.0 - std::exp (-(time - lastTime) / averageSeconds);
            for (int k = 1; k <= top; ++k)
            {
                const double p = std::pow (10.0, 0.1 * lev[k]), q = std::pow (10.0, 0.1 * noise[k]);
                if (k > avgTop) { avg[(size_t) k] = p; avgNoise[(size_t) k] = q; }
                else { avg[(size_t) k] += a * (p - avg[(size_t) k]); avgNoise[(size_t) k] += a * (q - avgNoise[(size_t) k]); }
            }
            avgTop = top;
            avgF0 += a * (f0In - avgF0);
        }
        const double dt = time - lastTime;
        lastTime = time;
        if (lastFit < 0.0 || time - lastFit >= refitSeconds)
        {
            for (int k = 1; k <= avgTop; ++k) { avgDb[(size_t) k] = 10.0 * std::log10 (avg[(size_t) k] + 1.0e-30); avgNoiseDb[(size_t) k] = 10.0 * std::log10 (avgNoise[(size_t) k] + 1.0e-30); }
            ok = fitter.fit (avgDb.data(), avgNoiseDb.data(), avgTop, avgF0, sampleRate);
            lastFit = time;
        }
        if (! ok) { fresh = true; smoothCount = 0; return false; }
        const double want = f0Out / std::max (scale, 1.0e-3);
        int inside = 0;
        while (inside + 1 <= outCount && (inside + 1) * std::max (want, f0In) <= topHz && inside + 1 <= count) ++inside;
        if (inside >= 1) fitter.gains (f0In, want, inside, gains);
        const double follow = fresh ? 1.0 : 1.0 - std::exp (-std::max (0.0, dt) / glideSeconds);
        for (int k = 1; k <= std::min (inside, maxOut); ++k)
        {
            if (k > smoothCount) smooth[(size_t) k] = gains[k];
            smooth[(size_t) k] += follow * (gains[k] - smooth[(size_t) k]);
            gains[k] = smooth[(size_t) k];
        }
        smoothCount = std::min (inside, maxOut);
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
    bool fresh = true;
    std::array<double, maxH + 1> avg {}, avgNoise {}, avgDb {}, avgNoiseDb {};
    double avgF0 = 0.0, lastTime = 0.0, lastFit = -1.0;
    int avgTop = 0;
    bool have = false, ok = false;
};
} // namespace heldenv
