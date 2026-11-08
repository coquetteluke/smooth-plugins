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

