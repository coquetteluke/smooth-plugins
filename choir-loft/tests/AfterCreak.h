#pragma once
// A word that ends in a creak an octave down, then the next word on the same note (1.5.1).
// The end of a word often drops into a creak: every other pulse of the voice goes weak, so for a few tens of ms the sound repeats every
// two periods and hardly at all every one. Behind a doubler this happens cleanly and often. Since 1.5.0 Alto/Tenor follows periods under
// its range, so it follows the creak down, which is right. What was wrong: the next word, on the same note, 130 Hz or more, starts with
// its pulses still a little uneven, and the tracker stayed an octave down for its first 30 to 40 ms. On Luke's takes behind Duo, 1.5.0
// sang 13 voice notes of 896 above 130 Hz an octave low where 1.4.4 sang 7.
// Depends only on the tracker, so the same file can be compiled against an older PitchTracker.h to show the fault.
#include "../src/PitchTracker.h"
#include <cmath>
#include <vector>

namespace aftercreak
{
struct Formant
{
    double a1 = 0, a2 = 0, b0 = 1, y1 = 0, y2 = 0;
    void set (double hz, double bw, double sr)
    {
        const double r = std::exp (-M_PI * bw / sr), c = 2.0 * r * std::cos (2.0 * M_PI * hz / sr);
        a1 = c; a2 = -r * r; b0 = 1.0 - c + r * r;
    }
    double process (double x) { const double y = b0 * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
};

// How many ms the tracker reads an octave or more under the note once the new word is plainly on it.
// The lead: 300 ms of an open vowel at hz, 60 ms of creak (every other pulse at 0.15 of the others), then the same note coming back
// without a break, the weak pulses growing from 0.15 to full over 80 ms. From where they pass half (33 ms in) the sound repeats every
// period well enough to be called a note there (a difference under 0.2), and the count runs from then to 150 ms in.
// Alto/Tenor's range, the default Tracking.
inline double lowMs (double hz, double sr = 48000.0)
{
    const double period = sr / hz;
    const long nA = (long) (0.3 * sr), nB = (long) (0.06 * sr), nC = (long) (0.4 * sr), n = nA + nB + nC;
    std::vector<double> flow ((size_t) n, 0.0);
    long count = 0;
    for (double at = 10.0; at < (double) n - 2.0; at += period, ++count)
    {
        const long i = (long) at;
        const bool odd = (count & 1) != 0;
        double amp = 1.0;
        if (i >= nA && i < nA + nB) amp = odd ? 0.15 : 1.0;
        else if (i >= nA + nB && i < nA + nB + (long) (0.08 * sr)) amp = odd ? 0.15 + 0.85 * (double) (i - nA - nB) / (0.08 * sr) : 1.0;
        const double frac = at - (double) i;
        flow[(size_t) i] += amp * (1.0 - frac);
        flow[(size_t) i + 1] += amp * frac;
    }
    static const double f[5] = { 730, 1090, 2440, 3400, 4500 }, bw[5] = { 70, 90, 120, 150, 200 };   // an open a
    Formant r[5];
    for (int k = 0; k < 5; ++k) r[k].set (f[k], bw[k], sr);
    std::vector<float> x ((size_t) n);
    double e = 0.0;
    for (long i = 0; i < n; ++i)
    {
        double v = flow[(size_t) i];
        for (auto& k : r) v = k.process (v);
        x[(size_t) i] = (float) v;
        if (i < nA) e += v * v;
    }
    const float g = (float) (std::pow (10.0, -18.0 / 20.0) / std::sqrt (e / (double) nA));
    for (auto& v : x) v *= g;

    smoothharmony::PitchTracker t;
    t.prepare (sr);
    t.setRange (98.0, 1000.0, 65.0);   // Alto/Tenor since 1.5.0
    t.setVoicingLimit (0.30f);
    const long from = nA + nB + (long) (0.033 * sr), to = nA + nB + (long) (0.15 * sr), tick = (long) (0.001 * sr);
    double low = 0.0;
    for (long i = 0; i < n; ++i)
    {
        t.push (x[(size_t) i]);
        if (i >= from && i < to && (i - from) % tick == 0 && t.period() > 0.0 && sr / t.period() < 0.75 * hz)
            low += 1.0;
    }
    return low;
}
} // namespace aftercreak
