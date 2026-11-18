// Build: clang++ -O3 -std=c++17 -I tests tests/prototype/HarmonicPrototype.cpp -framework Accelerate -o build/HarmonicPrototype
// Run: build/HarmonicPrototype <oracle|lines|perk|table|combo|curves> <tests: f formants, h high notes, b big shifts, d dev high glides, n dev held notes, v vowel change>
// Offline prototype: pitch-synchronous resampled frames with a gain per harmonic (a harmonic shifter), measured on the dev voices
// with the same readings as tests/PluginTest.cpp. True f0 is given, so this measures the shifter and the estimators, not the tracker.
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "VoiceSynth.h"

static const double SR = 48000.0;

// ------------------------------------------------------------------ readings, as in tests/Analysis.h and PluginTest.cpp
std::vector<double> smoothSpectrumDb (const float* x, long n, double sr, double smoothHz)
{
    constexpr int order = 12, size = 1 << order;
    static FFTSetup setup = vDSP_create_fftsetup (order, kFFTRadix2);
    std::vector<double> power (size / 2 + 1, 0.0);
    std::vector<float> buf (size), re (size / 2), im (size / 2);
    for (long at = 0; at + size <= n; at += size / 4)
    {
        for (int i = 0; i < size; ++i) buf[(size_t) i] = x[at + i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
        DSPSplitComplex sp { re.data(), im.data() };
        vDSP_ctoz ((const DSPComplex*) buf.data(), 2, &sp, 1, size / 2);
        vDSP_fft_zrip (setup, &sp, 1, order, kFFTDirection_Forward);
        power[0] += (double) re[0] * re[0]; power[size / 2] += (double) im[0] * im[0];
        for (int k = 1; k < size / 2; ++k) power[(size_t) k] += (double) re[k] * re[k] + (double) im[k] * im[k];
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
        const double at = hz / binHz; const int i = (int) at;
        perHz.push_back (smooth[(size_t) i] + (at - i) * (smooth[(size_t) i + 1] - smooth[(size_t) i]));
    }
    return perHz;
}
double shapeScale (const std::vector<double>& reference, const std::vector<double>& moved, double lowHz, double highHz, double lowest = 0.6, double highest = 1.6)
{
    double bestScale = 1.0, bestError = 1.0e30;
    const size_t limit = std::min (reference.size(), moved.size()) - 1;
    for (double scale = lowest; scale <= highest; scale *= 1.002)
    {
        double mean = 0.0; int count = 0;
        for (double hz = lowHz; hz <= highHz; hz *= 1.01) { const size_t a = (size_t) hz, b = (size_t) (hz * scale); if (b >= limit) break; mean += moved[b] - reference[a]; ++count; }
        if (count < 20) continue;
        mean /= count;
        double error = 0.0;
        for (double hz = lowHz; hz <= highHz; hz *= 1.01) { const size_t a = (size_t) hz, b = (size_t) (hz * scale); if (b >= limit) break; const double d = moved[b] - reference[a] - mean; error += d * d; }
        error /= count;
        if (error < bestError) { bestError = error; bestScale = scale; }
    }
    return bestScale;
}
double rms (const float* x, long n) { double s = 0; for (long i = 0; i < n; ++i) s += (double) x[i] * x[i]; return std::sqrt (s / std::max (n, 1L)); }
double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }
double harmonicDb (const float* x, long n, double hz)
{
    double best = 0.0;
    for (int cent = -6; cent <= 6; cent += 2)
    {
        const double w = 2.0 * M_PI * hz * std::pow (2.0, cent / 1200.0) / SR;
        double re = 0.0, im = 0.0;
        for (long i = 0; i < n; ++i) { const double h = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) i / (double) n); re += h * x[i] * std::cos (w * (double) i); im += h * x[i] * std::sin (w * (double) i); }
        best = std::max (best, re * re + im * im);
    }
    return 10.0 * std::log10 (best + 1.0e-20);
}

// ------------------------------------------------------------------ voices
struct VStyle { double breathDb = -60.0, softAboveHz = 0.0; };
std::vector<float> styledVoice (const std::vector<double>& f0, const voice::Vowel& v, const VStyle& st)
{
    auto x = voice::synth (f0, v, SR, st.breathDb);
    if (st.softAboveHz > 0.0)
    {
        const double a = std::exp (-2.0 * M_PI * st.softAboveHz / SR);
        double y = 0.0; const double before = rms (x.data(), (long) x.size());
        for (auto& s : x) { y = (1.0 - a) * s + a * y; s = (float) y; }
        const double trim = before / std::max (1.0e-12, rms (x.data(), (long) x.size()));
        for (auto& s : x) s = (float) (s * trim);
    }
    return x;
}
std::vector<double> glideF0 (double centre)
{
    std::vector<double> f0 ((size_t) (6.0 * SR));
    for (size_t i = 0; i < f0.size(); ++i) { const double u = std::fmod ((double) i / SR / 3.0, 1.0); const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u; f0[i] = centre * std::pow (2.0, 0.5 * tri); }
    return f0;
}
double resonatorDb (double f, double F, double B)
{
    const double r = std::exp (-M_PI * B / SR), c = 2.0 * r * std::cos (2.0 * M_PI * F / SR);
    const std::complex<double> z1 = std::polar (1.0, -2.0 * M_PI * f / SR);
    return 20.0 * std::log10 ((1.0 - c + r * r) / std::abs (1.0 - c * z1 + r * r * z1 * z1));
}
double tractDb (double f, const voice::Vowel& v, const VStyle& st)
{
    double s = 0;
    if (st.softAboveHz > 0) { const double a = std::exp (-2.0 * M_PI * st.softAboveHz / SR); s += 20.0 * std::log10 ((1.0 - a) / std::abs (1.0 - a * std::polar (1.0, -2.0 * M_PI * f / SR))); }
    for (int i = 0; i < 5; ++i) s += resonatorDb (f, v.f[i], v.bw[i]);
    return s;
}

// ------------------------------------------------------------------ estimators of the gain per harmonic
struct Frame { const double* levelDb; int harmonics; double f0In, f0Out, scale, time; const double* noiseDb = nullptr; };
struct Estimator
{
    virtual ~Estimator() = default;
    virtual void reset() {}
    // gains[k] in dB for k = 1..harmonics
    virtual void gains (const Frame& f, double* g) = 0;
};
struct Flat : Estimator { void gains (const Frame& f, double* g) override { for (int k = 1; k <= f.harmonics; ++k) g[k] = 0; } };
struct Oracle : Estimator
{
    const voice::Vowel* v = nullptr; VStyle st;
    void gains (const Frame& f, double* g) override { for (int k = 1; k <= f.harmonics; ++k) g[k] = tractDb (k * f.f0Out / f.scale, *v, st) - tractDb (k * f.f0In, *v, st); }
};
// Straight lines in dB through this frame's harmonics.
double linesAt (const Frame& f, double hz)
{
    const double x = hz / f.f0In;
    if (x <= 1.0) return f.levelDb[1];
    const int i = (int) x;
    if (i >= f.harmonics) return f.levelDb[f.harmonics];
    return f.levelDb[i] + (x - i) * (f.levelDb[i + 1] - f.levelDb[i]);
}
struct Lines : Estimator
{
    void gains (const Frame& f, double* g) override { for (int k = 1; k <= f.harmonics; ++k) g[k] = linesAt (f, k * f.f0Out / f.scale) - f.levelDb[k]; }
};
// Each harmonic number's level kept against the pitch it was heard at. Exact ratios wherever harmonic k has been at the pitch asked for.
struct PerHarmonic : Estimator
{
    static constexpr int maxK = 64, binsPerSemi = 8; static constexpr double lowHz = 60.0; static constexpr int bins = 60 * binsPerSemi;
    double forget = 2.0;      // seconds for an entry to fade when not refreshed
    double changeDb = 3.0;    // mean-removed RMS mismatch that counts as a new vowel
    double topHz = 5000.0;    // harmonics above this are not remembered
    std::vector<float> level, weight, when;
    double lastTime = 0; int resets = 0; double hits = 0, asks = 0;
    void reset() override { level.assign ((size_t) maxK * bins, 0.0f); weight = level; when = level; lastTime = 0; resets = 0; hits = asks = 0; }
    static double binOf (double hz) { return 12.0 * binsPerSemi * std::log2 (hz / lowHz); }
    double checkWeight = 1.0;
    bool lookup (int k, double hz, double& out, double minW = 0.2) const
    {
        const double b = binOf (hz); const int i = (int) std::floor (b); const double t = b - i;
        if (i < 0 || i + 1 >= bins) return false;
        const float w0 = weight[(size_t) (k * bins + i)], w1 = weight[(size_t) (k * bins + i + 1)];
        if (w0 < minW || w1 < minW) return false;
        out = (1 - t) * level[(size_t) (k * bins + i)] + t * level[(size_t) (k * bins + i + 1)];
        return true;
    }
    // Frames wait settleSec before they are stored, so a new frame can be checked against what was heard at this pitch before it.
    double settleSec = 0.05;
    struct Pending { double f0, time; int K; float lev[maxK]; };
    std::vector<Pending> fifo; int fifoHead = 0, fifoCount = 0;
    void store (const Pending& p)
    {
        const double dt = p.time - lastTime; lastTime = p.time;
        const float keep = (float) std::exp (-std::max (0.0, dt) / forget);
        for (auto& w : weight) w *= keep;
        const double b = binOf (p.f0); const int i = (int) std::floor (b); const double t = b - i;
        if (i < 0 || i + 1 >= bins) return;
        for (int k = 1; k <= p.K; ++k)
            for (int j = 0; j < 2; ++j)
            {
                const float a = (float) (j ? t : 1 - t);
                const size_t at = (size_t) (k * bins + i + j);
                const float w = weight[at] + a;
                level[at] += (float) ((p.lev[k] - level[at]) * a / std::max (1e-6f, w));
                weight[at] = std::min (w, 4.0f);
                when[at] = (float) p.time;
            }
    }
    void gains (const Frame& f, double* g) override
    {
        if (level.empty()) reset();
        if (fifo.empty()) fifo.resize (256);
        const int K = std::min (f.harmonics, maxK - 1);
        int Kt = 0; while (Kt + 1 <= K && (Kt + 1) * f.f0In <= topHz) ++Kt;
        // A new vowel? Compare with what is settled at this pitch.
        double sum = 0, sum2 = 0; int n = 0;
        for (int k = 1; k <= Kt; ++k)
        {
            double was; if (! lookup (k, f.f0In, was, checkWeight)) continue;
            if (f.noiseDb != nullptr && f.levelDb[k] - f.noiseDb[k] < 15.0) continue;
            const double d = f.levelDb[k] - was; sum += d; sum2 += d * d; ++n;
        }
        if (n >= 3)
        {
            const double m = sum / n, r = std::sqrt (std::max (0.0, sum2 / n - m * m));
            lastMismatch = r; maxMismatch = std::max (maxMismatch, r);
            if (r > changeDb) { if (getenv ("RESETLOG")) std::printf ("RESET t %.3f f0 %.1f mismatch %.2f n %d\n", f.time, f.f0In, r, n); std::fill (weight.begin(), weight.end(), 0.0f); fifoCount = 0; ++resets; }
        }
        // queue this frame, store the ones that have settled
        Pending p; p.f0 = f.f0In; p.time = f.time; p.K = Kt; for (int k = 1; k <= Kt; ++k) p.lev[k] = (float) f.levelDb[k];
        fifo[(size_t) ((fifoHead + fifoCount) % (int) fifo.size())] = p;
        if (fifoCount < (int) fifo.size()) ++fifoCount; else fifoHead = (fifoHead + 1) % (int) fifo.size();
        while (fifoCount > 0 && f.time - fifo[(size_t) fifoHead].time >= settleSec) { store (fifo[(size_t) fifoHead]); fifoHead = (fifoHead + 1) % (int) fifo.size(); --fifoCount; }
        for (int k = 1; k <= f.harmonics; ++k)
        {
            double then;
            const double want = f.f0Out / f.scale;
            asks += 1;
            if (k < maxK && k * want <= topHz && lookup (k, want, then)) { g[k] = then - f.levelDb[k]; hits += 1; }
            else g[k] = linesAt (f, k * want) - f.levelDb[k];
        }
    }
    double lastMismatch = 0, maxMismatch = 0;
};

// The vocal tract as a table over frequency, learned from every harmonic heard, with each harmonic number's source level learned
// alongside: level_k = source_k + tract(k f0) + offset. A harmonic number's source level is shared by every pitch, so harmonics
// that land on the same frequency at different pitches tie the two together.
struct TractTable : Estimator
{
    static constexpr double binHz = 10.0, topHz = 6000.0; static constexpr int bins = (int) (topHz / binHz) + 2, maxK = 128;
    double forget = 2.0, changeDb = 3.0, sourceRate = 2.0, leak = 0.05, useWeight = 0.2, fitTop = 5000.0;
    std::vector<double> tract, weight, when, source;
    double lastTime = -1; int resets = 0; long hits = 0, asks = 0;
    void reset() override { tract.assign (bins, 0.0); weight.assign (bins, 0.0); when.assign (bins, -10.0); source.assign (maxK, 0.0); lastTime = -1; resets = 0; hits = asks = 0; }
    bool at (double hz, double& out) const
    {
        const double b = hz / binHz; const int i = (int) b; const double t = b - i;
        if (i < 0 || i + 1 >= bins) return false;
        if (weight[(size_t) i] < useWeight || weight[(size_t) i + 1] < useWeight) return false;
        out = (1 - t) * tract[(size_t) i] + t * tract[(size_t) i + 1];
        return true;
    }
    void gains (const Frame& f, double* g) override
    {
        if (tract.empty()) reset();
        const double dt = lastTime < 0 ? 0.0 : std::max (0.0, f.time - lastTime); lastTime = f.time;
        const int K = std::min (f.harmonics, maxK - 1);
        // offset: how this frame sits against the table, from entries not written just now
        double sum = 0, sum2 = 0, wsum = 0; int n = 0;
        std::vector<double> resid ((size_t) K + 1, 0.0); std::vector<char> has ((size_t) K + 1, 0);
        for (int k = 1; k <= K && k * f.f0In <= fitTop; ++k)
        {
            double t; if (! at (k * f.f0In, t)) continue;
            resid[(size_t) k] = f.levelDb[k] - source[(size_t) k] - t; has[(size_t) k] = 1;
            sum += resid[(size_t) k]; sum2 += resid[(size_t) k] * resid[(size_t) k]; wsum += 1; ++n;
        }
        double offset = n > 0 ? sum / wsum : 0.0;
        if (n >= 3)
        {
            const double spread = std::sqrt (std::max (0.0, sum2 / wsum - offset * offset));
            // only judge against entries that are not this moment's own
            double s2 = 0, s1 = 0; int m = 0;
            for (int k = 1; k <= K && k * f.f0In <= fitTop; ++k)
                if (has[(size_t) k]) { const int i = (int) (k * f.f0In / binHz); if (f.time - when[(size_t) i] > 0.15) { s1 += resid[(size_t) k]; s2 += resid[(size_t) k] * resid[(size_t) k]; ++m; } }
            if (m >= 3)
            {
                const double mm = s1 / m, r = std::sqrt (std::max (0.0, s2 / m - mm * mm));
                if (r > changeDb) { std::fill (weight.begin(), weight.end(), 0.0); ++resets; offset = 0.0; std::fill (has.begin(), has.end(), 0); }
            }
            (void) spread;
        }
        // source levels move towards what reconciles this frame with the table, and leak back to 0
        const double rate = 1.0 - std::exp (-dt * sourceRate), back = 1.0 - std::exp (-dt * leak);
        for (int k = 1; k <= K && k * f.f0In <= fitTop; ++k)
        {
            if (has[(size_t) k]) source[(size_t) k] += rate * (resid[(size_t) k] - offset);
            source[(size_t) k] -= back * source[(size_t) k];
        }
        // fade, then write this frame's tract samples
        const double keep = std::exp (-dt / forget);
        for (auto& w : weight) w *= keep;
        for (int k = 1; k <= K && k * f.f0In <= topHz - binHz; ++k)
        {
            const double value = f.levelDb[k] - source[(size_t) k] - offset;
            const double b = k * f.f0In / binHz; const int i = (int) b; const double t = b - i;
            for (int j = 0; j < 2; ++j)
            {
                const double a = j ? t : 1 - t; const size_t q = (size_t) (i + j);
                const double w = weight[q] + a;
                tract[q] += (value - tract[q]) * a / std::max (1e-9, w);
                weight[q] = std::min (w, 4.0); when[q] = f.time;
            }
        }
        for (int k = 1; k <= f.harmonics; ++k)
        {
            const double want = k * f.f0Out / f.scale; double a, b;
            ++asks;
            if (k * f.f0In < fitTop && want < fitTop && at (want, a) && at (k * f.f0In, b)) { g[k] = a - b; ++hits; }
            else g[k] = linesAt (f, want) - f.levelDb[k];
        }
    }
};



// Per-harmonic memory, tied together: harmonics k and m that were heard at the same frequency at different pitches give the difference of
// their source levels. With those, every harmonic's memory is a sample of one tract curve, so any harmonic can borrow another's.
struct Curves : Estimator
{
    PerHarmonic mem; static constexpr int K = PerHarmonic::maxK, B = PerHarmonic::bins;
    std::vector<double> sigma, sigW; std::vector<int> comp; int frames = 0; double gapSemis = 0.5;
    int findRoot (int a) { while (comp[(size_t) a] != a) a = comp[(size_t) a] = comp[(size_t) comp[(size_t) a]]; return a; }
    long fromSame = 0, fromOther = 0, fromLines = 0;
    void reset() override { mem.reset(); sigma.assign (K, 0.0); sigW.assign (K, 0.0); comp.resize (K); for (int i = 0; i < K; ++i) comp[(size_t) i] = i; frames = 0; fromSame = fromOther = fromLines = 0; }
    // value of harmonic k's memory at fractional bin b, or false
    bool val (int k, double b, double& out, double minW = 0.2) const
    {
        const int i = (int) std::floor (b); const double t = b - i;
        if (i < 0 || i + 1 >= B) return false;
        const float w0 = mem.weight[(size_t) (k * B + i)], w1 = mem.weight[(size_t) (k * B + i + 1)];
        const bool ok0 = w0 >= minW, ok1 = w1 >= minW;
        if (ok0 && ok1) { out = (1 - t) * mem.level[(size_t) (k * B + i)] + t * mem.level[(size_t) (k * B + i + 1)]; return true; }
        // at the edge of what was heard: the nearer bin alone, if it is the one known
        if (ok0 && t < 0.5) { out = mem.level[(size_t) (k * B + i)]; return true; }
        if (ok1 && t >= 0.5) { out = mem.level[(size_t) (k * B + i + 1)]; return true; }
        return false;
    }
    void solveSigma (int top)
    {
        // pair differences d[k][m] = mean over overlap of P_k - P_m at equal frequency
        std::vector<double> d ((size_t) K * 3, 0.0), w ((size_t) K * 3, 0.0);
        for (int k = 1; k < top; ++k)
            for (int step = 1; step <= 2 && k + step <= top; ++step)
            {
                const int m = k + step; const double off = 12.0 * PerHarmonic::binsPerSemi * std::log2 ((double) k / m);
                double sum = 0, cnt = 0;
                for (int i = 0; i + 1 < B; ++i)
                {
                    double a, b; if (! val (k, i, a, 0.5)) continue;
                    if (! val (m, i + off, b, 0.5)) continue;
                    sum += a - b; cnt += 1;
                }
                // octave neighbours that only touch: compare the nearest ends if they are close
                if (cnt == 0 && step == 1)
                {
                    int hiK = -1, loM = -1;
                    for (int i = 0; i < B; ++i) if (mem.weight[(size_t) (k * B + i)] >= 0.5f) hiK = i;
                    for (int i = B - 1; i >= 0; --i) if (mem.weight[(size_t) (m * B + i)] >= 0.5f) loM = i;
                    if (hiK >= 0 && loM >= 0)
                    {
                        const double gap = (loM - off) - hiK; // in bins, at equal frequency
                        if (std::abs (gap) <= gapSemis * PerHarmonic::binsPerSemi) { sum = mem.level[(size_t) (k * B + hiK)] - mem.level[(size_t) (m * B + loM)]; cnt = 0.3; }
                    }
                }
                d[(size_t) (k * 3 + step)] = cnt >= 1 ? sum / cnt : sum;
                if (getenv ("PAIR") && k == 1 && step == 1 && (frames % 800) < 4) std::printf ("PAIR frame %d cnt %.1f d %.2f\n", frames, cnt, d[(size_t) (k * 3 + step)]);
                w[(size_t) (k * 3 + step)] = std::min (cnt, 20.0);
            }
        // Least squares on sigma_k - sigma_m = d_km with sigma_1 = 0: a weighted graph Laplacian, solved directly.
        const int n = top; // unknowns sigma_2..sigma_top -> index k-2
        std::vector<double> A ((size_t) n * n, 0.0), rhs ((size_t) n, 0.0);
        auto add = [&] (int k, int m, double ww, double dd)
        {
            // (sigma_k - sigma_m - dd)^2 * ww
            if (ww <= 0) return;
            if (k >= 2) { A[(size_t) ((k - 2) * n + (k - 2))] += ww; rhs[(size_t) (k - 2)] += ww * dd; }
            if (m >= 2) { A[(size_t) ((m - 2) * n + (m - 2))] += ww; rhs[(size_t) (m - 2)] -= ww * dd; }
            if (k >= 2 && m >= 2) { A[(size_t) ((k - 2) * n + (m - 2))] -= ww; A[(size_t) ((m - 2) * n + (k - 2))] -= ww; }
        };
        for (int k = 1; k < top; ++k) for (int step = 1; step <= 2 && k + step <= top; ++step) add (k, k + step, w[(size_t) (k * 3 + step)], d[(size_t) (k * 3 + step)]);
        for (int i = 0; i < n - 1; ++i) A[(size_t) (i * n + i)] += 1e-6; // unlinked ones stay put
        const int nn = n - 1;
        for (int c = 0; c < nn; ++c)
        {
            const double p = A[(size_t) (c * n + c)]; if (std::abs (p) < 1e-12) continue;
            for (int r = c + 1; r < nn; ++r) { const double f = A[(size_t) (r * n + c)] / p; if (f == 0) continue; for (int q = c; q < nn; ++q) A[(size_t) (r * n + q)] -= f * A[(size_t) (c * n + q)]; rhs[(size_t) r] -= f * rhs[(size_t) c]; }
        }
        for (int c = nn - 1; c >= 0; --c) { double v = rhs[(size_t) c]; for (int q = c + 1; q < nn; ++q) v -= A[(size_t) (c * n + q)] * sigma[(size_t) (q + 2)]; const double p = A[(size_t) (c * n + c)]; sigma[(size_t) (c + 2)] = std::abs (p) > 1e-12 ? v / p : 0.0; }
        for (int k = 2; k <= top; ++k) { double den = 0; for (int step = 1; step <= 2; ++step) { if (k - step >= 1) den += w[(size_t) ((k - step) * 3 + step)]; if (k + step <= top) den += w[(size_t) (k * 3 + step)]; } sigW[(size_t) k] = den; }
        sigW[1] = 1e9;
        for (int i = 0; i < K; ++i) comp[(size_t) i] = i;
        for (int k = 1; k < top; ++k) for (int step = 1; step <= 2 && k + step <= top; ++step)
            if (w[(size_t) (k * 3 + step)] > 0) { const int a = findRoot (k), b = findRoot (k + step); if (a != b) comp[(size_t) std::max (a, b)] = std::min (a, b); }
        for (int k = 1; k < K; ++k) comp[(size_t) k] = findRoot (k);
    }
    // tract curve at frequency hz: harmonic k's own memory if it has it, else the others', source levels taken out
    bool tract (double hz, int preferK, double& out, bool& same) const
    {
        same = false;
        double v;
        if (preferK < K && val (preferK, PerHarmonic::binOf (hz / preferK), v)) { out = v - sigma[(size_t) preferK]; same = true; return true; }
        double sum = 0, wsum = 0;
        for (int m = 1; m < K; ++m)
        {
            if (m * PerHarmonic::lowHz > hz || sigW[(size_t) m] <= 0 || preferK >= K || comp[(size_t) m] != comp[(size_t) preferK]) continue;
            if (val (m, PerHarmonic::binOf (hz / m), v)) { const double ww = 1.0 / (1.0 + std::abs (m - preferK)); sum += ww * (v - sigma[(size_t) m]); wsum += ww; }
        }
        if (wsum <= 0) return false;
        out = sum / wsum; return true;
    }
    void gains (const Frame& f, double* g) override
    {
        if (sigma.empty()) reset();
        std::vector<double> dummy ((size_t) f.harmonics + 2);
        mem.gains (f, dummy.data()); // stores this frame, vowel check
        int top = 1; while (top + 1 < K && (top + 1) * f.f0In <= mem.topHz) ++top;
        if ((frames++ % 4) == 0) solveSigma (std::min (K - 1, (int) (mem.topHz / PerHarmonic::lowHz)));
        for (int k = 1; k <= f.harmonics; ++k)
        {
            const double want = k * f.f0Out / f.scale, now = k * f.f0In; double a, b; bool sa, sb;
            sb = true; b = k < K ? f.levelDb[k] - sigma[(size_t) k] : 0.0;
            if (want <= mem.topHz && now <= mem.topHz && k < K && tract (want, k, a, sa))
            {
                // same harmonic at both ends needs no source levels
                g[k] = a - b; if (sa && sb) ++fromSame; else ++fromOther;
                static int shown = 0; if (getenv ("GBIG") && std::abs (g[k]) > 15 && shown < 25) { ++shown; std::printf ("GBIG t %.3f f0 %.1f k %d want %.0f now %.0f a %.2f (%d) b %.2f (%d) g %.2f\n", f.time, f.f0In, k, want, now, a, (int) sa, b, (int) sb, g[k]); }
            }
            else { g[k] = linesAt (f, want) - f.levelDb[k]; ++fromLines; }
        }
    }
};

const voice::Vowel* gV = nullptr; VStyle gSt;
struct Combo : Estimator
{
    double errK = 0, errT = 0, errL = 0; long nK = 0, nT = 0, nL = 0; double errLate = 0; long nLate = 0;
    PerHarmonic perK; TractTable table; long fromK = 0, fromTable = 0, fromLines = 0;
    bool unionCurve = true;
    std::vector<double> ch, cd;
    // Points: every table bin that is known, and this frame's harmonics with their source level and the frame's offset taken out.
    void buildCurve (const Frame& f)
    {
        ch.clear(); cd.clear();
        // offset of this frame against the table
        double sum = 0; int n = 0;
        for (int k = 1; k <= f.harmonics && k * f.f0In < table.fitTop; ++k) { double t; if (table.at (k * f.f0In, t)) { sum += f.levelDb[k] - table.source[(size_t) k] - t; ++n; } }
        const double offset = n > 0 ? sum / n : 0.0;
        std::vector<std::pair<double, double>> pts;
        for (int i = 0; i < TractTable::bins; ++i) if (table.weight[(size_t) i] >= table.useWeight) pts.push_back ({ i * TractTable::binHz, table.tract[(size_t) i] });
        // current harmonics fill the gaps: only where no table bin is within half a spacing
        for (int k = 1; k <= f.harmonics; ++k)
        {
            const double hz = k * f.f0In; bool near = false;
            for (int i = std::max (0, (int) ((hz - 0.5 * f.f0In) / TractTable::binHz)); i <= std::min (TractTable::bins - 1, (int) ((hz + 0.5 * f.f0In) / TractTable::binHz)); ++i)
                if (table.weight[(size_t) i] >= table.useWeight) { near = true; break; }
            if (! near || hz >= table.fitTop) pts.push_back ({ hz, f.levelDb[k] - (k < TractTable::maxK ? table.source[(size_t) k] : 0.0) - offset });
        }
        std::sort (pts.begin(), pts.end());
        for (auto& p : pts) { ch.push_back (p.first); cd.push_back (p.second); }
    }
    double curveAt (double hz) const
    {
        if (ch.empty()) return 0.0;
        if (hz <= ch.front()) return cd.front();
        if (hz >= ch.back()) return cd.back();
        const size_t j = (size_t) (std::lower_bound (ch.begin(), ch.end(), hz) - ch.begin());
        const double t = (hz - ch[j - 1]) / (ch[j] - ch[j - 1]);
        return cd[j - 1] + t * (cd[j] - cd[j - 1]);
    }
    void reset() override { perK.reset(); table.reset(); fromK = fromTable = fromLines = 0; }
    void gains (const Frame& f, double* g) override
    {
        std::vector<double> gk ((size_t) f.harmonics + 2), gt ((size_t) f.harmonics + 2);
        perK.gains (f, gk.data());
        table.gains (f, gt.data());
        if (unionCurve) buildCurve (f);
        for (int k = 1; k <= f.harmonics; ++k)
        {
            const double want = f.f0Out / f.scale; double a, b;
            int src = 0;
            if (k < PerHarmonic::maxK && k * want <= perK.topHz && perK.lookup (k, want, a) && perK.lookup (k, f.f0In, b)) { g[k] = a - b; ++fromK; src = 1; }
            else if (unionCurve) { g[k] = curveAt (k * want) - curveAt (k * f.f0In); ++fromTable; src = (k * want < table.fitTop && k * f.f0In < table.fitTop && table.at (k * want, a) && table.at (k * f.f0In, b)) ? 2 : 3; }
            double fmaxLev = -300; for (int q = 1; q <= f.harmonics; ++q) fmaxLev = std::max (fmaxLev, f.levelDb[q]);
            if (gV != nullptr && k * f.f0Out <= 4000.0 && f.levelDb[k] > fmaxLev - 30.0)
            {
                const double tru = tractDb (k * want, *gV, gSt) - tractDb (k * f.f0In, *gV, gSt) + 20.0 * std::log10 (want / f.f0In), e2 = (g[k] - tru) * (g[k] - tru);
                if (src == 1) { errK += e2; ++nK; } else if (src == 2) { errT += e2; ++nT; } else { errL += e2; ++nL; }
                if (f.time > 1.6) { errLate += e2; ++nLate; }
                static int big = 0;
                if (getenv ("BIG") && src == 1 && std::abs (g[k] - tru) > 4 && big < 30) { ++big; std::printf ("BIG t %.3f f0 %.1f out %.1f k %d: perK %.2f true %.2f then %.2f now %.2f level %.2f\n", f.time, f.f0In, f.f0Out, k, g[k], tru, a, b, f.levelDb[k]); }
                static int printed = 0;
                if (getenv ("PK") && src == 1 && f.time > 4.0 && printed < 40) { ++printed; std::printf ("t %.3f f0 %.1f k %d: perK %.2f true %.2f  stored then %.2f now %.2f level %.2f\n", f.time, f.f0In, k, g[k], tru, a, b, f.levelDb[k]); }
            }
            else { double x, y; if (k * f.f0In < table.fitTop && k * want < table.fitTop && table.at (k * want, x) && table.at (k * f.f0In, y)) { g[k] = x - y; ++fromTable; } else { g[k] = linesAt (f, k * want) - f.levelDb[k]; ++fromLines; } }
        }
    }
};

// ------------------------------------------------------------------ the shifter
double gSnrNeed = 10.0, gSmoothMs = 0.0; bool gKeepNoise = false;
struct Shifter
{
    int M = 4;               // periods per frame
    double over = 2.0;       // warped samples per period against the longest period
    bool perLobe = true;
    std::vector<double> sincTable;
    Shifter() {}
    static float readAt (const std::vector<float>& x, double pos)
    {
        // 16 point windowed sinc
        const int taps = 8; const long i = (long) std::floor (pos); const double f = pos - i;
        double s = 0, wsum = 0;
        for (int t = -taps + 1; t <= taps; ++t)
        {
            const long j = i + t; const double d = t - f;
            const double sinc = std::abs (d) < 1e-9 ? 1.0 : std::sin (M_PI * d) / (M_PI * d);
            const double u = d / taps; const double w = std::abs (u) >= 1 ? 0 : 0.5 + 0.5 * std::cos (M_PI * u);
            if (j >= 0 && j < (long) x.size()) s += x[(size_t) j] * sinc * w;
            wsum += sinc * w;
        }
        return (float) (s / wsum);
    }
    // phase arrays: cumulative cycles at each sample
    static double timeOfPhase (const std::vector<double>& ph, double p)
    {
        auto it = std::lower_bound (ph.begin(), ph.end(), p);
        if (it == ph.begin()) return 0.0;
        if (it == ph.end()) return (double) ph.size() - 1;
        const size_t j = (size_t) (it - ph.begin());
        return (double) (j - 1) + (p - ph[j - 1]) / (ph[j] - ph[j - 1]);
    }
    std::vector<float> run (const std::vector<float>& x, const std::vector<double>& f0, const std::function<double (double)>& ratioAt, double scale, Estimator& est)
    {
        const size_t n = x.size();
        std::vector<double> thIn (n), thOut (n);
        double a = 0, b = 0;
        for (size_t i = 0; i < n; ++i) { thIn[i] = a; thOut[i] = b; a += f0[i] / SR; b += f0[i] * ratioAt ((double) i / SR) / SR; }
        std::vector<float> y (n, 0.0f);
        std::vector<double> norm (n, 0.0);
        est.reset();
        const long firstJ = (long) std::ceil (thOut[0] + M / 2.0), lastJ = (long) std::floor (thOut[n - 1] - M / 2.0);
        std::vector<float> buf, re, im; std::vector<double> lev, g, smoothG; double lastT = 0;
        FFTSetup setup = nullptr; int setupOrder = 0;
        for (long j = firstJ; j <= lastJ; ++j)
        {
            const double tj = timeOfPhase (thOut, (double) j);
            const size_t ti = std::min (n - 1, (size_t) tj);
            const double fin = f0[ti], fout = fin * ratioAt (tj / SR);
            const double phA = (double) j + std::round (thIn[ti] + (tj - ti) * fin / SR - (double) j);
            int Q = 32; while (Q < over * std::max (SR / fin, SR / fout)) Q *= 2;
            const int N = M * Q; int order = 0; while ((1 << order) < N) ++order;
            if (order != setupOrder) { if (setup) vDSP_destroy_fftsetup (setup); setup = vDSP_create_fftsetup (order, kFFTRadix2); setupOrder = order; }
            buf.assign ((size_t) N, 0.0f); re.assign ((size_t) N / 2, 0.0f); im.assign ((size_t) N / 2, 0.0f);
            for (int i = 0; i < N; ++i)
            {
                const double u = (double) (i - N / 2) / Q;
                const double t = timeOfPhase (thIn, phA + u);
                const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / N);
                buf[(size_t) i] = (float) (w * readAt (x, t));
            }
            DSPSplitComplex sp { re.data(), im.data() };
            vDSP_ctoz ((const DSPComplex*) buf.data(), 2, &sp, 1, N / 2);
            vDSP_fft_zrip (setup, &sp, 1, order, kFFTDirection_Forward);
            // harmonic levels
            const int K = std::min (Q / 2 - 1, (int) (0.47 * SR / fin));
            lev.assign ((size_t) K + 2, -200.0); g.assign ((size_t) K + 2, 0.0);
            for (int k = 1; k <= K; ++k) { const int bin = k * M; lev[(size_t) k] = 10.0 * std::log10 ((double) re[bin] * re[bin] + (double) im[bin] * im[bin] + 1e-30) - 20.0 * std::log10 ((double) N); }
            // Noise floor from the bins between harmonics, where a Hann window over M periods puts no harmonic energy.
            std::vector<double> noise ((size_t) K + 2, 0.0);
            for (int k = 1; k <= K; ++k)
            {
                double sum = 0; int c = 0;
                for (int q = std::max (1, k - 2); q <= std::min (K, k + 2); ++q)
                    for (int side : { -M / 2, M / 2 }) { const int bin = q * M + side; if (bin > 0 && bin < N / 2) { sum += (double) re[bin] * re[bin] + (double) im[bin] * im[bin]; ++c; } }
                noise[(size_t) k] = 10.0 * std::log10 (sum / std::max (1, c) + 1e-30) - 20.0 * std::log10 ((double) N);
            }
            Frame fr { lev.data(), K, fin, fout, scale, tj / SR, noise.data() };
            if (getenv ("LV") && std::abs (fin - atof (getenv ("LV"))) < 0.4) std::printf ("LV t %.3f f0 %.2f k1 %.2f k2 %.2f k3 %.2f\n", tj / SR, fin, lev[1], lev[2], lev[3]);
            est.gains (fr, g.data());
            if (gSmoothMs > 0)
            {
                if (smoothG.size() < g.size()) smoothG.resize (g.size(), 0.0);
                const double dt = (tj - lastT) / SR; lastT = tj;
                const double a = 1.0 - std::exp (-std::max (0.0, dt) / (gSmoothMs * 0.001));
                for (size_t k = 1; k < g.size(); ++k) { smoothG[k] += a * (g[k] - smoothG[k]); g[k] = smoothG[k]; }
            }
            { static double last1 = 0, last2 = 0; if (getenv ("JUMP") && (std::abs (g[1] - last1) > 0.5 || std::abs (g[2] - last2) > 0.5)) std::printf ("JUMP t %.4f f0 %.1f g1 %.2f (was %.2f) g2 %.2f (was %.2f)\n", tj / SR, fin, g[1], last1, g[2], last2); last1 = g[1]; last2 = g[2]; }
            if (getenv ("G1") && tj / SR > atof (getenv ("G1")) && tj / SR < atof (getenv ("G1")) + 0.03) std::printf ("G1 t %.4f f0 %.1f g1 %.2f g2 %.2f g3 %.2f\n", tj / SR, fin, g[1], g[2], g[3]);
            std::vector<float> yr ((size_t) N / 2, 0.0f), yi ((size_t) N / 2, 0.0f);
            const int Kout = (int) (0.47 * SR / fout);
            const double snrNeed = gSnrNeed;
            for (int k = 1; k <= Kout; ++k)
            {
                int from = k; double gainDb = 0.0;
                const bool own = k <= K && lev[(size_t) k] - noise[(size_t) k] >= snrNeed;
                if (own) gainDb = g[(size_t) k];
                else
                {
                    // Too weak to trust, or past the input's top: borrow the harmonic now at the frequency this one is going to.
                    from = std::clamp ((int) std::lround (k * fout / (scale * fin)), 1, K);
                    gainDb = linesAt (fr, k * fout / scale) - lev[(size_t) from];
                    if (gKeepNoise) { from = std::min (k, K); gainDb = 0.0; }
                }
                gainDb = std::clamp (gainDb, -40.0, 24.0);
                const double gain = std::pow (10.0, gainDb / 20.0);
                for (int d = -M / 2; d < M / 2; ++d)
                {
                    const int to = k * M + d, src = from * M + d;
                    if (to <= 0 || to >= N / 2 || src <= 0 || src >= N / 2) continue;
                    yr[(size_t) to] = (float) (re[(size_t) src] * gain); yi[(size_t) to] = (float) (im[(size_t) src] * gain);
                }
            }
            // below the first harmonic: as it was
            for (int bin = 1; bin < M / 2; ++bin) { yr[(size_t) bin] = re[(size_t) bin]; yi[(size_t) bin] = im[(size_t) bin]; }
            yr[0] = re[0]; yi[0] = 0.0f;
            std::copy (yr.begin(), yr.end(), re.begin()); std::copy (yi.begin(), yi.end(), im.begin());
            vDSP_fft_zrip (setup, &sp, 1, order, kFFTDirection_Inverse);
            vDSP_ztoc (&sp, 1, (DSPComplex*) buf.data(), 2, N / 2);
            const float scaleBack = 1.0f / (2.0f * N);
            // overlap-add into output time
            const double t0 = timeOfPhase (thOut, (double) j - M / 2.0), t1 = timeOfPhase (thOut, (double) j + M / 2.0);
            for (long i = (long) std::ceil (t0); i <= (long) std::floor (t1) && i < (long) n; ++i)
            {
                if (i < 0) continue;
                const double u = thOut[(size_t) i] - (double) j;
                const double pos = u * Q + N / 2;
                const int p = (int) std::floor (pos); const double f = pos - p;
                if (p < 1 || p + 2 >= N) continue;
                // cubic
                const double y0 = buf[(size_t) p - 1], y1 = buf[(size_t) p], y2 = buf[(size_t) p + 1], y3 = buf[(size_t) p + 2];
                const double v = y1 + 0.5 * f * (y2 - y0 + f * (2 * y0 - 5 * y1 + 4 * y2 - y3 + f * (3 * (y1 - y2) + y3 - y0)));
                y[(size_t) i] += (float) (v * scaleBack);
                norm[(size_t) i] += 0.5 - 0.5 * std::cos (2.0 * M_PI * pos / N);
            }
        }
        for (size_t i = 0; i < n; ++i) y[i] = norm[i] > 1e-3 ? (float) (y[i] / norm[i]) : 0.0f;
        if (setup) vDSP_destroy_fftsetup (setup);
        return y;
    }
};

// ------------------------------------------------------------------ the readings
struct Score { double move = 0, moveReal = 0, floor = 0, level = 0, flatter = 0, peakFlatter = 0, peakSharper = -100, peakFlatterCrossed = 0; };
double peakOverAround (const std::vector<double>& spec, size_t f1)
{
    double sum = 0.0; int count = 0;
    for (size_t hz = (size_t) (0.6 * (double) f1); hz <= (size_t) (1.5 * (double) f1); hz += 5) { sum += spec[hz]; ++count; }
    return spec[f1] - sum / count;
}
Shifter gShifter;
std::function<Estimator* (const voice::Vowel&, const VStyle&)> gMake;
bool gDump = false;
// optional crude level trim, as the plug-in's: whole-signal after 0.3 s
void scoreFormants (Score& s, const voice::Vowel& v, double centre, int shift, const VStyle& st = {})
{
    const long from = (long) ((getenv ("FROM") ? atof (getenv ("FROM")) : 0.3) * SR);
    const auto f0 = glideF0 (centre);
    const auto in = styledVoice (f0, v, st);
    const double r = std::pow (2.0, shift / 12.0);
    std::vector<double> f0c (f0.size()); for (size_t i = 0; i < f0.size(); ++i) f0c[i] = f0[i] * r;
    const auto control = styledVoice (f0c, v, st);
    gV = &v; gSt = st;
    Estimator* e = gMake (v, st);
    auto out = gShifter.run (in, f0, [r] (double) { return r; }, 1.0, *e);
    if (getenv ("SIG")) if (auto* c = dynamic_cast<Curves*> (e))
    {
        const int N = 8192; std::vector<double> flow (N); for (int i = 0; i < N; ++i) { const double ph = (double) i / N; flow[i] = ph < 0.42 ? 0.5 * (1 - std::cos (M_PI * ph / 0.42)) : ph < 0.58 ? std::cos (0.5 * M_PI * (ph - 0.42) / 0.16) : 0.0; }
        auto mag = [&] (int k) { double re = 0, im = 0; for (int i = 0; i < N; ++i) { re += flow[i] * std::cos (2 * M_PI * k * i / N); im -= flow[i] * std::sin (2 * M_PI * k * i / N); } return 20 * std::log10 (k * std::sqrt (re * re + im * im)); };
        for (int k = 1; k <= (getenv ("SIGN") ? atoi (getenv ("SIGN")) : 12); ++k) std::printf ("      k %d sigma+20logk %.2f  true %.2f  w %.1f\n", k, c->sigma[(size_t) k] + 20 * std::log10 (k), mag (k) - mag (1), c->sigW[(size_t) k]);
    }
    if (gDump) if (auto* c = dynamic_cast<Curves*> (e)) std::printf ("      same %ld other %ld lines %ld resets %d max mismatch %.2f\n", c->fromSame, c->fromOther, c->fromLines, c->mem.resets, c->mem.maxMismatch);
    if (gDump) if (auto* c = dynamic_cast<Combo*> (e)) std::printf ("      gain error rms: perK %.2f dB (%ld), table %.2f (%ld), curve %.2f (%ld), after 1.6 s %.2f\n", std::sqrt (c->errK / std::max (1L, c->nK)), c->nK, std::sqrt (c->errT / std::max (1L, c->nT)), c->nT, std::sqrt (c->errL / std::max (1L, c->nL)), c->nL, std::sqrt (c->errLate / std::max (1L, c->nLate)));
    if (gDump) if (auto* t = dynamic_cast<TractTable*> (e)) std::printf ("      resets %d hits %.1f%%\n", t->resets, 100.0 * t->hits / std::max (1L, t->asks));
    delete e;
    const long n = (long) in.size() - from - 4000;
    const double smoothing = std::max (centre, centre * r);
    const auto want = smoothSpectrumDb (control.data() + from, n, SR, smoothing), inS = smoothSpectrumDb (in.data() + from, n, SR, smoothing), got = smoothSpectrumDb (out.data() + from, n, SR, smoothing);
    for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
    {
        const double against = shapeScale (inS, got, band.first, band.second), real = shapeScale (want, got, band.first, band.second), noise = shapeScale (want, inS, band.first, band.second);
        if (gDump) std::printf ("      band %.0f-%.0f: against input %.3f, real %.3f, floor %.3f\n", band.first, band.second, against, real, noise);
        s.move = std::max (s.move, std::abs (against - 1.0)); s.moveReal = std::max (s.moveReal, std::abs (real - 1.0)); s.floor = std::max (s.floor, std::abs (noise - 1.0));
    }
    if (getenv ("SPEC")) { for (int hz = 1000; hz <= 6000; hz += 250) std::printf ("      %d Hz: real %.1f out %.1f in %.1f\n", hz, want[(size_t) hz], got[(size_t) hz], inS[(size_t) hz]); }
    const auto fine = smoothSpectrumDb (control.data() + from, n, SR, 50.0), fineOut = smoothSpectrumDb (out.data() + from, n, SR, 50.0);
    const size_t f1 = (size_t) v.f[0];
    if (getenv ("FINE")) { const auto fineIn = smoothSpectrumDb (in.data() + from, n, SR, 50.0); for (size_t hz = (size_t) (0.5 * f1); hz <= (size_t) (1.6 * f1); hz += 25) std::printf ("      %zu Hz: real %.1f out %.1f in %.1f\n", hz, fine[hz], fineOut[hz], fineIn[hz]); }
    const double flatten = (fine[f1] - fine[f1 / 2]) - (fineOut[f1] - fineOut[f1 / 2]);
    const double pf = peakOverAround (fine, f1) - peakOverAround (fineOut, f1);
    s.peakFlatter = std::max (s.peakFlatter, pf); s.peakSharper = std::max (s.peakSharper, -pf);
    if (v.f[0] >= 1.02 * centre * r * std::sqrt (0.5)) s.peakFlatterCrossed = std::max (s.peakFlatterCrossed, pf);
    s.flatter = std::max (s.flatter, flatten);
    const double lvl = toDb (rms (out.data() + from, n)) - toDb (rms (in.data() + from, n));
    s.level = std::max (s.level, std::abs (lvl));
    if (gDump) std::printf ("    %s %.0f %+d: move %.2f%% real %.2f%% flat %.2f peak %.2f level %+.2f\n", v.name, centre, shift, 100 * s.move, 100 * s.moveReal, flatten, pf, lvl);
}
double heldNote (const voice::Vowel& v, double centre, int shift, const VStyle& st = {})
{
    const double r = std::pow (2.0, shift / 12.0), moved = centre * r;
    const auto f0 = voice::steady (centre, 1.6, SR);
    const auto in = styledVoice (f0, v, st);
    Estimator* e = gMake (v, st);
    const auto out = gShifter.run (in, f0, [r] (double) { return r; }, 1.0, *e);
    delete e;
    const auto real = styledVoice (voice::steady (moved, 1.6, SR), v, st);
    const long from = (long) (0.5 * SR), n = (long) (1.0 * SR);
    std::vector<double> d; double mean = 0;
    for (double hz = moved; hz <= 4000.0; hz += moved) { d.push_back (harmonicDb (out.data() + from, n, hz) - harmonicDb (real.data() + from, n, hz)); mean += d.back(); }
    mean /= d.size(); double s = 0; for (double x : d) s += (x - mean) * (x - mean);
    return std::sqrt (s / d.size());
}

int main (int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "lines";
    const std::string tests = argc > 2 ? argv[2] : "fhbn";
    gDump = std::getenv ("DUMP") != nullptr;
    if (const char* m = std::getenv ("M")) gShifter.M = std::atoi (m);
    if (getenv ("SNR")) gSnrNeed = atof (getenv ("SNR")); gKeepNoise = getenv ("KEEPNOISE") != nullptr; if (getenv ("SMOOTH")) gSmoothMs = atof (getenv ("SMOOTH"));
    const double forget = std::getenv ("FORGET") ? std::atof (std::getenv ("FORGET")) : 2.0;
    const double change = std::getenv ("CHANGE") ? std::atof (std::getenv ("CHANGE")) : 3.0;
    gMake = [&] (const voice::Vowel& v, const VStyle& st) -> Estimator* {
        if (which == "flat") return new Flat;
        if (which == "oracle") { auto* o = new Oracle; o->v = &v; o->st = st; return o; }
        if (which == "table") { auto* p = new TractTable; p->forget = forget; p->changeDb = change; if (getenv ("SRATE")) p->sourceRate = atof (getenv ("SRATE")); if (getenv ("LEAK")) p->leak = atof (getenv ("LEAK")); return p; }
        if (which == "curves") { auto* p = new Curves; p->mem.forget = forget; p->mem.changeDb = change; if (getenv ("SETTLE")) p->mem.settleSec = atof (getenv ("SETTLE")); return p; }
        if (which == "combo") { auto* p = new Combo; p->unionCurve = ! getenv ("NOUNION"); p->perK.forget = p->table.forget = forget; p->perK.changeDb = p->table.changeDb = change; if (getenv ("SRATE")) p->table.sourceRate = atof (getenv ("SRATE")); if (getenv ("LEAK")) p->table.leak = atof (getenv ("LEAK")); return p; }
        if (which == "perk") { auto* p = new PerHarmonic; p->forget = forget; p->changeDb = change; return p; }
        return new Lines; };
    if (tests.find ('f') != std::string::npos)
    {
        Score s, other;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU }) for (double c : { 110.0, 165.0 }) for (int sh : { 3, -3, 1, -1 }) scoreFormants (s, *v, c, sh);
        for (auto* v : { &voice::vowelA, &voice::vowelE }) for (int sh : { 3, -3 }) scoreFormants (s, *v, 247.0, sh);
        for (auto* v : { &voice::vowelO, &voice::vowelAe }) for (double c : { 130.0, 196.0 }) for (int sh : { 3, -3, 2, -2 }) scoreFormants (other, *v, c, sh);
        std::printf ("formants: move %.2f%% (4), other %.2f%% (4), F1 flatter %.2f dB (7), level %.2f dB (1.5), peak flatter %.2f, real move %.2f%%\n", 100 * s.move, 100 * other.move, std::max (s.flatter, other.flatter), std::max (s.level, other.level), std::max (s.peakFlatter, other.peakFlatter), 100 * std::max (s.moveReal, other.moveReal));
    }
    if (tests.find ('h') != std::string::npos)
    {
        Score h;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO }) for (double c : { 440.0, 660.0 }) for (int sh : { 3, -3 }) scoreFormants (h, *v, c, sh);
        std::printf ("high notes: move %.2f%% (floor %.2f + 2), peak flatter %.2f dB (9), sharper %.2f (6), crossed %.2f, level %.2f\n", 100 * h.move, 100 * h.floor, h.peakFlatter, h.peakSharper, h.peakFlatterCrossed, h.level);
    }
    if (tests.find ('b') != std::string::npos)
    {
        Score b;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO }) for (double c : { 165.0, 247.0 }) for (int sh : { 7, -7 }) scoreFormants (b, *v, c, sh);
        std::printf ("big shifts: real move %.2f%% (8), peak flatter %.2f dB (9), level %.2f\n", 100 * b.moveReal, b.peakFlatter, b.level);
    }
    if (tests.find ('d') != std::string::npos)
    {
        // dev stand in for the high held out voices: all six dev vowels at 440 to 660, +/-2 and +/-3
        Score h;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe }) for (double c : { 440.0, 500.0, 600.0, 660.0 }) for (int sh : { 3, -3, 2, -2 }) scoreFormants (h, *v, c, sh);
        std::printf ("dev high glides: crossed flatter %.2f dB (9), all %.2f, sharper %.2f (6), move %.2f%% (floor %.2f + 2), level %.2f\n", h.peakFlatterCrossed, h.peakFlatter, h.peakSharper, 100 * h.move, 100 * h.floor, h.level);
    }
    if (tests.find ('l') != std::string::npos)
    {
        Score s1; const double c = getenv ("C") ? atof (getenv ("C")) : 110.0; const int sh = getenv ("SH") ? atoi (getenv ("SH")) : 3;
        scoreFormants (s1, getenv ("VI") ? voice::vowelI : getenv ("VE") ? voice::vowelE : getenv ("VU") ? voice::vowelU : getenv ("VO") ? voice::vowelO : voice::vowelA, c, sh);
        std::printf ("one case: move %.2f%% flat %.2f peak %.2f level %.2f\n", 100 * s1.move, s1.flatter, s1.peakFlatter, s1.level);
    }
    if (tests.find ('v') != std::string::npos)
    {
        // Vowel change in the middle of a glide: after it, the output should match the output for the new vowel alone.
        double worst = 0, worstPeak = 0;
        for (auto pair : { std::pair<const voice::Vowel*, const voice::Vowel*> { &voice::vowelA, &voice::vowelI }, { &voice::vowelI, &voice::vowelA }, { &voice::vowelU, &voice::vowelE }, { &voice::vowelO, &voice::vowelAe } })
            for (double c : { 147.0, 440.0 }) for (int sh : { 3, -3 })
            {
                const auto f0 = glideF0 (c); const double r = std::pow (2.0, sh / 12.0);
                const auto x1 = styledVoice (f0, *pair.first, {}), x2 = styledVoice (f0, *pair.second, {});
                std::vector<float> x (x1.size());
                const size_t at = (size_t) (3.0 * SR), fade = (size_t) (0.03 * SR);
                for (size_t i = 0; i < x.size(); ++i) { const double t = i < at ? 0.0 : i >= at + fade ? 1.0 : (double) (i - at) / fade; x[i] = (float) ((1 - t) * x1[i] + t * x2[i]); }
                Estimator* e1 = gMake (*pair.second, {}); auto mixed = gShifter.run (x, f0, [r] (double) { return r; }, 1.0, *e1);
                if (gDump) if (auto* cc = dynamic_cast<Curves*> (e1)) std::printf ("    resets %d max mismatch %.2f\n", cc->mem.resets, cc->mem.maxMismatch);
                delete e1;
                Estimator* e2 = gMake (*pair.second, {}); auto alone = gShifter.run (x2, f0, [r] (double) { return r; }, 1.0, *e2); delete e2;
                for (double from : { 3.15, 3.5 })
                {
                    const long a = (long) (from * SR), n = (long) (0.5 * SR);
                    const double sm = std::max (c, c * r);
                    // short windows: use a 2048 point reading by zero padding is not available, so take 0.5 s
                    const auto sa = smoothSpectrumDb (alone.data() + a, n, SR, sm), sb = smoothSpectrumDb (mixed.data() + a, n, SR, sm);
                    const auto& v = *pair.second;
                    for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
                    { const double m = std::abs (shapeScale (sa, sb, band.first, band.second) - 1.0); worst = std::max (worst, m); if (gDump) std::printf ("    %s to %s %.0f %+d from %.2f: move %.2f%%\n", pair.first->name, v.name, c, sh, from, 100 * m); }
                    double d = 0; int cnt = 0; for (double hz = 300; hz < 4000; hz *= 1.02) { const double q = sb[(size_t) hz] - sa[(size_t) hz]; d += q; ++cnt; }
                    const double mean = d / cnt; double ss = 0; for (double hz = 300; hz < 4000; hz *= 1.02) { const double q = sb[(size_t) hz] - sa[(size_t) hz] - mean; ss += q * q; }
                    worstPeak = std::max (worstPeak, std::sqrt (ss / cnt));
                }
            }
        std::printf ("vowel change: worst formant move against the new vowel alone %.2f%%, worst colour distance %.2f dB\n", 100 * worst, worstPeak);
    }
    if (tests.find ('n') != std::string::npos)
    {
        double sum = 0; int count = 0;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe }) for (double c : { 440.0, 500.0, 600.0, 660.0 }) for (int sh : { 3, -3, 2, -2 })
        { const double e = heldNote (*v, c, sh); sum += e; ++count; if (gDump) std::printf ("    held %s %.0f %+d: %.2f\n", v->name, c, sh, e); }
        std::printf ("dev held notes: mean %.2f dB (3.5)\n", sum / count);
    }
    return 0;
}
