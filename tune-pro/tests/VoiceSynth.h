#pragma once
// Test material: a glottal source through formant filters, with vibrato, scoops, breath noise, and known true pitch.
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace voice
{
struct Vowel { double f[5]; double bw[5]; const char* name; };
inline const Vowel vowelA { { 730, 1090, 2440, 3400, 4500 }, { 70, 90, 120, 150, 200 }, "a" };
inline const Vowel vowelI { { 270, 2290, 3010, 3500, 4500 }, { 50, 90, 120, 150, 200 }, "i" };
inline const Vowel vowelU { { 300, 870, 2240, 3400, 4500 }, { 50, 80, 120, 150, 200 }, "u" };
inline const Vowel vowelE { { 530, 1840, 2480, 3400, 4500 }, { 60, 90, 120, 150, 200 }, "e" };
inline const Vowel vowelO { { 570, 840, 2410, 3400, 4500 }, { 60, 80, 120, 150, 200 }, "o" };
inline const Vowel vowelAe { { 660, 1720, 2410, 3400, 4500 }, { 70, 90, 120, 150, 200 }, "ae" };

struct Resonator
{
    double a1 = 0, a2 = 0, b0 = 1, y1 = 0, y2 = 0;
    void set (double hz, double bw, double sr)
    {
        const double r = std::exp (-M_PI * bw / sr), c = 2.0 * r * std::cos (2.0 * M_PI * hz / sr);
        a1 = c; a2 = -r * r; b0 = 1.0 - c + r * r; // unity at DC, the usual cascade form
        y1 = y2 = 0;
    }
    double process (double x) { const double y = b0 * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
};

// One sample per entry of f0 (Hz). breathDb is noise against the voice. level is the output RMS in dBFS.
inline std::vector<float> synth (const std::vector<double>& f0, const Vowel& v, double sr, double breathDb = -60.0, double levelDb = -18.0, unsigned seed = 3)
{
    constexpr int over = 4;
    const size_t n = f0.size();
    std::vector<float> out (n, 0.0f);
    std::mt19937 gen (seed);
    std::normal_distribution<double> gauss (0.0, 1.0);

    // Two second order low passes before dropping back from 4 times the rate
    const double osr = sr * over, w = 2.0 * M_PI * 0.42 * sr / osr, cw = std::cos (w), sw = std::sin (w);
    const double al1 = sw / (2.0 * 0.541), al2 = sw / (2.0 * 1.307);
    const double bA[3] = { (1 - cw) / 2 / (1 + al1), (1 - cw) / (1 + al1), (1 - cw) / 2 / (1 + al1) }, aA[2] = { -2 * cw / (1 + al1), (1 - al1) / (1 + al1) };
    const double bB[3] = { (1 - cw) / 2 / (1 + al2), (1 - cw) / (1 + al2), (1 - cw) / 2 / (1 + al2) }, aB[2] = { -2 * cw / (1 + al2), (1 - al2) / (1 + al2) };
    double z1a = 0, z2a = 0, z1b = 0, z2b = 0;

    Resonator res[5];
    for (int i = 0; i < 5; ++i) res[i].set (v.f[i], v.bw[i], sr);

    double phase = 0.0, lastFlow = 0.0, noiseState = 0.0;
    constexpr double tp = 0.42, tn = 0.16;
    std::vector<double> dry (n), noise (n);
    for (size_t i = 0; i < n; ++i)
    {
        double y = 0.0;
        for (int o = 0; o < over; ++o)
        {
            phase += f0[i] / osr;
            if (phase >= 1.0) phase -= 1.0;
            double flow = 0.0;
            if (phase < tp) flow = 0.5 * (1.0 - std::cos (M_PI * phase / tp));
            else if (phase < tp + tn) flow = std::cos (0.5 * M_PI * (phase - tp) / tn);
            const double x = (flow - lastFlow) * osr / sr; // flow derivative: the excitation
            lastFlow = flow;
            const double ya = bA[0] * x + z1a; z1a = bA[1] * x - aA[0] * ya + z2a; z2a = bA[2] * x - aA[1] * ya;
            y = bB[0] * ya + z1b; z1b = bB[1] * ya - aB[0] * y + z2b; z2b = bB[2] * ya - aB[1] * y;
        }
        for (auto& r : res) y = r.process (y);
        dry[i] = y;
        noiseState += 0.55 * (gauss (gen) - noiseState); // breath: lightly low passed noise
        noise[i] = noiseState;
    }

    const auto rmsOf = [] (const std::vector<double>& x)
    {
        double s = 0; for (double v2 : x) s += v2 * v2; return std::sqrt (s / std::max<size_t> (x.size(), 1));
    };
    const double voiceRms = rmsOf (dry), noiseRms = rmsOf (noise);
    const double gain = std::pow (10.0, levelDb / 20.0) / std::max (voiceRms, 1.0e-12);
    const double noiseGain = voiceRms * std::pow (10.0, breathDb / 20.0) / std::max (noiseRms, 1.0e-12);
    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (gain * (dry[i] + noiseGain * noise[i]));
    return out;
}

inline std::vector<double> steady (double hz, double seconds, double sr)
{
    return std::vector<double> ((size_t) (seconds * sr), hz);
}

inline std::vector<double> vibrato (double hz, double seconds, double sr, double depthCents, double rateHz, double startSec = 0.0)
{
    std::vector<double> f ((size_t) (seconds * sr));
    for (size_t i = 0; i < f.size(); ++i)
    {
        const double t = (double) i / sr;
        const double d = t < startSec ? 0.0 : depthCents * std::sin (2.0 * M_PI * rateHz * (t - startSec));
        f[i] = hz * std::pow (2.0, d / 1200.0);
    }
    return f;
}

// Notes with glides, each with a scoop up at its start.
inline std::vector<double> phrase (const std::vector<double>& notesHz, double noteSec, double sr, double scoopCents = -150.0, double scoopSec = 0.08)
{
    std::vector<double> f;
    f.reserve ((size_t) (notesHz.size() * noteSec * sr));
    for (double hz : notesHz)
    {
        const size_t len = (size_t) (noteSec * sr);
        for (size_t i = 0; i < len; ++i)
        {
            const double t = (double) i / sr;
            const double c = t < scoopSec ? scoopCents * (1.0 - t / scoopSec) * (1.0 - t / scoopSec) : 0.0;
            f.push_back (hz * std::pow (2.0, c / 1200.0));
        }
    }
    return f;
}
} // namespace voice
