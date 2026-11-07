#pragma once
// Voice pitch tracker. Written to be copied into other plug-ins: it knows nothing about keys, scales or the host.
//
// Two stages, both in the time domain and both causal:
// 1. Coarse: a normalised difference function on a 12 kHz copy of the input, run every 1.3 ms. Picks the period, protects
//    against octave errors with a first deep dip rule plus a rule for staying on the current period, and decides voiced or not.
// 2. Fine: pitch marks at full rate. Each new period is found by matching the last period against the one before, so marks
//    land on the same point of the waveform every period with very little jitter. Those marks are what the shifter centres
//    its grains on, and the distance between them is the precise period.
//
// A mark is known 0.33 periods after it happens. That sets how much latency a PSOLA shifter needs. See requiredLatency().
// Everything is allocated in prepare(), so push() is real time safe.
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace smoothtune
{
inline float dotProduct (const float* a, const float* b, int n, int stride = 1) noexcept
{
    float r = 0.0f;
    vDSP_dotpr (a, stride, b, stride, &r, (vDSP_Length) n);
    return r;
}

// Second order filter sections for the analysis path only. Plain double precision, cleared if they ever go bad.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void lowPass (double hz, double sr, double q) noexcept
    {
        const double w = 2.0 * M_PI * hz / sr, c = std::cos (w), alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
        b0 = b2 = (1.0 - c) * 0.5 / a0; b1 = (1.0 - c) / a0; a1 = -2.0 * c / a0; a2 = (1.0 - alpha) / a0;
    }
    void highPass (double hz, double sr, double q) noexcept
    {
        const double w = 2.0 * M_PI * hz / sr, c = std::cos (w), alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
        b0 = b2 = (1.0 + c) * 0.5 / a0; b1 = -(1.0 + c) / a0; a1 = -2.0 * c / a0; a2 = (1.0 - alpha) / a0;
    }
    void clear() noexcept { z1 = z2 = 0.0; }
    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    bool bad() const noexcept { return ! std::isfinite (z1) || ! std::isfinite (z2); }
};

class PitchTracker
{
public:
    struct Mark
    {
        double position = 0.0;  // in input samples since prepare or reset
        double period = 0.0;    // samples to the mark before it
        float strength = 0.0f;  // match against the period before, 0 to 1
    };

    static constexpr int maxMarks = 256;
    static constexpr double markKnownAfterPeriods = 0.33;

    // Latency a shifter needs to place a grain on a mark for the longest period in the range, plus its interpolator's reach.
    static int requiredLatency (double sampleRate, double lowestHz, int interpolatorReach) noexcept
    {
        return (int) std::ceil ((1.0 + markKnownAfterPeriods) * sampleRate / lowestHz) + 1 + interpolatorReach;
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        decimation = std::max (1, (int) std::lround (sr / 12000.0));
        slowRate = sr / decimation;
        hop = std::max (1, (int) std::lround (slowRate * 0.00133));
        slowSize = 8192;
        slow.assign ((size_t) slowSize * 2, 0.0f);
        fastSize = 1;
        while (fastSize < (int) (sr * 0.25)) fastSize *= 2;
        fast.assign ((size_t) fastSize * 2, 0.0f);
        curve.assign (1024, 1.0f);
        rumble.highPass (50.0, sr, 0.707);
        presence.lowPass (std::min (5500.0, sr * 0.45), sr, 0.707);
        coarseA.lowPass (3000.0, sr, 0.541);
        coarseB.lowPass (3000.0, sr, 1.307);
        envAttack = 1.0 - std::exp (-1.0 / (0.0015 * sr));
        setRange (lowHz, highHz);
        reset();
    }

    // Clears tracking state, so a change of range starts clean. No allocation.
    void setRange (double lowestHz, double highestHz) noexcept
    {
        lowHz = lowestHz;
        highHz = highestHz;
        tauMin = std::max (2, (int) std::floor (slowRate / highHz));
        tauMax = std::min ((int) curve.size() - 3, (int) std::ceil (slowRate / lowHz) + 1);
        window = tauMax;
        periodMin = sr / highHz * 0.9;
        periodMax = sr / lowHz * 1.1;
        clearTracking();
    }

    // Upper limit on the difference function for calling a sound voiced: lower is choosier.
    void setVoicingLimit (float limit) noexcept { voicedOn = limit; voicedOff = limit + 0.08f; }

    void reset() noexcept
    {
        std::fill (slow.begin(), slow.end(), 0.0f);
        std::fill (fast.begin(), fast.end(), 0.0f);
        rumble.clear(); presence.clear(); coarseA.clear(); coarseB.clear();
        envFull = envLow = 0.0;
        count = 0; slowCount = 0;
        decimationPhase = 0; hopPhase = 0;
        clearTracking();
    }

    void push (float input) noexcept
    {
        const double x = rumble.process ((double) input);
        const double f = presence.process (x);
        const double low = coarseB.process (coarseA.process (x));
        if (rumble.bad() || presence.bad() || coarseA.bad() || coarseB.bad())
        {
            rumble.clear(); presence.clear(); coarseA.clear(); coarseB.clear();
            return;
        }
        envFull += envAttack * (x * x - envFull);
        envLow += envAttack * (low * low - envLow);

        const int at = (int) (count & (long long) (fastSize - 1));
        fast[(size_t) at] = fast[(size_t) (at + fastSize)] = (float) f;
        ++count;

        if (++decimationPhase >= decimation)
        {
            decimationPhase = 0;
            const int s = (int) (slowCount & (long long) (slowSize - 1));
            slow[(size_t) s] = slow[(size_t) (s + slowSize)] = (float) low;
            ++slowCount;
            if (++hopPhase >= hop)
            {
                hopPhase = 0;
                analyse();
            }
        }

        // A hiss-like sound shows at once, long before the difference function has moved. Consonants must not be shifted.
        if (voiced && envFull > 3.0 * envLow + 1.0e-12 && envFull > 1.0e-9)
            setUnvoiced();

        if (voiced)
            stepMarks();
    }

    long long now() const noexcept { return count - 1; }        // index of the newest input sample
    bool isVoiced() const noexcept { return voiced; }
    bool isFricative() const noexcept { return envFull > 3.0 * envLow + 1.0e-12 && envFull > 1.0e-9; }
    double coarsePeriod() const noexcept { return tauCurrent * decimation; }
    float difference() const noexcept { return dipCurrent; }    // 0 = perfectly periodic

    // Best period right now in input samples, 0 when not voiced.
    double period() const noexcept { return ! voiced ? 0.0 : (markValid ? finePeriod : coarsePeriod()); }

    // Marks count up by sequence number. Valid sequence numbers are firstMark() to lastMark().
    long long lastMark() const noexcept { return markSeq - 1; }
    long long firstMark() const noexcept { return std::max (0LL, markSeq - maxMarks); }
    const Mark& mark (long long seq) const noexcept { return marks[(size_t) (seq & (maxMarks - 1))]; }
    bool hasMarks() const noexcept { return markSeq > 0; }

private:
    void clearTracking() noexcept
    {
        tauCurrent = 0.0; dipCurrent = 1.0f; voiced = false; voicedVotes = 0; unvoicedVotes = 0; pendingVotes = 0; pendingTau = 0.0;
        markValid = false; finePeriod = 0.0; weakMarks = 0; markSeq = 0; lastMarkPosition = -1.0e12; mismatchVotes = 0;
    }

    void setUnvoiced() noexcept
    {
        voiced = false;
        voicedVotes = 0;
        markValid = false;
    }

    // The coarse stage: one run of the normalised difference function over the newest window.
    void analyse() noexcept
    {
        if (slowCount < window + tauMax + 2)
            return;
        const int end = (int) ((slowCount - 1) & (long long) (slowSize - 1)) + slowSize + 1; // one past the newest, in the mirrored part
        const float* base = slow.data() + end - window;

        const float e0 = dotProduct (base, base, window);
        if (e0 / (float) window < gateEnergy)
        {
            noPitch();
            return;
        }

        const int first = tauMin - 1, last = tauMax + 1;
        float e = dotProduct (base - first, base - first, window);
        for (int tau = first; tau <= last; ++tau)
        {
            const float c = dotProduct (base, base - tau, window);
            const float den = e0 + e;
            curve[(size_t) tau] = den > 1.0e-12f ? 1.0f - 2.0f * c / den : 1.0f;
            const float leaving = base[window - 1 - tau], arriving = base[-tau - 1];
            e = std::max (0.0f, e + arriving * arriving - leaving * leaving);
        }

        // All the dips, each with a refined position.
        int numDips = 0;
        float lowest = 2.0f;
        for (int tau = tauMin; tau <= tauMax && numDips < (int) dipTau.size(); ++tau)
        {
            const float a = curve[(size_t) tau - 1], b = curve[(size_t) tau], c = curve[(size_t) tau + 1];
            if (b <= a && b < c)
            {
                const float den = a - 2.0f * b + c;
                const float shift = den > 1.0e-9f ? std::clamp (0.5f * (a - c) / den, -0.5f, 0.5f) : 0.0f;
                dipTau[(size_t) numDips] = (double) tau + shift;
                dipValue[(size_t) numDips] = b - 0.25f * (a - c) * shift;
                lowest = std::min (lowest, dipValue[(size_t) numDips]);
                ++numDips;
            }
        }
        if (numDips == 0 || lowest > 0.9f)
        {
            noPitch();
            return;
        }

        // First deep dip: the shortest period nearly as good as the best. A true period's multiples are just as deep, so
        // this is the main guard against octave-down errors. The margin keeps a shallow dip at half the period, from a strong
        // second harmonic, from winning.
        const float limit = lowest * 1.5f + 0.03f;
        int chosen = 0;
        for (int i = 0; i < numDips; ++i)
            if (dipValue[(size_t) i] <= limit) { chosen = i; break; }

        // A strong second harmonic, say on a low vowel whose first formant sits right on it, can make half the period look deep.
        // If a multiple of the chosen period is much deeper still, that multiple is the true period.
        if (dipValue[(size_t) chosen] < 0.08f)
            for (int i = chosen + 1; i < numDips; ++i)
            {
                const double multiple = dipTau[(size_t) i] / dipTau[(size_t) chosen];
                const double nearestWhole = std::round (multiple);
                if (nearestWhole >= 2.0 && nearestWhole <= 3.0 && std::abs (multiple / nearestWhole - 1.0) < 0.04
                    && clearlyDeeper (dipTau[(size_t) i], dipTau[(size_t) chosen]))
                {
                    chosen = i;
                    break;
                }
            }

        double tau = dipTau[(size_t) chosen];
        float dip = dipValue[(size_t) chosen];
        trackContinuity (tau, dip, numDips);

        // Voiced or not, with votes so one lucky frame of noise does nothing.
        const bool looksVoiced = dipCurrent < (voiced ? voicedOff : voicedOn) && ! isFricative();
        if (looksVoiced)
        {
            unvoicedVotes = 0;
            if (! voiced)
            {
                const bool steady = voicedVotes == 0 || std::abs (tauCurrent / votedTau - 1.0) < 0.05;
                voicedVotes = steady ? voicedVotes + 1 : 1;
                votedTau = tauCurrent;
                if (voicedVotes >= 3)
                {
                    voiced = true;
                    markValid = false;
                }
            }
        }
        else
        {
            voicedVotes = 0;
            if (voiced && ++unvoicedVotes >= 3)
                setUnvoiced();
        }

        // A real change of note that the marks did not follow: start them again from the coarse period.
        if (voiced && markValid && std::abs (coarsePeriod() / finePeriod - 1.0) > 0.12)
        {
            if (++mismatchVotes >= 2) { markValid = false; mismatchVotes = 0; }
        }
        else
        {
            mismatchVotes = 0;
        }
    }

    void trackContinuity (double tau, float dip, int numDips) noexcept
    {
        if (tauCurrent <= 0.0)
        {
            tauCurrent = tau;
            dipCurrent = dip;
            return;
        }
        const double ratio = tau / tauCurrent;
        if (std::abs (ratio - 1.0) < 0.08)
        {
            tauCurrent = tau;
            dipCurrent = dip;
            pendingVotes = 0;
            return;
        }

        // How good is the period we were already on?
        double nearTau = 0.0;
        float nearDip = 2.0f;
        for (int i = 0; i < numDips; ++i)
            if (std::abs (dipTau[(size_t) i] / tauCurrent - 1.0) < 0.08 && dipValue[(size_t) i] < nearDip)
            {
                nearDip = dipValue[(size_t) i];
                nearTau = dipTau[(size_t) i];
            }

        bool stay = false;
        if (nearTau > 0.0)
        {
            const bool related = isOctaveRelated (ratio);
            if (ratio < 1.0)
                stay = related ? dip > nearDip + 0.015f : nearDip <= dip + 0.01f; // the new period is shorter
            else
            {
                // The new period is longer. A rough sound can look that way for a moment, so stay put, unless a clean sound
                // plainly fits the longer period far better: then the note really has come down.
                const bool plainlyBetter = nearDip < 0.15f && dip < 0.35f * nearDip && clearlyDeeper (tau, nearTau);
                stay = related ? (nearDip <= dip + 0.15f && ! plainlyBetter) : nearDip <= dip + 0.02f;
            }
        }

        if (stay)
        {
            tauCurrent = nearTau;
            dipCurrent = nearDip;
            pendingVotes = 0;
            return;
        }

        // Accept the move once it has held for two frames.
        if (pendingVotes > 0 && std::abs (tau / pendingTau - 1.0) < 0.08)
            ++pendingVotes;
        else
            pendingVotes = 1;
        pendingTau = tau;
        if (pendingVotes >= 2)
        {
            tauCurrent = tau;
            dipCurrent = dip;
            pendingVotes = 0;
        }
        else if (nearTau > 0.0)
        {
            tauCurrent = nearTau;
            dipCurrent = nearDip;
        }
        else
        {
            dipCurrent = dip;
        }
    }

    // Is one period much deeper than another? Asked at full rate, because at 12 kHz a period falling between two samples
    // reads worse than a multiple of it that falls on a sample, even when both fit equally well.
    bool clearlyDeeper (double longTau, double shortTau) const noexcept
    {
        const float a = fineDip (longTau * decimation), b = fineDip (shortTau * decimation);
        return a < 0.35f * b - 0.002f;
    }

    float fineDip (double lagExact) const noexcept
    {
        const int span = window * decimation;
        const int lag = (int) std::lround (lagExact);
        const int stride = std::max (1, span / 400);
        const int points = span / stride;
        const long long start = now() - span + 1;
        if (start - lag - 1 < 0 || lag < 3)
            return 1.0f;
        const float* a = fastAt (start);
        const float ea = dotProduct (a, a, points, stride);
        float v[3];
        for (int l = 0; l < 3; ++l)
        {
            const float* b = fastAt (start - (lag - 1 + l));
            const float eb = dotProduct (b, b, points, stride);
            const float c = dotProduct (a, b, points, stride);
            const float den = ea + eb;
            v[l] = den > 1.0e-12f ? 1.0f - 2.0f * c / den : 1.0f;
        }
        // The bottom of the curve between the samples
        const float curve2 = v[0] - 2.0f * v[1] + v[2];
        if (curve2 <= 1.0e-9f)
            return std::min ({ v[0], v[1], v[2] });
        return std::max (0.0f, v[1] - (v[0] - v[2]) * (v[0] - v[2]) / (8.0f * curve2));
    }

    static bool isOctaveRelated (double ratio) noexcept
    {
        for (double r : { 2.0, 3.0, 4.0, 0.5, 1.0 / 3.0, 0.25, 1.5, 2.0 / 3.0 })
            if (std::abs (ratio / r - 1.0) < 0.05)
                return true;
        return false;
    }

    void noPitch() noexcept
    {
        dipCurrent = 1.0f;
        voicedVotes = 0;
        if (voiced && ++unvoicedVotes >= 3)
            setUnvoiced();
    }

    // Reads from the mirrored buffer: a pointer to the sample at an absolute index, with room to read forwards contiguously.
    const float* fastAt (long long index) const noexcept
    {
        return fast.data() + (int) (index & (long long) (fastSize - 1));
    }

    void addMark (double position, double period, float strength) noexcept
    {
        Mark& m = marks[(size_t) (markSeq & (maxMarks - 1))];
        m.position = position; m.period = period; m.strength = strength;
        ++markSeq;
        lastMarkPosition = position;
    }

    void seedMarks() noexcept
    {
        const double period = coarsePeriod();
        const int span = std::min ((int) std::lround (period), fastSize / 4);
        const long long start = now() - span + 1;
        if (start < 0 || period <= 0.0)
            return;
        int best = 0;
        float bestValue = -1.0f;
        for (int i = 0; i < span; ++i)
        {
            const float v = std::abs (fastAt (start + i)[0]);
            if (v > bestValue) { bestValue = v; best = i; }
        }
        const double position = (double) (start + best);

        // The window that proved this sound periodic reaches back, so grains can use marks from all of it.
        const double back = (double) (window + tauCurrent) * decimation;
        int steps = (int) (back / period);
        steps = std::clamp (steps, 0, 16);
        for (int k = steps; k >= 1; --k)
        {
            const double p = position - k * period;
            if (p > lastMarkPosition + 0.5 * period && p > 0.0)
                addMark (p, period, 0.5f);
        }
        if (position > lastMarkPosition + 0.25 * period)
            addMark (position, period, 0.5f);
        finePeriod = period;
        markValid = true;
        weakMarks = 0;
    }

    void stepMarks() noexcept
    {
        if (! markValid)
        {
            seedMarks();
            return;
        }
        const double period = finePeriod;
        const double predicted = lastMarkPosition + period;
        const int reach = std::max (2, (int) std::ceil (0.08 * period));
        if ((double) now() < predicted + markKnownAfterPeriods * period + 1.0)
            return;

        // Match the period before the last mark against the same stretch one period later.
        const int length = std::max (8, (int) std::lround (period));
        const int stride = std::max (1, length / 160);
        const int points = length / stride;
        const long long a0 = (long long) std::llround (lastMarkPosition - 0.75 * period);
        if (a0 < 0 || a0 + length + (long long) std::ceil (period) + reach > now() + 1)
        {
            if (a0 < 0) { markValid = false; }
            return;
        }
        const float* a = fastAt (a0);
        const float ea = dotProduct (a, a, points, stride);
        const int centre = (int) std::lround (period);
        const int lowLag = std::max (2, centre - reach), highLag = centre + reach;

        float bestMatch = -2.0f, before = -2.0f, after = -2.0f, previous = -2.0f;
        int bestLag = centre;
        bool wantAfter = false;
        for (int lag = lowLag; lag <= highLag; ++lag)
        {
            const float* b = fastAt (a0 + lag);
            const float eb = dotProduct (b, b, points, stride);
            const float c = dotProduct (a, b, points, stride);
            const float den = std::sqrt (ea * eb);
            const float match = den > 1.0e-12f ? c / den : 0.0f;
            if (wantAfter) { after = match; wantAfter = false; }
            if (match > bestMatch)
            {
                bestMatch = match;
                bestLag = lag;
                before = previous;
                after = -2.0f;
                wantAfter = true;
            }
            previous = match;
        }

        double lagExact = bestLag;
        if (before > -1.5f && after > -1.5f)
        {
            const float den = before - 2.0f * bestMatch + after;
            if (den < -1.0e-9f)
                lagExact += std::clamp (0.5f * (before - after) / den, -0.5f, 0.5f);
        }

        const bool atEdge = bestLag == lowLag || bestLag == highLag;
        if (bestMatch < 0.35f || atEdge)
        {
            // Lost it for now. Keep going on the prediction, and start again if this goes on.
            addMark (predicted, period, std::max (0.0f, bestMatch));
            if (++weakMarks >= 3)
                markValid = false;
            return;
        }
        weakMarks = 0;

        finePeriod = std::clamp (lagExact, periodMin, periodMax);
        double position = lastMarkPosition + lagExact;

        // Keep marks near the loudest part of each period. Marks that slide against the waveform bend a shifted voice's pitch by as much, so
        // this is slow: Smooth Harmony measured 8.6 cents at the 1% Smooth Tune used, and 1.7 cents at most at this rate.
        if (hasMarks())
        {
            const Mark& older = mark (lastMark());
            const int half = (int) (period * 0.5);
            const long long from = (long long) older.position - half;
            if (from > 0)
            {
                int best = 0;
                float bestValue = -1.0f;
                for (int i = 0; i < 2 * half; ++i)
                {
                    const float v = std::abs (fastAt (from + i)[0]);
                    if (v > bestValue) { bestValue = v; best = i; }
                }
                const double off = (double) (from + best) - older.position;
                if (std::abs (off) > 0.1 * period)
                    position += 0.002 * off;
            }
        }
        addMark (position, lagExact, bestMatch);
    }

    double sr = 48000.0, slowRate = 12000.0, lowHz = 70.0, highHz = 1300.0, envAttack = 0.01;
    int decimation = 4, hop = 16, slowSize = 8192, fastSize = 16384;
    int tauMin = 8, tauMax = 180, window = 180;
    double periodMin = 30.0, periodMax = 800.0;
    std::vector<float> slow, fast, curve;
    Biquad rumble, presence, coarseA, coarseB;
    double envFull = 0.0, envLow = 0.0;
    long long count = 0, slowCount = 0;
    int decimationPhase = 0, hopPhase = 0;

    static constexpr float gateEnergy = 1.0e-6f; // -60 dBFS: quieter sounds pass through untouched
    float voicedOn = 0.30f, voicedOff = 0.38f;
    std::array<double, 64> dipTau {};
    std::array<float, 64> dipValue {};
    double tauCurrent = 0.0, pendingTau = 0.0, votedTau = 1.0;
    float dipCurrent = 1.0f;
    int pendingVotes = 0, voicedVotes = 0, unvoicedVotes = 0, mismatchVotes = 0;
    bool voiced = false;

    std::array<Mark, maxMarks> marks {};
    long long markSeq = 0;
    bool markValid = false;
    double finePeriod = 0.0, lastMarkPosition = -1.0e12;
    int weakMarks = 0;
};
} // namespace smoothtune
