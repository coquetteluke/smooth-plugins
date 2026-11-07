#pragma once
// Measuring tools for the tests. None of this shares code with the plug-in, so a bug in one does not hide in the other.
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace measure
{
inline double median (std::vector<double> v)
{
    if (v.empty()) return std::nan ("");
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}
inline double percentile (std::vector<double> v, double p)
{
    if (v.empty()) return std::nan ("");
    std::sort (v.begin(), v.end());
    return v[std::min (v.size() - 1, (size_t) (p * 0.01 * (double) v.size()))];
}
inline std::vector<double> absolute (std::vector<double> v) { for (auto& x : v) x = std::abs (x); return v; }
inline double cents (double a, double b) { return 1200.0 * std::log2 (a / b); }

// Period near an expected one at a position, by full rate normalised correlation. Returns Hz, or 0 if there is nothing periodic.
inline double pitchNear (const float* x, long n, long centre, double expectedHz, double sr, double searchFraction = 0.06, double windowPeriods = 3.0, double minWindowSec = 0.0)
{
    const double T = sr / expectedHz;
    const int W = (int) std::max (windowPeriods * T, minWindowSec * sr);
    const int lo = std::max (2, (int) std::floor (T * (1.0 - searchFraction)));
    const int hi = (int) std::ceil (T * (1.0 + searchFraction)) + 1;
    const long start = centre - (W + hi) / 2;
    if (start < 0 || start + W + hi + 2 >= n) return 0.0;
    const float* a = x + start;
    double ea = 0;
    for (int i = 0; i < W; ++i) ea += a[i] * a[i];
    std::vector<double> r ((size_t) (hi + 2), -2.0);
    double best = -2.0; int at = lo;
    for (int lag = lo - 1; lag <= hi + 1; ++lag)
    {
        double c = 0, eb = 0;
        for (int i = 0; i < W; ++i) { c += a[i] * a[i + lag]; eb += a[i + lag] * a[i + lag]; }
        const double den = std::sqrt (ea * eb);
        const double v = den > 1.0e-12 ? c / den : 0.0;
        r[(size_t) lag] = v;
        if (lag >= lo && lag <= hi && v > best) { best = v; at = lag; }
    }
    if (best < 0.5 || at == lo || at == hi) return 0.0;
    const double a0 = r[(size_t) at - 1], b0 = r[(size_t) at], c0 = r[(size_t) at + 1];
    const double den = a0 - 2.0 * b0 + c0;
    const double lag = at + (std::abs (den) > 1.0e-12 ? 0.5 * (a0 - c0) / den : 0.0);
    return sr / lag;
}

// Cents off an expected pitch, one value per frame.
inline std::vector<double> centsOff (const float* x, long n, double sr, double expectedHz, double fromSec, double toSec, double hopSec = 0.005, double search = 0.06)
{
    std::vector<double> out;
    for (double t = fromSec; t < toSec; t += hopSec)
    {
        const double hz = pitchNear (x, n, (long) (t * sr), expectedHz, sr, search);
        if (hz > 0.0) out.push_back (cents (hz, expectedHz));
    }
    return out;
}

// Sliding pitch around an expected value, used for time constants and steps. Short window for time resolution.
inline std::vector<double> pitchTrace (const float* x, long n, double sr, double expectedHz, double fromSec, double toSec, double hopSec, double search = 0.15, double windowPeriods = 2.5)
{
    std::vector<double> out;
    for (double t = fromSec; t < toSec; t += hopSec)
        out.push_back (pitchNear (x, n, (long) (t * sr), expectedHz, sr, search, windowPeriods));
    return out;
}

// Size of a sine in a pitch trace, by correlating with sine and cosine. Trace is in cents at a fixed hop.
inline double sineDepth (const std::vector<double>& trace, double hopSec, double hz)
{
    double re = 0, im = 0, mean = 0;
    int count = 0;
    for (double v : trace) if (std::isfinite (v)) { mean += v; ++count; }
    if (count == 0) return 0.0;
    mean /= count;
    // Whole cycles only
    const int cycleFrames = (int) std::lround (1.0 / (hz * hopSec));
    const int use = (int) (trace.size() / (size_t) cycleFrames) * cycleFrames;
    int used = 0;
    for (int i = 0; i < use; ++i)
    {
        if (! std::isfinite (trace[(size_t) i])) continue;
        const double ph = 2.0 * M_PI * hz * hopSec * i;
        re += (trace[(size_t) i] - mean) * std::cos (ph);
        im += (trace[(size_t) i] - mean) * std::sin (ph);
        ++used;
    }
    return used > 0 ? 2.0 * std::sqrt (re * re + im * im) / used : 0.0;
}

// Formants by linear prediction on a 16 kHz copy. Returns the peaks of the envelope in Hz, in order.
inline std::vector<double> formants (const float* x, long n, double sr, int order = 18)
{
    // Averaged low pass and drop to about 16 kHz
    const int drop = std::max (1, (int) std::lround (sr / 16000.0));
    const double rate = sr / drop;
    std::vector<double> y;
    y.reserve ((size_t) (n / drop));
    for (long i = 0; i + drop <= n; i += drop)
    {
        double s = 0; for (int k = 0; k < drop; ++k) s += x[i + k];
        y.push_back (s / drop);
    }
    // Pre emphasis flattens the source so the peaks are the formants.
    for (size_t i = y.size(); i-- > 1;) y[i] -= 0.9 * y[i - 1];
    const size_t m = y.size();
    if (m < 512) return {};

    // Autocorrelation over overlapping Hann windows, so several periods go into each.
    const size_t win = std::min<size_t> (m, 1024);
    std::vector<double> ac ((size_t) order + 1, 0.0);
    for (size_t start = 0; start + win <= m; start += win / 2)
    {
        std::vector<double> w (win);
        for (size_t i = 0; i < win; ++i) w[i] = y[start + i] * (0.5 - 0.5 * std::cos (2.0 * M_PI * (double) i / (double) (win - 1)));
        for (int lag = 0; lag <= order; ++lag)
            for (size_t i = (size_t) lag; i < win; ++i)
                ac[(size_t) lag] += w[i] * w[i - (size_t) lag];
    }
    if (ac[0] <= 0.0) return {};
    ac[0] *= 1.0001;

    // Levinson-Durbin
    std::vector<double> a ((size_t) order + 1, 0.0), tmp ((size_t) order + 1, 0.0);
    a[0] = 1.0;
    double err = ac[0];
    for (int i = 1; i <= order; ++i)
    {
        double acc = ac[(size_t) i];
        for (int j = 1; j < i; ++j) acc += a[(size_t) j] * ac[(size_t) (i - j)];
        const double k = -acc / err;
        tmp = a;
        for (int j = 1; j < i; ++j) a[(size_t) j] = tmp[(size_t) j] + k * tmp[(size_t) (i - j)];
        a[(size_t) i] = k;
        err *= (1.0 - k * k);
    }

    // Envelope on a fine grid
    constexpr int grid = 2048;
    std::vector<double> env (grid + 1);
    for (int i = 0; i <= grid; ++i)
    {
        const double w = M_PI * i / grid;
        double re = 0, im = 0;
        for (int k = 0; k <= order; ++k) { re += a[(size_t) k] * std::cos (k * w); im -= a[(size_t) k] * std::sin (k * w); }
        env[(size_t) i] = -10.0 * std::log10 (re * re + im * im + 1.0e-30);
    }
    std::vector<double> peaks;
    for (int i = 2; i < grid - 1; ++i)
        if (env[(size_t) i] > env[(size_t) i - 1] && env[(size_t) i] >= env[(size_t) i + 1])
            peaks.push_back (rate * 0.5 * i / grid);
    return peaks;
}

// Peaks of the long term average spectrum, on material whose pitch glides, so the harmonics fill in the shape of the vocal tract.
inline std::vector<double> spectrumPeaks (const float* x, long n, double sr)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> power (size / 2 + 1, 0.0);
    std::vector<float> buf ((size_t) size * 2);
    for (long at = 0; at + size <= n; at += size / 4)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[at + i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k <= size / 2; ++k)
            power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    // An all pole fit to that averaged spectrum gives a smooth outline, free of ripple from the harmonics.
    const double binHz = sr / size;
    constexpr double topHz = 6000.0;
    const int top = (int) (topHz / binHz);
    constexpr int fitOrder = 16;
    std::vector<double> ac ((size_t) fitOrder + 1, 0.0);
    for (int lag = 0; lag <= fitOrder; ++lag)
        for (int k = 1; k <= top; ++k)
            ac[(size_t) lag] += power[(size_t) k] * std::cos (M_PI * lag * k / top);
    if (ac[0] <= 0.0) return {};
    ac[0] *= 1.00001;
    std::vector<double> a ((size_t) fitOrder + 1, 0.0), tmp ((size_t) fitOrder + 1, 0.0);
    a[0] = 1.0;
    double err = ac[0];
    for (int i = 1; i <= fitOrder; ++i)
    {
        double acc = ac[(size_t) i];
        for (int j = 1; j < i; ++j) acc += a[(size_t) j] * ac[(size_t) (i - j)];
        const double r = -acc / err;
        tmp = a;
        for (int j = 1; j < i; ++j) a[(size_t) j] = tmp[(size_t) j] + r * tmp[(size_t) (i - j)];
        a[(size_t) i] = r;
        err *= (1.0 - r * r);
    }
    constexpr int grid = 3000;
    std::vector<double> env (grid + 1);
    for (int i = 0; i <= grid; ++i)
    {
        const double w = M_PI * i / grid;
        double re = 0, im = 0;
        for (int k = 0; k <= fitOrder; ++k) { re += a[(size_t) k] * std::cos (k * w); im -= a[(size_t) k] * std::sin (k * w); }
        env[(size_t) i] = -10.0 * std::log10 (re * re + im * im + 1.0e-30);
    }
    std::vector<double> peaks;
    for (int i = 2; i < grid - 1; ++i)
        if (env[(size_t) i] > env[(size_t) i - 1] && env[(size_t) i] >= env[(size_t) i + 1])
            peaks.push_back (topHz * i / grid);
    return peaks;
}

// The averaged spectrum in dB, one value per Hz from 0 up, smoothed over a width in Hz.
inline std::vector<double> smoothSpectrumDb (const float* x, long n, double sr, double smoothHz)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> power (size / 2 + 1, 0.0);
    std::vector<float> buf ((size_t) size * 2);
    for (long at = 0; at + size <= n; at += size / 4)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[at + i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k <= size / 2; ++k)
            power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    const double binHz = sr / size;
    const int reach = std::max (1, (int) std::lround (0.5 * smoothHz / binHz));
    std::vector<double> smooth (power.size(), 0.0);
    for (int k = 0; k < (int) power.size(); ++k)
    {
        double s = 0; int c = 0;
        for (int j = std::max (0, k - reach); j <= std::min ((int) power.size() - 1, k + reach); ++j) { s += power[(size_t) j]; ++c; }
        smooth[(size_t) k] = 10.0 * std::log10 (s / c + 1.0e-20);
    }
    std::vector<double> perHz;
    for (double hz = 0.0; hz < sr * 0.5 - binHz; hz += 1.0)
    {
        const double at = hz / binHz;
        const int i = (int) at;
        perHz.push_back (smooth[(size_t) i] + (at - i) * (smooth[(size_t) i + 1] - smooth[(size_t) i]));
    }
    return perHz;
}

// How far one spectrum's shape has moved against another's, as a scale on frequency: the scale that lines the two up best in a
// band. Level differences don't count. Both should be smoothed over the same width first, about one harmonic spacing.
inline double shapeScale (const std::vector<double>& reference, const std::vector<double>& moved, double lowHz, double highHz, double lowest = 0.6, double highest = 1.6)
{
    double bestScale = 1.0, bestError = 1.0e30;
    const size_t limit = std::min (reference.size(), moved.size()) - 1;
    for (double scale = lowest; scale <= highest; scale *= 1.002)
    {
        // Compare reference(f) with moved(f * scale), after taking out the mean difference.
        double mean = 0.0; int count = 0;
        for (double hz = lowHz; hz <= highHz; hz *= 1.01)
        {
            const size_t a = (size_t) hz, b = (size_t) (hz * scale);
            if (b >= limit) break;
            mean += moved[b] - reference[a];
            ++count;
        }
        if (count < 20) continue;
        mean /= count;
        double error = 0.0;
        for (double hz = lowHz; hz <= highHz; hz *= 1.01)
        {
            const size_t a = (size_t) hz, b = (size_t) (hz * scale);
            if (b >= limit) break;
            const double d = moved[b] - reference[a] - mean;
            error += d * d;
        }
        error /= count;
        if (error < bestError) { bestError = error; bestScale = scale; }
    }
    return bestScale;
}

// The peak nearest a frequency, or 0 if none within reach.
inline double peakNear (const std::vector<double>& peaks, double hz, double reach = 0.35)
{
    double best = 0.0, dist = 1.0e9;
    for (double p : peaks)
        if (std::abs (p / hz - 1.0) < reach && std::abs (p - hz) < dist) { dist = std::abs (p - hz); best = p; }
    return best;
}

// Non-harmonic energy against the total, in dB, on a tone whose pitch is known.
inline double nonHarmonicDb (const float* x, int n, double hz, double sr)
{
    int order = 1;
    while ((1 << order) < n) ++order;
    --order;
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2, 0.0f);
    for (int i = 0; i < size; ++i)
        buf[(size_t) i] = x[i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    const double binHz = sr / size;
    double total = 0, off = 0;
    for (int k = (int) (50.0 / binHz); k < (int) (16000.0 / binHz) && k <= size / 2; ++k)
    {
        const double f = k * binHz;
        const double e = (double) buf[(size_t) k] * buf[(size_t) k];
        const double near = std::round (f / hz) * hz;
        total += e;
        if (std::abs (f - near) > std::max (4.0 * binHz, 4.0))
            off += e;
    }
    return 10.0 * std::log10 (std::max (off, 1.0e-30) / std::max (total, 1.0e-30));
}

// Two sided pitch track for real material, used as the reference: long windows and a best path over the whole file.
// Not ground truth, but far better placed than any real time tracker, since it can see the future.
struct RefFrame { double hz = 0.0; };
inline std::vector<double> referenceTrack (const std::vector<float>& input, double sr, double hopSec = 0.005, double lowHz = 60.0, double highHz = 1100.0)
{
    const int drop = std::max (1, (int) std::lround (sr / 12000.0));
    const double rate = sr / drop;
    // Simple low pass by averaging, twice
    std::vector<float> y;
    y.reserve (input.size() / (size_t) drop);
    double a = 0, b = 0;
    const double k = 1.0 - std::exp (-2.0 * M_PI * 2500.0 / sr);
    long count = 0;
    for (float v : input)
    {
        a += k * ((double) v - a);
        b += k * (a - b);
        if (++count % drop == 0) y.push_back ((float) b);
    }
    const int tauMin = (int) (rate / highHz), tauMax = (int) (rate / lowHz) + 1, W = tauMax + 40;
    const int hop = std::max (1, (int) std::lround (hopSec * rate));
    const int frames = std::max (0, ((int) y.size() - W - tauMax - 2) / hop);
    struct Cand { double tau; double cost; };
    constexpr int maxCand = 8;
    std::vector<std::array<Cand, maxCand>> cands ((size_t) frames);
    std::vector<int> numCand ((size_t) frames, 0);
    std::vector<float> d ((size_t) tauMax + 3);
    for (int f = 0; f < frames; ++f)
    {
        const float* base = y.data() + f * hop + tauMax + 2; // +2: tau reaches tauMax + 1, so stay inside the buffer
        float e0 = 0; for (int i = 0; i < W; ++i) e0 += base[i] * base[i];
        auto& c = cands[(size_t) f];
        int found = 0;
        if (e0 / W > 1.0e-7f)
        {
            for (int tau = tauMin - 1; tau <= tauMax + 1; ++tau)
            {
                float x = 0, eb = 0;
                const float* lagged = base - tau;
                for (int i = 0; i < W; ++i) { x += base[i] * lagged[i]; eb += lagged[i] * lagged[i]; }
                d[(size_t) tau] = 1.0f - 2.0f * x / (e0 + eb + 1.0e-12f);
            }
            for (int tau = tauMin; tau <= tauMax && found < maxCand; ++tau)
                if (d[(size_t) tau] < d[(size_t) tau - 1] && d[(size_t) tau] <= d[(size_t) tau + 1] && d[(size_t) tau] < 0.35f)
                {
                    const float p = d[(size_t) tau - 1], q = d[(size_t) tau], r = d[(size_t) tau + 1];
                    const float den = p - 2 * q + r;
                    const double t = tau + (den > 1.0e-9f ? 0.5 * (p - r) / den : 0.0);
                    // Small bias towards short periods, as every multiple of a period also fits.
                    c[(size_t) found++] = { t, (double) q + 0.01 * std::log2 (t / tauMin) };
                }
        }
        numCand[(size_t) f] = found;
    }

    // Best path. No pitch is a candidate at a fixed cost.
    constexpr double noPitchCost = 0.35, jumpCost = 0.5;
    std::vector<std::array<double, maxCand + 1>> best ((size_t) frames);
    std::vector<std::array<int, maxCand + 1>> from ((size_t) frames);
    for (int f = 0; f < frames; ++f)
    {
        const int nc = numCand[(size_t) f];
        for (int s = 0; s <= nc; ++s)
        {
            const double own = s < nc ? cands[(size_t) f][(size_t) s].cost : noPitchCost;
            double bestCost = 0.0; int bestFrom = 0;
            if (f > 0)
            {
                bestCost = 1.0e12;
                const int pc = numCand[(size_t) f - 1];
                for (int q = 0; q <= pc; ++q)
                {
                    double trans = 0.0;
                    if (s < nc && q < pc) trans = jumpCost * std::abs (std::log2 (cands[(size_t) f][(size_t) s].tau / cands[(size_t) f - 1][(size_t) q].tau));
                    else if (s < nc || q < pc) trans = 0.05;
                    const double total = best[(size_t) f - 1][(size_t) q] + trans;
                    if (total < bestCost) { bestCost = total; bestFrom = q; }
                }
            }
            best[(size_t) f][(size_t) s] = own + bestCost;
            from[(size_t) f][(size_t) s] = bestFrom;
        }
    }
    std::vector<double> track ((size_t) frames, 0.0);
    if (frames > 0)
    {
        int s = 0;
        double bestCost = 1.0e12;
        for (int q = 0; q <= numCand[(size_t) frames - 1]; ++q)
            if (best[(size_t) frames - 1][(size_t) q] < bestCost) { bestCost = best[(size_t) frames - 1][(size_t) q]; s = q; }
        for (int f = frames - 1; f >= 0; --f)
        {
            track[(size_t) f] = s < numCand[(size_t) f] ? rate / cands[(size_t) f][(size_t) s].tau : 0.0;
            s = from[(size_t) f][(size_t) s];
        }
    }
    return track;
}

// Time of frame f of referenceTrack, in seconds: the centre of its window, near enough for an octave check.
inline double referenceTime (int f, double sr, double hopSec = 0.005, double lowHz = 60.0)
{
    const int drop = std::max (1, (int) std::lround (sr / 12000.0));
    const double rate = sr / drop;
    const int tauMax = (int) (rate / lowHz) + 1, W = tauMax + 40;
    const int hop = std::max (1, (int) std::lround (hopSec * rate));
    return ((double) (f * hop + tauMax) + 0.5 * W) / rate;
}
} // namespace measure
