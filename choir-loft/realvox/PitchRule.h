#pragma once
// RealVox's pitch reading, in a header so the plug-in's own tests can check it (round 6c). Included by RealVox.cpp and tests/CheckerTests.h.
#include <algorithm>
#include <cmath>
#include <vector>

namespace realvox
{
// Normalised autocorrelation around a moment, for every lag a search within 8 semitones of an expected pitch can reach. Fills r (index is the
// lag) and returns the best value inside the range, or -2 when the window does not fit.
inline double pitchCurve (const std::vector<float>& y, long centre, double aroundHz, double sr, int& tMin, int& tMax, std::vector<double>& r)
{
    const double lo = aroundHz * std::pow (2.0, -8.0 / 12.0), hi = aroundHz * std::pow (2.0, 8.0 / 12.0);
    tMin = std::max (4, (int) std::floor (sr / hi));
    tMax = (int) std::ceil (sr / lo);
    const int W = 2 * tMax, stride = std::max (1, tMax / 150);
    const long start = centre - (W + tMax) / 2;
    if (start < 0 || start + W + tMax + 2 >= (long) y.size()) return -2.0;
    const float* a = y.data() + start;
    double ea = 0.0;
    for (int i = 0; i < W; i += stride) ea += (double) a[i] * a[i];
    r.assign ((size_t) tMax + 3, -2.0);
    double best = -2.0;
    for (int lag = tMin - 1; lag <= tMax + 1; ++lag)
    {
        double c = 0.0, eb = 0.0;
        for (int i = 0; i < W; i += stride) { c += (double) a[i] * a[i + lag]; eb += (double) a[i + lag] * a[i + lag]; }
        const double den = std::sqrt (ea * eb);
        r[(size_t) lag] = den > 1.0e-12 ? c / den : 0.0;
        if (lag >= tMin && lag <= tMax) best = std::max (best, r[(size_t) lag]);
    }
    return best;
}

inline double refinedHz (const std::vector<double>& r, int lag, double sr)
{
    const double p0 = r[(size_t) lag - 1], p1 = r[(size_t) lag], p2 = r[(size_t) lag + 1], den = p0 - 2.0 * p1 + p2;
    return sr / (lag + (std::abs (den) > 1.0e-12 ? 0.5 * (p0 - p2) / den : 0.0));
}

// The rule up to round 6 (and still the onset test's freePitch): the first local peak, from the shortest lag, that reaches 0.9 of the best.
// 0 if nothing periodic. Kept so every old record can be made again (RealVox: REALVOX_OLD_PITCH_RULE=1).
inline double pitchAroundFirstPeak (const std::vector<float>& y, long centre, double aroundHz, double sr = 48000.0)
{
    int tMin = 0, tMax = 0;
    std::vector<double> r;
    const double best = pitchCurve (y, centre, aroundHz, sr, tMin, tMax, r);
    if (best < 0.5) return 0.0;
    for (int lag = tMin; lag <= tMax; ++lag)
        if (r[(size_t) lag] >= 0.9 * best && r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1])
            return refinedHz (r, lag, sr);
    return 0.0;
}

// The rule from round 6c (Luke, 2026-11-16: "Yes, fix checker"). The same shortest strong period first, but a period is a whole peak of the
// curve, not every ripple on it. On low notes that are mostly fundamental the peak at the period is broad, and a ripple from the voice's
// high harmonics sits on its shoulder about 10 samples short, at 0.90 to 0.95 of the best: the first-peak rule took that ripple and read the
// note 40 to 60 cents sharp, on Luke's dry takes as on the voices. So the curve is cut into lobes where it stays above half the best (two
// periods a whole period apart are always split: halfway between them a periodic voice correlates with itself half a period out), and the
// first lobe whose highest point reaches 0.9 of the best is read at that highest point. A voice that really is off its note still reads
// off: its own peak is the top of its lobe.
inline double pitchAround (const std::vector<float>& y, long centre, double aroundHz, double sr = 48000.0)
{
    int tMin = 0, tMax = 0;
    std::vector<double> r;
    const double best = pitchCurve (y, centre, aroundHz, sr, tMin, tMax, r);
    if (best < 0.5) return 0.0;
    for (int lag = tMin; lag <= tMax;)
    {
        if (r[(size_t) lag] < 0.5 * best) { ++lag; continue; }
        int top = -1;
        for (; lag <= tMax && r[(size_t) lag] >= 0.5 * best; ++lag)
            if (r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1] && (top < 0 || r[(size_t) lag] > r[(size_t) top]))
                top = lag;
        if (top >= 0 && r[(size_t) top] >= 0.9 * best)
            return refinedHz (r, top, sr);
    }
    return 0.0;
}
} // namespace realvox
