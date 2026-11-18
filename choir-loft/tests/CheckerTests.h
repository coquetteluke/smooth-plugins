#pragma once
// Round 6c: the real vocal checker's pitch reading (realvox/PitchRule.h, RealVox's pitchAround). Luke, 2026-11-16: "Yes, fix checker".
// Included by PluginTest.cpp inside its namespace, after Round5Tests.h. Run alone: PluginTest checker. Synthetic signals only.
#include "../realvox/PitchRule.h"

namespace checker
{
using namespace phase2;

inline double centsOff (double hz, double wantHz) { return hz > 0.0 ? 1200.0 * std::log2 (hz / wantHz) : 99999.0; }

// A sum of harmonics of one fundamental: { harmonic number, amplitude }.
inline std::vector<float> harmonics (double f0, std::initializer_list<std::pair<int, double>> parts, double seconds)
{
    std::vector<float> x ((size_t) (seconds * SR));
    for (size_t i = 0; i < x.size(); ++i)
    {
        double s = 0.0;
        for (const auto& [k, a] : parts) s += a * std::sin (2.0 * M_PI * k * f0 * (double) i / SR + 0.3 * k);
        x[i] = (float) (0.1 * s);
    }
    return x;
}

inline std::vector<float> vowelOf (double f0, const voice::Vowel& v, double seconds = 1.0)
{
    return voice::synth (voice::steady (f0, seconds, SR), v, SR, -80.0, -18.0);
}

inline void testPitchRule()
{
    const long mid = (long) (0.5 * SR);

    // 1. The fault's shape. A low voice that is mostly fundamental, with a little energy in one high harmonic: the peak of the
    // autocorrelation at the period is broad, and the high harmonic puts a ripple on its shoulder 10 samples short of the period. On Luke's
    // dry take 02 at 61.935 to 62.030 s that ripple is 10 samples short at 0.90 to 0.95 of the best, and the first-peak rule read the note
    // 44 to 61 cents sharp. Here the period is exactly 350 samples and the 35th harmonic repeats every 10.
    {
        const double f0 = SR / 350.0;
        const auto x = harmonics (f0, { { 1, 1.0 }, { 2, 0.9 }, { 3, 0.7 }, { 35, 0.3 } }, 1.0);
        int tMin = 0, tMax = 0;
        std::vector<double> r;
        const double best = realvox::pitchCurve (x, mid, f0, SR, tMin, tMax, r);
        int shoulder = -1;
        for (int lag = 335; lag <= 345; ++lag)
            if (r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1]) shoulder = lag;
        const double share = shoulder > 0 ? r[(size_t) shoulder] / best : 0.0;
        check (shoulder > 0 && share >= 0.90 && share <= 0.95, "checker (guard): the shape, a ripple 8 to 12 samples short of the period at 0.90 to 0.95 of the best (share)", share);
        check (std::abs (centsOff (realvox::pitchAround (x, mid, f0, SR), f0)) <= 5.0,
               "checker 1: a broad peak with a ripple on its shoulder reads the period (cents off, 5 or less)", centsOff (realvox::pitchAround (x, mid, f0, SR), f0));
    }

    // 2. Clean voices from 70 to 400 Hz, the search centred anywhere within 7 semitones of the voice.
    {
        double worst = 0.0;
        std::string where;
        for (const double f0 : { 70.0, 100.0, 140.0, 200.0, 280.0, 400.0 })
            for (const auto* v : { &voice::vowelA, &voice::vowelI, &voice::vowelU, &voice::vowelE, &voice::vowelO })
            {
                const auto x = vowelOf (f0, *v);
                for (const int off : { -7, -4, 0, 4, 7 })
                {
                    const double c = std::abs (centsOff (realvox::pitchAround (x, mid, f0 * std::pow (2.0, off / 12.0), SR), f0));
                    if (c > worst) { worst = c; where = std::to_string ((int) f0) + " Hz " + v->name + " searched " + std::to_string (off) + " st"; }
                }
            }
        check (worst <= 5.0, "checker 2: clean voices 70 to 400 Hz, search centre within 7 semitones, worst (cents off, 5 or less; " + where + ")", worst);
    }

    // 3. No octave down. A voice at 150 Hz with a little of 75 Hz in it, so every other period differs: the peak at twice the period is
    // the stronger one. The search reaches both periods. It must read the period.
    {
        const double f0 = 150.0;
        auto x = vowelOf (f0, voice::vowelA);
        for (size_t i = 0; i < x.size(); ++i) x[i] += (float) (0.004 * std::sin (2.0 * M_PI * 75.0 * (double) i / SR));
        int tMin = 0, tMax = 0;
        std::vector<double> r;
        realvox::pitchCurve (x, mid, f0 / 1.5, SR, tMin, tMax, r);
        const double atOne = r[320], atTwo = r[640];
        check (atTwo >= atOne && atTwo <= atOne + 0.05 && tMin <= 320 && tMax >= 640,
               "checker (guard): the peak at twice the period is as strong or a little stronger (difference), and both are searched", atTwo - atOne);
        check (std::abs (centsOff (realvox::pitchAround (x, mid, f0 / 1.5, SR), f0)) <= 5.0,
               "checker 3: the period wins over twice the period (cents off, 5 or less)", centsOff (realvox::pitchAround (x, mid, f0 / 1.5, SR), f0));
    }

    // 4. A real octave up is still reported. Expected 110 Hz, the voice sings 220 Hz. Searched an octave up, as the verdicts' octave
    // check does, and searched 7 semitones up, where both 110 and 220 Hz are in reach.
    {
        const double expected = 110.0;
        const auto x = vowelOf (2.0 * expected, voice::vowelA);
        const double a = centsOff (realvox::pitchAround (x, mid, 2.0 * expected, SR), 2.0 * expected);
        const double b = centsOff (realvox::pitchAround (x, mid, expected * std::pow (2.0, 7.0 / 12.0), SR), 2.0 * expected);
        check (std::abs (a) <= 5.0 && std::abs (b) <= 5.0, "checker 4: a voice an octave up reads an octave up (cents off 220 Hz, worst of two searches, 5 or less)",
               std::max (std::abs (a), std::abs (b)));
    }

    // 5. A voice that really is sharp reads sharp. Expected period 350 samples, the voice's is 340, with no ripple.
    {
        const double expected = SR / 350.0, sung = SR / 340.0;
        double worst = 0.0;
        for (const auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI })
            worst = std::max (worst, std::abs (centsOff (realvox::pitchAround (vowelOf (sung, *v), mid, expected, SR), sung)));
        const auto clean = harmonics (sung, { { 1, 1.0 }, { 2, 0.75 }, { 3, 0.5 } }, 1.0);
        worst = std::max (worst, std::abs (centsOff (realvox::pitchAround (clean, mid, expected, SR), sung)));
        check (worst <= 5.0, "checker 5: a voice 10 samples short of the expected period reads 10 samples short (cents off its own pitch, 5 or less)", worst);
    }
}
} // namespace checker

void testChecker()
{
    checker::testPitchRule();
}
