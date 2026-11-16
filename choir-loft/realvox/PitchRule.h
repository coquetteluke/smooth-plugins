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

// Pitch within 8 semitones of an expected one, shortest strong period first: the first local peak, from the shortest lag, that reaches 0.9
// of the best. 0 if nothing periodic. Same rule as the onset test's freePitch.
inline double pitchAround (const std::vector<float>& y, long centre, double aroundHz, double sr = 48000.0)
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

} // namespace realvox
