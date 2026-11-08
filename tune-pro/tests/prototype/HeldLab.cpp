// Build: clang++ -O3 -std=c++17 -I tests tests/prototype/HeldLab.cpp -o build/HeldLab
// Run: build/HeldLab <estimator> [set] where set is dev (the six dev vowels, plain voice) or devstyled (the same vowels with breath and
// darker sources). Offline lab for held notes in the harmonic shifter: the shifter keeps each harmonic's own level and only adds the
// vocal tract change, so the held note score is decided by how well the gain per harmonic, tract(k f_out) - tract(k f_in), is guessed
// from the input's harmonic levels alone. This reads that guess against the truth with the same score as the held note test
// (mean removed RMS over output harmonics up to 4 kHz), without running the shifter, so many estimates can be tried quickly.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>
#include "VoiceSynth.h"
#include "../../src/HeldEnvelope.h"

static const double SR = 48000.0;
struct VStyle { double breathDb = -60.0, softAboveHz = 0.0; };

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
std::vector<float> styledVoice (const std::vector<double>& f0, const voice::Vowel& v, const VStyle& st)
{
    auto x = voice::synth (f0, v, SR, std::getenv ("BREATH") ? std::atof (std::getenv ("BREATH")) : st.breathDb);
    if (st.softAboveHz > 0.0)
    {
        const double a = std::exp (-2.0 * M_PI * st.softAboveHz / SR);
        double y = 0.0;
        for (auto& s : x) { y = (1.0 - a) * s + a * y; s = (float) y; }
    }
    return x;
}

// Harmonic levels as the shifter sees them: Hann over 4 periods, one frame per period, power averaged over 0.5 to 1.5 s when
// average is set, else every frame kept.
struct Measured { std::vector<std::vector<double>> frames; std::vector<double> mean, noise; };
Measured measure (const std::vector<float>& x, double f0, int K)
{
    Measured m;
    const double P = SR / f0;
    const int len = (int) std::floor (4.0 * P);
    std::vector<double> pw ((size_t) K + 2, 0.0), nz ((size_t) K + 2, 0.0);
    int frames = 0;
    for (double start = 0.5 * SR; start + len < 1.5 * SR; start += 2.0 * P)
    {
        const long s0 = (long) start;
        std::vector<double> lev ((size_t) K + 2, -200.0);
        auto dft = [&] (double hz)
        {
            double re = 0, im = 0, ws = 0; const double w = 2.0 * M_PI * hz / SR;
            for (int i = 0; i < len; ++i) { const double h = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / len); re += h * x[(size_t) (s0 + i)] * std::cos (w * i); im += h * x[(size_t) (s0 + i)] * std::sin (w * i); ws += h; }
            return (re * re + im * im) / (ws * ws);
        };
        for (int k = 1; k <= K; ++k)
        {
            const double p = dft (k * f0);
            lev[(size_t) k] = 10.0 * std::log10 (p + 1e-30);
            pw[(size_t) k] += p;
            nz[(size_t) k] += 0.5 * (dft ((k - 0.5) * f0) + dft ((k + 0.5) * f0));
        }
        m.frames.push_back (lev);
        ++frames;
    }
    m.mean.assign ((size_t) K + 2, -200.0); m.noise = m.mean;
    for (int k = 1; k <= K; ++k) { m.mean[(size_t) k] = 10.0 * std::log10 (pw[(size_t) k] / frames + 1e-30); m.noise[(size_t) k] = 10.0 * std::log10 (nz[(size_t) k] / frames + 1e-30); }
    return m;
}

// Closed phase linear prediction: within each period there is a stretch where the glottis is shut and the voice is only the vocal
// tract ringing on, which a linear recursion predicts exactly. Every window position within the period is tried over the given periods,
// and the one predicted best is kept. Returns the envelope in dB as a function of Hz.
struct CpResult { std::vector<double> a; double residual = 1.0, phase = 0.0; double fs = 48000.0; };
double envDb (const CpResult& c, double hz)
{
    double re = 0, im = 0; const double w = 2.0 * M_PI * hz / c.fs;
    for (size_t k = 0; k < c.a.size(); ++k) { re += c.a[k] * std::cos (k * w); im -= c.a[k] * std::sin (k * w); }
    return -10.0 * std::log10 (re * re + im * im + 1e-30);
}
bool solveSym (std::vector<double> A, std::vector<double> b, int n, std::vector<double>& x)
{
    // Cholesky with a little loading
    for (int i = 0; i < n; ++i) A[(size_t) (i * n + i)] *= 1.0 + 1e-9;
    std::vector<double> L ((size_t) n * n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j <= i; ++j)
        {
            double s = A[(size_t) (i * n + j)];
            for (int k = 0; k < j; ++k) s -= L[(size_t) (i * n + k)] * L[(size_t) (j * n + k)];
            if (i == j) { if (s <= 0) return false; L[(size_t) (i * n + i)] = std::sqrt (s); }
            else L[(size_t) (i * n + j)] = s / L[(size_t) (j * n + j)];
        }
    std::vector<double> y (n);
    for (int i = 0; i < n; ++i) { double s = b[(size_t) i]; for (int k = 0; k < i; ++k) s -= L[(size_t) (i * n + k)] * y[(size_t) k]; y[(size_t) i] = s / L[(size_t) (i * n + i)]; }
    x.assign (n, 0.0);
    for (int i = n - 1; i >= 0; --i) { double s = y[(size_t) i]; for (int k = i + 1; k < n; ++k) s -= L[(size_t) (k * n + i)] * x[(size_t) k]; x[(size_t) i] = s / L[(size_t) (i * n + i)]; }
    return true;
}
template <typename T> CpResult closedPhase (const std::vector<T>& x, double P, long start, int periods, int order, double width, int positions)
{
    CpResult best; best.residual = 1e30;
    for (int pos = 0; pos < positions; ++pos)
    {
        const double phi = (double) pos / positions;
        const int n1 = order + 1;
        std::vector<double> C ((size_t) n1 * n1, 0.0); double energy = 0;
        for (long n = start + order; n < start + (long) (periods * P); ++n)
        {
            double ph = std::fmod ((n - start) / P - phi, 1.0); if (ph < 0) ph += 1.0;
            if (ph >= width) continue;
            for (int i = 0; i <= order; ++i) for (int j = 0; j <= i; ++j) C[(size_t) (i * n1 + j)] += (double) x[(size_t) (n - i)] * x[(size_t) (n - j)];
            energy += (double) x[(size_t) n] * x[(size_t) n];
        }
        for (int i = 0; i <= order; ++i) for (int j = i + 1; j <= order; ++j) C[(size_t) (i * n1 + j)] = C[(size_t) (j * n1 + i)];
        std::vector<double> A ((size_t) order * order), b ((size_t) order), sol;
        for (int i = 1; i <= order; ++i) { b[(size_t) i - 1] = -C[(size_t) (i * n1)]; for (int j = 1; j <= order; ++j) A[(size_t) ((i - 1) * order + j - 1)] = C[(size_t) (i * n1 + j)]; }
        if (! solveSym (A, b, order, sol)) continue;
        double e = C[0]; for (int j = 1; j <= order; ++j) e += sol[(size_t) j - 1] * C[(size_t) j];
        const double rn = e / std::max (energy, 1e-30);
        if (std::getenv ("POS")) std::printf ("      pos %.3f rn %.3e\n", phi, rn);
        if (rn < best.residual) { best.residual = rn; best.phase = phi; best.a.assign (1, 1.0); for (double v : sol) best.a.push_back (v); }
    }
    return best;
}

std::vector<double> sourceMag;
void makeSource()
{
    const int N = 16384; std::vector<double> flow (N);
    for (int i = 0; i < N; ++i) { const double ph = (double) i / N; flow[i] = ph < 0.42 ? 0.5 * (1.0 - std::cos (M_PI * ph / 0.42)) : ph < 0.58 ? std::cos (0.5 * M_PI * (ph - 0.42) / 0.16) : 0.0; }
    sourceMag.assign (200, 0.0);
    for (int k = 1; k < 200; ++k) { double re = 0, im = 0; for (int i = 0; i < N; ++i) { re += flow[i] * std::cos (2 * M_PI * k * i / N); im -= flow[i] * std::sin (2 * M_PI * k * i / N); } sourceMag[(size_t) k] = k * std::sqrt (re * re + im * im) / N; }
}

// Formant model fit: level = G + s log2 k + five resonators. Bandwidths from bwOf (index, frequency). F5 fixed.
struct FormantFit
{
    std::function<double (int, double)> bwOf;
    double F5 = 4500.0, topHz = 5000.0, pull = 0.0, snrNeed = 10.0, tilt0 = -8.0, tiltW = 0.0, boundMargin = 3.0;
    double F[5] = { 500, 1500, 2500, 3500, 4500 };
    std::vector<double> f5Grid { 4500.0 };
    double lp = 0.0, lpTry = 0.0; std::vector<double> lpGrid { 0.0 };
    std::vector<double> hz, db, xs, cap; std::vector<int> good;
    static double onePoleDb (double f, double fc) { if (fc <= 0) return 0.0; const double a = std::exp (-2.0 * M_PI * fc / SR); return 20.0 * std::log10 ((1.0 - a) / std::abs (1.0 - a * std::polar (1.0, -2.0 * M_PI * f / SR))); }
    double model (double f, const double* FF) const
    {
        double m = onePoleDb (f, lpTry);
        for (int i = 0; i < 5; ++i) m += resonatorDb (f, FF[i], bwOf (i, FF[i]));
        return m;
    }
    double cost (const double* FF) const
    {
        for (int i = 1; i < 5; ++i) if (FF[i] < FF[i - 1] + 150.0) return 1e30;
        if (FF[0] < 150 || FF[4] > 5600) return 1e30;
        double sx = tiltW * 0, sy = 0, sxx = 0, sxy = 0, n = 0; std::vector<double> y (hz.size());
        for (size_t k = 0; k < hz.size(); ++k) { y[k] = db[k] - model (hz[k], FF); if (! good[k]) continue; sx += xs[k]; sy += y[k]; sxx += xs[k] * xs[k]; sxy += xs[k] * y[k]; n += 1; }
        if (n < 2) return 1e30;
        // minimise sum (y - g - s x)^2 + tiltW (s - tilt0)^2
        const double a11 = n, a12 = sx, a22 = sxx + tiltW, b1 = sy, b2 = sxy + tiltW * tilt0;
        const double det = a11 * a22 - a12 * a12;
        double ss = (a11 * b2 - a12 * b1) / std::max (1e-9, det);
        ss = std::clamp (ss, -24.0, 6.0);
        const double gg = (sy - ss * sx) / n;
        double c = tiltW * (ss - tilt0) * (ss - tilt0);
        for (size_t k = 0; k < hz.size(); ++k)
        {
            const double e = y[k] - gg - ss * xs[k];
            if (good[k]) c += e * e;
            else { const double over = db[k] - e - cap[k]; if (over > 0) c += over * over; }
        }
        static const double mu[4] = { 550, 1500, 2600, 3500 }, sg[4] = { 300, 700, 500, 500 };
        for (int i = 0; i < 4; ++i) { const double z = (FF[i] - mu[i]) / sg[i]; c += pull * z * z; }
        return c;
    }
    void fit (const double* lev, const double* noise, int K, double f0)
    {
        hz.clear(); db.clear(); xs.clear(); cap.clear(); good.clear(); pts.clear();
        for (int k = 1; k <= K && k * f0 <= topHz; ++k)
        {
            hz.push_back (k * f0); db.push_back (lev[k]); xs.push_back (std::log2 ((double) k));
            good.push_back (lev[k] - noise[k] >= snrNeed ? 1 : 0); cap.push_back (std::max (lev[k], noise[k] + boundMargin));
        }
        double best = 1e30, bestF[5] = { 500, 1500, 2500, 3500, 4500 }, bestLp = 0;
        for (double lpc : lpGrid)
        {
        lpTry = lpc;
        double bestHere = 1e30, bestFH[5] = { 500, 1500, 2500, 3500, 4500 };
        for (double f5 : f5Grid)
        for (double f4 : { 3300.0, 3600.0, 3900.0 })
        for (double f3 : { 2200.0, 2500.0, 2800.0, 3100.0 })
            for (double f1 = 200; f1 <= 1100; f1 += 20)
                for (double f2 = f1 + 150; f2 <= 2900 && f2 < f3 - 150; f2 *= 1.03)
                {
                    if (f4 > f5 - 200) continue;
                    double FF[5] = { f1, f2, f3, f4, f5 };
                    const double c = cost (FF);
                    if (c < bestHere) { bestHere = c; std::copy (FF, FF + 5, bestFH); }
                    if (c < 1e29) { Pt pt; pt.c = c; std::copy (FF, FF + 5, pt.f); pt.lp = lpc; pts.push_back (pt); }
                }
        for (double step : { 40.0, 15.0, 5.0, 2.0 })
            for (int pass = 0; pass < 3; ++pass)
                for (int i = 0; i < nFree; ++i)
                    for (int dir : { -1, 1 })
                        while (true)
                        {
                            double FF[5]; std::copy (bestFH, bestFH + 5, FF); FF[i] += dir * step;
                            const double c = cost (FF);
                            if (c < bestHere) { bestHere = c; std::copy (FF, FF + 5, bestFH); }
                    if (c < 1e29) { Pt pt; pt.c = c; std::copy (FF, FF + 5, pt.f); pt.lp = lpc; pts.push_back (pt); } else break;
                        }
        if (bestHere < best) { best = bestHere; std::copy (bestFH, bestFH + 5, bestF); bestLp = lpc; }
        }
        lp = lpTry = bestLp;
        std::copy (bestF, bestF + 5, F);
        bestCost = best;
    }
    double bestCost = 0; int nFree = 4;
    struct Pt { double c, f[5], lp; };
    std::vector<Pt> pts;
    // Gains averaged over every configuration tried, weighted by how well each fits: where two shapes fit about as well, hedge.
    void posterior (double fin, double fout, int K, double sigma2, double* g)
    {
        double minC = 1e30; for (auto& p : pts) minC = std::min (minC, p.c);
        std::vector<double> acc ((size_t) K + 1, 0.0); double wsum = 0;
        for (auto& p : pts)
        {
            const double w = std::exp (-(p.c - minC) / (2.0 * sigma2));
            if (w < 1e-4) continue;
            lpTry = p.lp;
            for (int k = 1; k <= K; ++k) acc[(size_t) k] += w * (model (k * fout, p.f) - model (k * fin, p.f));
            wsum += w;
        }
        lpTry = lp;
        for (int k = 1; k <= K; ++k) g[k] = acc[(size_t) k] / wsum;
    }
    double tract (double f) const { return model (f, F); }
};

double gCostSum = 0, gShapes = 0; int gCostN = 0;
struct Case { const voice::Vowel* v; VStyle st; double f0; int shift; };

int main (int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "lines";
    const std::string set = argc > 2 ? argv[2] : "dev";
    const bool dump = std::getenv ("DUMP") != nullptr, perFrame = std::getenv ("FRAMES") != nullptr;
    std::vector<Case> cases;
    std::vector<VStyle> styles { {} };
    if (set == "devstyled") styles = { { -40.0, 0.0 }, { -60.0, 2500.0 }, { -45.0, 4000.0 }, { -50.0, 3000.0 } };
    if (set == "devall") styles = { {}, { -40.0, 0.0 }, { -60.0, 2500.0 }, { -45.0, 4000.0 }, { -50.0, 3000.0 } };
    static std::vector<voice::Vowel> randomVowels;
    if (set.rfind ("rand", 0) == 0)
    {
        // Plausible vowels drawn at random, not from any table: a check on vowels nothing was chosen on. rand1 tunes, rand2 checks.
        std::mt19937 gen ((unsigned) std::atoi (set.c_str() + 4) * 7919u);
        std::uniform_real_distribution<double> u (0.0, 1.0);
        const int count = 40;
        randomVowels.resize (count);
        static std::vector<std::string> names (count);
        for (int i = 0; i < count; ++i)
        {
            auto& v = randomVowels[(size_t) i];
            v.f[0] = 250 + 600 * u (gen);
            v.f[1] = std::max (v.f[0] + 250, 700 + 1800 * u (gen));
            v.f[2] = std::max (v.f[1] + 250, 2200 + 900 * u (gen));
            v.f[3] = std::max (v.f[2] + 300, 3300 + 800 * u (gen));
            v.f[4] = std::max (v.f[3] + 400, 4300 + 800 * u (gen));
            v.bw[0] = 45 + 70 * u (gen); v.bw[1] = 60 + 70 * u (gen); v.bw[2] = 90 + 80 * u (gen); v.bw[3] = 120 + 100 * u (gen); v.bw[4] = 150 + 120 * u (gen);
            names[(size_t) i] = "r" + std::to_string (i); v.name = names[(size_t) i].c_str();
            VStyle st { -60.0 + 22.0 * u (gen), u (gen) < 0.5 ? 0.0 : 2000.0 + 3000.0 * u (gen) };
            const double c = 392.0 * std::pow (2.0, u (gen));
            for (int sh : { 3, -3, 2, -2 }) cases.push_back ({ &v, st, std::round (c), sh });
        }
    }
    else
    for (const auto& st : styles)
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe })
            for (double c : { 440.0, 500.0, 600.0, 660.0 })
                for (int sh : { 3, -3, 2, -2 })
                    cases.push_back ({ v, st, c, sh });
    std::map<int, double> byK; std::map<int, int> byKn; std::map<std::string, double> byVowel; std::map<std::string, int> byVowelN;
    double total = 0; int count = 0;
    std::map<std::string, std::vector<float>> cache;
    for (const auto& cs : cases)
    {
        const double fin = cs.f0, r = std::pow (2.0, cs.shift / 12.0), fout = fin * r;
        const int K = std::min (63, (int) (0.47 * SR / fin));
        char key[128]; std::snprintf (key, sizeof key, "%s %.0f %.0f %.0f", cs.v->name, cs.st.breathDb, cs.st.softAboveHz, fin);
        if (! cache.count (key)) cache[key] = styledVoice (voice::steady (fin, 1.6, SR), *cs.v, cs.st);
        static std::map<std::string, Measured> measCache;
        if (! measCache.count (key)) measCache[key] = measure (cache[key], fin, K);
        auto meas = measCache[key];
        if (std::getenv ("NSUB"))
            for (int k = 1; k <= K; ++k)
            {
                const double p = std::pow (10.0, 0.1 * meas.mean[(size_t) k]), q = std::pow (10.0, 0.1 * meas.noise[(size_t) k]);
                meas.mean[(size_t) k] = 10.0 * std::log10 (std::max (p - q, 0.25 * p));
            }
        if (std::getenv ("CMPLEV"))
        {
            if (sourceMag.empty()) makeSource();
            std::printf ("%s %.0f:", cs.v->name, fin);
            for (int k = 1; k <= std::min (K, 12); ++k) std::printf (" %+.2f", meas.mean[(size_t) k] - (20.0 * std::log10 (sourceMag[(size_t) k]) + tractDb (k * fin, *cs.v, cs.st)) - (meas.mean[1] - (20.0 * std::log10 (sourceMag[1]) + tractDb (fin, *cs.v, cs.st))));
            std::printf ("  noise k1 %.1f\n", meas.mean[1] - meas.noise[1]);
        }
        if (std::getenv ("IDEAL"))
        {
            if (sourceMag.empty()) makeSource();
            for (int k = 1; k <= K; ++k) { meas.mean[(size_t) k] = 20.0 * std::log10 (sourceMag[(size_t) k]) + tractDb (k * fin, *cs.v, cs.st); meas.noise[(size_t) k] = -300; }
        }
        std::vector<double> truth, est;
        int Kout = 0; while ((Kout + 1) * fout <= 4000.0) ++Kout;
        std::vector<double> g ((size_t) K + 2, 0.0);
        if (which == "lines")
            for (int k = 1; k <= K; ++k) g[(size_t) k] = heldenv::linesAt (meas.mean.data(), K, k * fout / fin) - meas.mean[(size_t) k];
        if (which == "fit")
        {
            static heldenv::FormantFit fitter;
            static std::string lastKey;
            auto env = [] (const char* n, double d) { return std::getenv (n) ? std::atof (std::getenv (n)) : d; };
            const heldenv::Settings d;
            fitter.settings.snrNeedDb = env ("SNRF", d.snrNeedDb); fitter.settings.tiltDb = env ("TILT0", d.tiltDb); fitter.settings.tiltWeight = env ("TILTW", d.tiltWeight);
            fitter.settings.spreadDb2 = env ("SIG2", d.spreadDb2); fitter.settings.bandwidthScale = env ("BWS", d.bandwidthScale); fitter.settings.topHz = env ("TOPF", d.topHz);
            if (std::getenv ("HIGHS")) fitter.settings.highStarts = std::atoi (std::getenv ("HIGHS"));
            bool ok = true;
            if (lastKey != key) { if (! std::getenv ("CARRY")) fitter.forget(); ok = fitter.fit (meas.mean.data(), meas.noise.data(), K, fin, SR); lastKey = key; if (ok) { gCostSum += fitter.bestShape().cost; gCostN++; gShapes += fitter.count(); } }
            if (ok && fitter.count() > 0) fitter.gains (fin, fout, K, g.data());
            else for (int k = 1; k <= K; ++k) g[(size_t) k] = heldenv::linesAt (meas.mean.data(), K, k * fout / fin) - meas.mean[(size_t) k];
            if (std::getenv ("BLEND1")) { const double w = env ("BLEND1", 1.0); g[1] = w * g[1] + (1 - w) * (heldenv::linesAt (meas.mean.data(), K, fout / fin) - meas.mean[1]); }
            if (std::getenv ("BLEND")) { const double w = env ("BLEND", 1.0); for (int k = 1; k <= K; ++k) g[(size_t) k] = w * g[(size_t) k] + (1 - w) * (heldenv::linesAt (meas.mean.data(), K, k * fout / fin) - meas.mean[(size_t) k]); }
            if (dump) { const auto& b = fitter.bestShape(); std::printf ("   fit F %.0f %.0f %.0f %.0f %.0f lp %.0f, %d shapes (true %.0f %.0f %.0f %.0f %.0f)\n", fitter.hzOf (b.j[0]), fitter.hzOf (b.j[1]), fitter.hzOf (b.j[2]), fitter.hzOf (b.j[3]), fitter.hzOf (b.j[4]), fitter.lowPassOf (b.lp), fitter.count(), cs.v->f[0], cs.v->f[1], cs.v->f[2], cs.v->f[3], cs.v->f[4]); }
        }
        if (which == "cp")
        {
            const int order = std::getenv ("ORDER") ? std::atoi (std::getenv ("ORDER")) : 14;
            const double width = std::getenv ("WIDTH") ? std::atof (std::getenv ("WIDTH")) : 0.3;
            const int periods = std::getenv ("PERIODS") ? std::atoi (std::getenv ("PERIODS")) : 4;
            const int dec = std::getenv ("DEC") ? std::atoi (std::getenv ("DEC")) : 1;
            const int taps = std::getenv ("TAPS") ? std::atoi (std::getenv ("TAPS")) : 12;
            const double cut = std::getenv ("CUT") ? std::atof (std::getenv ("CUT")) : 5500.0;
            std::vector<double> y;
            const auto& xx = cache[key];
            if (dec > 1)
            {
                for (size_t m = (size_t) taps; m * dec + taps < xx.size(); ++m)
                {
                    double s2 = 0, ws = 0;
                    for (int t = -taps; t <= taps; ++t)
                    {
                        const double u = t * 2.0 * cut / SR, sinc = t == 0 ? 1.0 : std::sin (M_PI * u) / (M_PI * u);
                        const double w = 0.5 + 0.5 * std::cos (M_PI * t / (taps + 1));
                        s2 += sinc * w * xx[m * dec + t]; ws += sinc * w;
                    }
                    y.push_back (s2 / ws);
                }
                y.insert (y.begin(), (size_t) taps, 0.0);
            }
            else y.assign (xx.begin(), xx.end());
            auto cp = closedPhase (y, SR / fin / dec, (long) (0.8 * SR / dec), periods, order, width, 32);
            cp.fs = SR / dec;
            for (int k = 1; k <= K; ++k) g[(size_t) k] = envDb (cp, k * fout) - envDb (cp, k * fin);
            if (dump) std::printf ("   cp residual %.2e phase %.2f\n", cp.residual, cp.phase);
            if (std::getenv ("ENV")) { const double off = envDb (cp, 100) - tractDb (100, *cs.v, cs.st); for (double hz = 200; hz <= 5000; hz += 200) std::printf ("     %5.0f Hz: est %+6.1f true %+6.1f\n", hz, envDb (cp, hz) - off, tractDb (hz, *cs.v, cs.st)); }
        }
        if (which == "fbtrue" || which == "fbpub" || which == "fbtable")
        {
            static std::map<std::string, FormantFit> fits;
            const bool have = fits.count (key) > 0;
            FormantFit& ff = fits[key];
            const voice::Vowel* vv = cs.v;
            if (which == "fbtrue") ff.bwOf = [vv] (int i, double) { return vv->bw[i]; };
            else if (which == "fbtable") ff.bwOf = [] (int i, double f) { static const double b[5] = { 0, 85, 120, 150, 200 }; return i == 0 ? 50.0 + 20.0 * std::clamp ((f - 270.0) / 460.0, 0.0, 1.0) : b[i]; };
            else { const double bs = std::getenv ("BWS") ? std::atof (std::getenv ("BWS")) : 1.0; ff.bwOf = [bs] (int, double f) { return bs * (f < 1000 ? 60.0 + 0.03 * f : 50.0 + 0.04 * f); }; }
            if (std::getenv ("F5")) ff.f5Grid = { std::atof (std::getenv ("F5")) };
            if (std::getenv ("F5FREE")) { ff.f5Grid = { 4100.0, 4500.0, 4900.0 }; ff.nFree = 5; }
            if (which == "fbtrue") ff.f5Grid = { cs.v->f[4] };
            if (std::getenv ("PULL")) ff.pull = std::atof (std::getenv ("PULL"));
            if (std::getenv ("TILTW")) ff.tiltW = std::atof (std::getenv ("TILTW")); if (std::getenv ("TILT0")) ff.tilt0 = std::atof (std::getenv ("TILT0"));
            if (std::getenv ("SNRF")) ff.snrNeed = std::atof (std::getenv ("SNRF")); if (std::getenv ("TOPF")) ff.topHz = std::atof (std::getenv ("TOPF"));
            if (std::getenv ("LPGRID")) ff.lpGrid = { 0.0, 1500.0, 2200.0, 3200.0, 4500.0 };
            if (! have) ff.fit (meas.mean.data(), meas.noise.data(), K, fin);
            for (int k = 1; k <= K; ++k) g[(size_t) k] = ff.tract (k * fout) - ff.tract (k * fin);
            if (std::getenv ("SIG2")) ff.posterior (fin, fout, K, std::atof (std::getenv ("SIG2")), g.data());
            if (std::getenv ("BLEND"))
            {
                const double w = std::atof (std::getenv ("BLEND"));
                for (int k = 1; k <= K; ++k) g[(size_t) k] = w * g[(size_t) k] + (1 - w) * (heldenv::linesAt (meas.mean.data(), K, k * fout / fin) - meas.mean[(size_t) k]);
            }
            if (std::getenv ("OPTCHECK") && ! have)
            {
                double tf[5]; for (int i = 0; i < 5; ++i) tf[i] = cs.v->f[i];
                const double save = ff.lpTry; ff.lpTry = cs.st.softAboveHz;
                const double ct = ff.cost (tf); ff.lpTry = save;
                std::printf ("   OPT %s found cost %.2f, true params cost %.2f %s\n", key, ff.bestCost, ct, ct < ff.bestCost - 0.01 ? "SEARCH MISSED" : "");
            }
            if (dump) std::printf ("   fit F %.0f %.0f %.0f %.0f %.0f lp %.0f (true %.0f %.0f %.0f %.0f %.0f)\n", ff.F[0], ff.F[1], ff.F[2], ff.F[3], ff.F[4], ff.lp, cs.v->f[0], cs.v->f[1], cs.v->f[2], cs.v->f[3], cs.v->f[4]);
        }
        if (which == "oracle") for (int k = 1; k <= K; ++k) g[(size_t) k] = tractDb (k * fout, *cs.v, cs.st) - tractDb (k * fin, *cs.v, cs.st);
        std::vector<double> d;
        double mean = 0;
        for (int k = 1; k <= Kout; ++k) { d.push_back (g[(size_t) k] - (tractDb (k * fout, *cs.v, cs.st) - tractDb (k * fin, *cs.v, cs.st))); mean += d.back(); }
        mean /= d.size();
        double s = 0; for (double x : d) s += (x - mean) * (x - mean);
        const double score = std::sqrt (s / d.size());
        for (int k = 1; k <= Kout; ++k) { byK[k] += (d[(size_t) k - 1] - mean) * (d[(size_t) k - 1] - mean); byKn[k]++; }
        byVowel[cs.v->name] += score; byVowelN[cs.v->name]++;
        char sk[32]; std::snprintf (sk, sizeof sk, "shift %+d", cs.shift); byVowel[sk] += score; byVowelN[sk]++;
        char stk[48]; std::snprintf (stk, sizeof stk, "style %.0f/%.0f", cs.st.breathDb, cs.st.softAboveHz); byVowel[stk] += score; byVowelN[stk]++;
        char fk[32]; std::snprintf (fk, sizeof fk, "f0 %.0f", fin); byVowel[fk] += score; byVowelN[fk]++;
        total += score; ++count;
        if (dump)
        {
            std::printf ("%-4s br %.0f soft %.0f f0 %.0f %+d: %.2f |", cs.v->name, cs.st.breathDb, cs.st.softAboveHz, fin, cs.shift, score);
            for (double x : d) std::printf (" %+.1f", x - mean);
            std::printf ("\n");
        }
    }
    if (std::getenv ("BREAK"))
    {
        for (auto& [k, v] : byK) std::printf ("  harmonic %d: rms %.2f (%d)\n", k, std::sqrt (v / byKn[k]), byKn[k]);
        for (auto& [k, v] : byVowel) std::printf ("  %s: %.2f\n", k.c_str(), v / byVowelN[k]);
    }
    if (gCostN) std::printf ("  mean best cost %.3f, mean shapes %.1f\n", gCostSum / gCostN, gShapes / gCostN);
    std::printf ("%s %s held notes: mean %.3f dB over %d\n", which.c_str(), set.c_str(), total / count, count);
    return 0;
}
