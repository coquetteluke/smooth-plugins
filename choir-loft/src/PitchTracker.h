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

namespace smoothharmony
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
        forgetAfterQuiet = std::max (1, (int) std::lround (0.1 * slowRate / hop));
        slowSize = 8192;
        slow.assign ((size_t) slowSize * 2, 0.0f);
        fastSize = 1;
        while (fastSize < (int) (sr * 0.25)) fastSize *= 2;
        fast.assign ((size_t) fastSize * 2, 0.0f);
        curve.assign (1024, 1.0f);
        lowCurve.assign (1024, 1.0f);
        lpcIn.assign (2 * 1024 + 64, 0.0f);
        lpcOut.assign (2 * 1024 + 64, 0.0f);
        rumble.highPass (50.0, sr, 0.707);
        presence.lowPass (std::min (5500.0, sr * 0.45), sr, 0.707);
        coarseA.lowPass (3000.0, sr, 0.541);
        coarseB.lowPass (3000.0, sr, 1.307);
        envAttack = 1.0 - std::exp (-1.0 / (0.0015 * sr));
        setRange (lowHz, highHz);
        reset();
    }

    // Clears tracking state, so a change of range starts clean. No allocation.
    // belowHz (1.5.0), when under lowestHz: notes from there up to lowestHz are followed too, without the range itself getting any slower.
    // The window that sets how fast a start is called stays the one for lowestHz. Only when that window finds nothing periodic, or a note
    // under the range is already being followed, are the longer periods looked at as well, over a window as long as the longest of them,
    // which reaches further back in time and adds no delay. See lowDip().
    void setRange (double lowestHz, double highestHz, double belowHz = 0.0) noexcept
    {
        lowHz = lowestHz;
        highHz = highestHz;
        tauMin = std::max (2, (int) std::floor (slowRate / highHz));
        tauMax = std::min ((int) curve.size() - 3, (int) std::ceil (slowRate / lowHz) + 1);
        window = tauMax;
        periodMin = sr / highHz * 0.9;
        periodMax = sr / lowHz * 1.1;
        lowTauMax = 0;
        if (belowHz > 0.0 && belowHz < lowestHz)
        {
            lowTauMax = std::min ((int) curve.size() - 3, (int) std::ceil (slowRate / belowHz) + 1);
            periodMax = sr / belowHz * 1.1;
        }
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
        {
            setUnvoiced();
            whyNow = whyHiss;
        }

        if (voiced)
            stepMarks();
    }

    long long now() const noexcept { return count - 1; }        // index of the newest input sample
    bool isVoiced() const noexcept { return voiced; }
    bool isFricative() const noexcept { return envFull > 3.0 * envLow + 1.0e-12 && envFull > 1.0e-9; }
    double coarsePeriod() const noexcept { return tauCurrent * decimation; }
    float difference() const noexcept { return dipCurrent; }    // 0 = perfectly periodic

    // How far back the newest start reaches: to where nearly periodic sound began, or to when the plain periodicity tests alone would have
    // called it, whichever is earlier. In input samples, counted back from the moment it was called.
    long long startedLateBy() const noexcept { return lateSamples; }

    // Best period right now in input samples, 0 when not voiced.
    double period() const noexcept { return ! voiced ? 0.0 : (markValid ? (steadied > 0.0 ? steadied : finePeriod) : coarsePeriod()); }

    // Why the newest analysis called the sound voiced or not. For measuring tools only: nothing in the audio path reads it.
    enum Why { whyVoiced = 0, whyQuiet, whyNoDip, whyRough, whyHiss, whyEven, whyResidual, whyVotes };
    int why() const noexcept { return whyNow; }
    // A breathy start is being steadied: see steadied in analyse().
    bool breathy() const noexcept { return voiced && steadied > 0.0; }
    // For measuring tools only: how well the newest window repeats at a period of 1 / hz, at full rate, and how well its even harmonics do.
    int startRule() const noexcept { return startRuleNow; }   // what startPeriod did in the newest analysis: 0 nothing, 1 a shorter period, 2 a longer one
    void probe (double hz, float& fine, float& even, int windows = 1) const noexcept { fine = fineDip (sr / hz, windows); even = evenDip (sr / hz); }

    // Marks count up by sequence number. Valid sequence numbers are firstMark() to lastMark().
    long long lastMark() const noexcept { return markSeq - 1; }
    long long firstMark() const noexcept { return std::max (0LL, markSeq - maxMarks); }
    const Mark& mark (long long seq) const noexcept { return marks[(size_t) (seq & (maxMarks - 1))]; }
    bool hasMarks() const noexcept { return markSeq > 0; }

private:
    void clearTracking() noexcept
    {
        tauCurrent = 0.0; dipCurrent = 1.0f; voiced = false; voicedAnalyses = 0; voicedVotes = 0; unvoicedVotes = 0; pendingVotes = 0; pendingTau = 0.0;
        markValid = false; finePeriod = 0.0; weakMarks = 0; markSeq = 0; lastMarkPosition = -1.0e12; mismatchVotes = 0;
        plainVotes = 0; plainTau = 1.0; plainVotedAt = -1; plainDipSum = 0.0f; lateSamples = 0; nearSince = -1;
        lowEndedAt = -1;
        quietAnalyses = 0;
    }

    void setUnvoiced() noexcept
    {
        if (voiced && lowTauMax > tauMax && tauCurrent > (double) tauMax)
            lowEndedAt = slowCount;
        voiced = false;
        plainVotes = 0;
        plainVotedAt = -1;
        voicedAnalyses = 0;
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
            whyNow = whyQuiet;
            // A pause (1.6.0, choir-dsp-r3-1): after 100 ms of quiet with no note, the period of the last phrase is forgotten, so the next
            // phrase starts as it would on a tracker that never heard the last one. Kept, it weighed the new start against a note from
            // before the pause, and a phrase an octave away could start an octave off or late.
            if (! voiced && ++quietAnalyses >= forgetAfterQuiet && tauCurrent > 0.0)
            {
                tauCurrent = 0.0;
                pendingVotes = 0;
                pendingTau = 0.0;
            }
            return;
        }
        quietAnalyses = 0;

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
        const bool rangeHasDip = numDips > 0 && lowest <= 0.9f;
        if (! rangeHasDip && lowTauMax <= tauMax)
        {
            noPitch();
            whyNow = whyNoDip;
            return;
        }

        // First deep dip: the shortest period nearly as good as the best. A true period's multiples are just as deep, so
        // this is the main guard against octave-down errors. The margin keeps a shallow dip at half the period, from a strong
        // second harmonic, from winning.
        const float limit = lowest * 1.5f + 0.03f;
        int chosen = 0;
        if (rangeHasDip)
            for (int i = 0; i < numDips; ++i)
                if (dipValue[(size_t) i] <= limit) { chosen = i; break; }

        // Under the range (1.5.0). Looked at only where the range itself has nothing to offer (no dip, or none good enough to be called
        // voiced), or while a note under the range is being followed, so a note inside the range is read exactly as before.
        bool usingLow = false, leftLow = false;
        if (lowTauMax > tauMax)
        {
            const bool onLow = voiced && tauCurrent > (double) tauMax;
            const bool rangeGood = rangeHasDip && dipValue[(size_t) chosen] < (voiced ? voicedOff : voicedOn);
            double lowTau = 0.0;
            float lowValue = 2.0f;
            if ((onLow || ! rangeGood) && numDips < (int) dipTau.size() && lowDip (lowTau, lowValue))
            {
                bool take = onLow;
                // A note under the range that is being followed gives way to a note at half its period, 130 Hz or more, as soon as that
                // one is a clean note of its own: it repeats every period, and so do its even harmonics (1.5.1). The end of
                // a word often drops an octave into a creak, and behind a doubler it does so cleanly; the next word, starting from there,
                // was sung an octave low for its first 30 ms.
                if (onLow)
                    for (int i = 0; i < numDips; ++i)
                        if (std::abs (lowTau / (dipTau[(size_t) i] * 2.0) - 1.0) < 0.06 && dipValue[(size_t) i] < voicedOff
                            && fineDip (dipTau[(size_t) i] * decimation) < lowHalfClean && evenDip (dipTau[(size_t) i] * decimation) < startEvenLimit)
                        {
                            take = false;
                            chosen = i;
                            leftLow = true;
                            break;
                        }
                if (! take && ! voiced)
                {
                    // A new note. Twice a period always fits wherever the period does, and on a rough start fits better, the longer window
                    // averaging more of the noise away. So the long period must be plainly the better of the two, asked at full rate over
                    // the same stretch, and the same against a third of it if the range has a dip there.
                    take = clearlyDeeper (lowTau, 0.5 * lowTau);
                    for (int i = 0; i < numDips && take; ++i)
                        if (std::abs (lowTau / (dipTau[(size_t) i] * 3.0) - 1.0) < 0.04 && ! clearlyDeeper (lowTau, dipTau[(size_t) i]))
                            take = false;
                    // And when the range has a dip at half the long period (1.5.1), the sound may well be that note, 130 Hz or more, made rough
                    // by something: a doubler in front, which Luke runs, lays a second voice a few ms and cents away over the first, and two
                    // periods of that fit better than one. A true low note leaves half its period with flipped even harmonics; a note at
                    // the half period does not. Behind Duo, 1.5.0 read 13 voice notes of 896 above 130 Hz an octave low where 1.4.4 read 7.
                    // Only just after a followed low note ended (1.5.2): that is the case above, a word's last creak running into the
                    // next word. A fresh start out of a creak, after a pause, is a low note and is followed: 1.5.1 refused those as
                    // well, the votes then flipped between the creak and the note for 40 ms with no voice at all (take 10 at 23.69 s,
                    // take 05 at 115.25 s), and time on the note under 98 Hz fell by a point.
                    const bool lowJustEnded = lowEndedAt >= 0 && slowCount - lowEndedAt < (long long) (lowEndedWithin * slowRate);
                    for (int i = 0; i < numDips && take && lowJustEnded; ++i)
                        if (std::abs (lowTau / (dipTau[(size_t) i] * 2.0) - 1.0) < 0.06 && dipValue[(size_t) i] < 0.6f && ! evenFlipsAt (dipTau[(size_t) i]))
                            take = false;
                }
                // A note that slides down out of the range keeps going. One that is in the range and turns rough for a moment is not moved
                // an octave down: it goes unvoiced as before.
                if (! take && voiced && lowTau < 1.3 * tauCurrent)
                    take = true;
                if (take)
                {
                    dipTau[(size_t) numDips] = lowTau;
                    dipValue[(size_t) numDips] = lowValue;
                    // While on a low note the range's own dips are mostly its harmonics. When one of those is good enough to be called
                    // voiced, the low dip only joins the list: the usual rules for a change of period then weigh the two, and a move to a
                    // shorter related period needs it to be about as deep. So a note that began an octave low comes back up.
                    if (! rangeGood)
                    {
                        chosen = numDips;
                        usingLow = true;
                    }
                    ++numDips;
                }
            }
            if (! rangeHasDip && ! usingLow)
            {
                noPitch();
                whyNow = whyNoDip;
                return;
            }
        }

        // A strong second harmonic, say on a low vowel whose first formant sits right on it, can make half the period look deep.
        // If a multiple of the chosen period is much deeper still, that multiple is the true period.
        if (usingLow || leftLow)
        {
        }
        else if (dipValue[(size_t) chosen] < 0.08f)
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
        // On breathy sound a high note's period and half of it can look about as good, the noise filling both dips alike. See noisyOctave.
        else
            for (int i = chosen + 1; i < numDips; ++i)
                if (std::abs (dipTau[(size_t) i] / dipTau[(size_t) chosen] / 2.0 - 1.0) < 0.06)
                {
                    if (noisyOctave (dipTau[(size_t) chosen], dipTau[(size_t) i]))
                        chosen = i;
                    break;
                }

        double tau = dipTau[(size_t) chosen];
        float dip = dipValue[(size_t) chosen];
        // A start (1.5.1): see startPeriod. Until a note is established its period is chosen there, when that rule has something to say.
        startRuleNow = 0;
        bool creditHeld = false;
        if (! usingLow && rangeHasDip && (! voiced || voicedAnalyses * hop < 3 * window))
        {
            const double before = tau;
            const float dipBefore = dip;
            if (startPeriod (chosen, numDips, limit, tau, dip))
                startRuleNow = tau < before ? 1 : 2;
            // A longer period than the first deep dip (1.5.2): while nothing is voiced it must come out the same twice in a row, and
            // until then the first deep dip is weighed as it was before 1.5.1. Taken at once, with the longer period's deeper reading
            // counting for the votes, it called 5 ms of a murmur at -46 dBFS a note (take 02 at 189.41 s: 516 Hz reading 0.37 to 0.48,
            // twice its period 0.18 to 0.22), and the voices sang 30 ms of it: the leak inside phrases that 1.5.1 added.
            if (startRuleNow == 2 && ! voiced)
            {
                longerVotes = longerVotes > 0 && std::abs (tau / longerTau - 1.0) < 0.08 ? longerVotes + 1 : 1;
                longerTau = tau;
                if (longerVotes < 2)
                {
                    // Would this analysis have been a vote for a start at the longer period? Then it counts as one once the next agrees,
                    // so a start that keeps to the longer period is called no later than before.
                    longerHeldOk = dip < voicedOn && ! isFricative() && evenDip (tau * decimation) <= evenLimit
                                   && (dip < plainlyPeriodicDip || residualDip (tau) <= residualLimit);
                    tau = before; dip = dipBefore; startRuleNow = 0;
                }
                creditHeld = longerVotes == 2 && longerHeldOk;
            }
            else
                longerVotes = 0;
        }
        if (voiced) ++voicedAnalyses;
        // A note just called does not drop to two, three or four of its periods while its own period still holds: a multiple of a period
        // always fits, and on breathy sound fits better (see startPeriod). A real drop leaves the old period with flipped even harmonics.
        bool heldPeriod = false;
        if (startRuleNow == 0 && voiced && ! usingLow && voicedAnalyses * hop < 3 * window && tauCurrent > 0.0)
        {
            const double ratio = tau / tauCurrent, whole = std::round (ratio);
            heldPeriod = whole >= 2.0 && whole <= 4.0 && std::abs (ratio / whole - 1.0) < 0.06
                         && evenDip (tauCurrent * decimation) <= startEvenLimit && fineDip (tauCurrent * decimation) <= startFineLimit;
        }
        if (heldPeriod)
        {
            dipCurrent = dip;
            pendingVotes = 0;
        }
        else if (leftLow)
        {
            // Up from under the range to the note at half the period, once it has held for two analyses
            pendingVotes = pendingVotes > 0 && std::abs (tau / pendingTau - 1.0) < 0.08 ? pendingVotes + 1 : 1;
            pendingTau = tau;
            if (pendingVotes >= 2)
            {
                tauCurrent = tau;
                dipCurrent = dip;
                pendingVotes = 0;
            }
        }
        else if (startRuleNow == 0)
            trackContinuity (tau, dip, numDips);
        else if (! voiced || std::abs (tau / tauCurrent - 1.0) < 0.08)
        {
            // The start rule has weighed the related periods itself. The votes below still ask a start to hold for three analyses.
            tauCurrent = tau;
            dipCurrent = dip;
            pendingVotes = 0;
        }
        else
        {
            // In a note already called, a move must hold for two analyses, as any move does
            pendingVotes = pendingVotes > 0 && std::abs (tau / pendingTau - 1.0) < 0.08 ? pendingVotes + 1 : 1;
            pendingTau = tau;
            if (pendingVotes >= 2)
            {
                tauCurrent = tau;
                dipCurrent = dip;
                pendingVotes = 0;
            }
        }

        // Under the range the tracker is choosier: fry and the creak at the end of a word repeat at these periods too, roughly, and before
        // 1.5.0 they were left alone. A sung low note repeats far better than they do (on Luke's takes a median difference of 0.01 to 0.14
        // against 0.24), so there a start needs half the usual limit and a held note the usual limit for a start.
        const bool underRange = lowTauMax > tauMax && tauCurrent > (double) tauMax;
        const float limitNow = underRange ? (voiced ? voicedOn : 0.5f * voicedOn) : (voiced ? voicedOff : voicedOn);

        // Where the current run of nearly periodic sound began, for a start to be written back to. Under the range only sound good enough
        // to start a note counts, so a creak before a word does not pull the word's start back over it.
        if (! voiced)
        {
            if (dipCurrent < (underRange ? limitNow : nearlyPeriodic) && ! isFricative()) { if (nearSince < 0) nearSince = slowCount; }
            else nearSince = -1;
        }

        // Voiced or not, with votes so one lucky frame of noise does nothing.
        const bool looksVoiced = dipCurrent < limitNow && ! isFricative();
        whyNow = looksVoiced ? (voiced ? whyVoiced : whyVotes) : (isFricative() ? whyHiss : whyRough);
        if (looksVoiced)
        {
            unvoicedVotes = 0;
            // A start must repeat in its even harmonics too. Breath through a closed vowel rings one narrow formant and passes for a note: the
            // sound plus itself half a period later cancels that ring, as it cancels a voice's fundamental and odd harmonics, but a voice's
            // even harmonics are left and repeat a period later. A reading an octave high fails the same way, its odd multiples flipping sign.
            if (! voiced && evenDip (tauCurrent * decimation) > evenLimit)
            {
                voicedVotes = 0;
                plainVotes = 0;
                plainVotedAt = -1;
                whyNow = whyEven;
            }
            else if (! voiced)
            {
                // The plain tests: three steady votes. 1.4.1 called a start here.
                const bool plainSteady = plainVotes == 0 || std::abs (tauCurrent / plainTau - 1.0) < 0.05;
                plainVotes = plainSteady ? plainVotes + 1 : 1;
                plainTau = tauCurrent;
                plainDipSum = plainVotes == 1 ? dipCurrent : plainDipSum + dipCurrent;
                if (creditHeld && plainVotes == 1) { plainVotes = 2; plainDipSum += dipCurrent; }
                if (plainVotes == 3) plainVotedAt = slowCount;

                // And once its formants are taken out it must still repeat (residualDip). Breath through a vowel passes every test above for a
                // few ms at a time; a voice leaves a train of pulses. Not asked of plainly periodic sound, nor of a run that has stayed clean for
                // 10 ms, since what prediction leaves of a gliding or breathy voice repeats poorly at first.
                const bool plainlyPeriodic = dipCurrent < plainlyPeriodicDip;
                const bool heldClean = plainVotes * hop >= (int) (0.01 * slowRate) && plainDipSum / (float) plainVotes < heldCleanDip;
                if (! plainlyPeriodic && ! heldClean && residualDip (tauCurrent) > residualLimit)
                {
                    voicedVotes = 0;
                    whyNow = whyResidual;
                }
                else
                {
                    const bool steady = voicedVotes == 0 || std::abs (tauCurrent / votedTau - 1.0) < 0.05;
                    voicedVotes = steady ? voicedVotes + 1 : 1;
                    votedTau = tauCurrent;
                    if (creditHeld && voicedVotes == 1) voicedVotes = 2;
                    if (voicedVotes >= 3)
                    {
                        voiced = true;
                        whyNow = whyVoiced;
                        markValid = false;
                        lateSamples = plainVotes >= 3 && plainVotedAt >= 0 ? (slowCount - plainVotedAt) * decimation : 0;
                        if (nearSince >= 0) lateSamples = std::max (lateSamples, (slowCount - nearSince) * decimation);
                    }
                }
            }
        }
        else
        {
            voicedVotes = 0;
            plainVotes = 0;
            plainVotedAt = -1;
            if (voiced && ++unvoicedVotes >= 3)
                setUnvoiced();
        }

        // A breathy start (1.5.1). Each period's mark is matched against a noisy period before it, and breath ringing in the formants pulls the
        // match about: on a high note the period read 4% either way from one period to the next and a little short on average, enough to
        // move the voices to the next note and back, and to bend them while they follow the lead out of the start. The coarse period,
        // read over a whole window, holds steadier. So when a note is still breathy 10 ms in (a difference of 0.15 or more; a clean one is under
        // 0.03 by then), the period given out for its first 80 ms is the coarse one, averaged over the last 10 ms.
        steadied = 0.0;
        if (voiced)
        {
            const long long age = (long long) voicedAnalyses * hop * decimation;
            if (recentCount > 0 && std::abs (tauCurrent / recentTau[(size_t) ((recentCount - 1) % steadyOver)] - 1.0) > 0.08)
                recentCount = 0;                                // a real move: start the average again
            recentTau[(size_t) (recentCount % steadyOver)] = tauCurrent;
            ++recentCount;
            // Only where the period is short, under 5 ms: a low note's period is longer than the breath rings, and reads steadily as it is.
            if (age >= (long long) (0.01 * sr) && age < (long long) (0.08 * sr) && dipCurrent >= breathyDip && tauCurrent * decimation < 0.005 * sr)
                breathyStart = true;
            if (age >= (long long) (0.08 * sr))
                breathyStart = false;
            if (breathyStart)
            {
                const int have = std::min (recentCount, steadyOver);
                double sum = 0.0;
                for (int i = 0; i < have; ++i) sum += recentTau[(size_t) i];
                steadied = longLagPeriod (sum / have * decimation);
            }
        }
        else
        {
            recentCount = 0;
            breathyStart = false;
        }

        // A real change of note that the marks did not follow: start them again from the coarse period.
        if (voiced && markValid && steadied <= 0.0 && std::abs (coarsePeriod() / finePeriod - 1.0) > 0.12)
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
                // A multiple of the current period always fits wherever the current one does, so on breathy or noisy sound one short window can
                // make two or three periods look deepest for a few ms, and the voices would jump an octave or a twelfth down and glide back.
                // So a move to a multiple must also hold over a stretch three windows long.
                const bool noisy = std::abs (ratio / 2.0 - 1.0) < 0.06 && noisyOctave (nearTau, tau);
                const bool plainlyBetter = (nearDip < 0.15f && dip < 0.35f * nearDip && clearlyDeeper (tau, nearTau)) || noisy;
                const bool established = voicedAnalyses * hop >= 3 * window;   // not in a note's first moments, where an octave-high start still needs correcting
                stay = related ? ((nearDip <= dip + 0.15f && ! plainlyBetter) || (established && ! clearlyDeeper (tau, nearTau, 3) && ! noisy)) : nearDip <= dip + 0.02f;
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

    // A period read from several periods at once: the lag near a whole number of them, 8 ms or more, at which the newest window best
    // repeats, divided by that number. An error of a sample in the lag is then a fraction of a sample in the period, and breath that
    // rings for a few ms has died away at that distance. Returns the period it was given when the long lag has no clear best.
    double longLagPeriod (double period) const noexcept
    {
        const int k = std::max (2, (int) std::ceil (0.008 * sr / period));
        const int span = std::min (window * decimation, fastSize / 4);
        const int centre = (int) std::lround (k * period), reach = std::max (2, (int) std::ceil (0.02 * k * period));
        const int stride = std::max (1, span / 400), points = span / stride;
        const long long start = now() - span + 1;
        if (start - centre - reach - 1 < 0 || centre + reach + span >= fastSize)
            return period;
        const float* a = fastAt (start);
        const float ea = dotProduct (a, a, points, stride);
        float best = 2.0f, before = 2.0f, after = 2.0f, previous = 2.0f;
        int bestLag = centre;
        bool wantAfter = false;
        for (int lag = centre - reach; lag <= centre + reach; ++lag)
        {
            const float* b = fastAt (start - lag);
            const float eb = dotProduct (b, b, points, stride), c = dotProduct (a, b, points, stride);
            const float den = ea + eb;
            const float v = den > 1.0e-12f ? 1.0f - 2.0f * c / den : 1.0f;
            if (wantAfter) { after = v; wantAfter = false; }
            if (v < best) { best = v; bestLag = lag; before = previous; after = 2.0f; wantAfter = true; }
            previous = v;
        }
        if (bestLag == centre - reach || bestLag == centre + reach || before > 1.5f || after > 1.5f)
            return period;
        const float curve2 = before - 2.0f * best + after;
        const double exact = bestLag + (curve2 > 1.0e-9f ? std::clamp (0.5f * (before - after) / curve2, -0.5f, 0.5f) : 0.0f);
        return exact / k;
    }

    // The period of a start (1.5.1). On a breathy start the breath is at about the voice's own level and has gone through the same formants,
    // so it rings at them for a few ms: at lags that short the difference function measures the ring as much as the voice. On a high note
    // the period is that short. Half the period then looks about as deep as the period (and is refused as a start, its even harmonics not
    // repeating, so the note waits), and two or three periods, where the ring has died away, look deepest and get picked: a note that
    // starts a twelfth low. Both were the breathy high starts that stayed late from 1.4.0 to 1.5.0.
    // So among the deep dips and their halves, thirds and quarters, the period is the shortest one that a voice could have: its even
    // harmonics repeat (which half a true period fails outright, its odd harmonics flipping sign), and it is a dip of its own at full
    // rate. How periodic the sound is, is then read at the deep dip it came from, the longer lag being the cleaner reading.
    // A longer period than the first deep dip is only taken when that dip's even harmonics plainly flip, so a voice that is nearly a pure
    // tone, whose even harmonics say nothing either way, keeps the period it had. Returns false, changing nothing, when the first deep dip
    // already is that period: every clean start.
    bool startPeriod (int chosen, int numDips, float limit, double& tau, float& dip) const noexcept
    {
        const double was = dipTau[(size_t) chosen];
        double best = 0.0;
        float bestValue = 2.0f;
        int tried = 0;
        for (int i = 0; i < numDips && tried < 6; ++i)
        {
            if (dipValue[(size_t) i] > limit)
                continue;
            ++tried;
            for (int k = 4; k >= 1; --k)
            {
                const double t = dipTau[(size_t) i] / k;
                if (t < (double) tauMin || (best > 0.0 && t > best * 0.96))
                    continue;
                if (std::abs (t / was - 1.0) < 0.04 && k == 1)
                    continue;                                   // the first deep dip itself: looked at last, below
                // Every deep dip from it up must be a whole number of it: a loud harmonic makes two thirds of a period look like one,
                // and the period itself, a deep dip too, is then one and a half of it. Every deep dip under it must be one that flips.
                bool fits = true;
                for (int j = 0; j < numDips && fits; ++j)
                {
                    if (dipValue[(size_t) j] > limit)
                        continue;
                    const double ratio = dipTau[(size_t) j] / t;
                    if (ratio > 0.96)
                        fits = std::abs (ratio / std::round (ratio) - 1.0) < 0.06;
                    else
                        fits = evenFlipsAt (dipTau[(size_t) j]);
                }
                // And it must be about as periodic as the deep dip it came from. Breath ringing in the formants can make a true period read
                // up to twice as rough as its multiples; a third or a half of a true period, carried by one loud harmonic, reads rougher still.
                const float from = dipValue[(size_t) i];
                if (! fits || fineDip (t * decimation) > std::min (startFineLimit, 2.5f * from + 0.05f)
                    || evenDip (t * decimation) > std::min (startEvenLimit, 1.6f * from + 0.05f))
                    continue;
                // Longer than the first deep dip: only when that one is plainly half, a third or so of a true period
                if (t > was * 1.04 && ! evenFlipsAt (was))
                    continue;
                best = t;
                bestValue = dipValue[(size_t) i];
            }
        }
        if (best <= 0.0)
            return false;
        // The first deep dip stands if it is the shorter of the two and a period a voice could have
        if (was < best * 0.96 && ! evenFlipsAt (was))
            return false;
        if (std::abs (best / was - 1.0) < 0.04)
            return false;
        tau = best;
        dip = std::min (bestValue, dip);
        return true;
    }

    bool evenFlipsAt (double tauSlow) const noexcept { return evenDip (tauSlow * decimation) > startFlipLimit; }

    static constexpr float lowHalfClean = 0.15f;   // a low note of Luke's reads 0.4 or more at half its period, the whole note through
    static constexpr float startFineLimit = 0.5f, startEvenLimit = 0.6f, startFlipLimit = 1.15f;

    // The first deep dip among the periods under the range, from the range's longest period to lowTauMax, over a window as long as the
    // longest of them: a window shorter than a period can fall between two pulses of a low, creaky note and see nothing. False if there
    // is none. It only reaches further back, so it costs no delay; a low note is called a few ms later than one in the range.
    bool lowDip (double& tauOut, float& valueOut) noexcept
    {
        const int span = lowTauMax;
        if (slowCount < span + lowTauMax + 3)
            return false;
        const int end = (int) ((slowCount - 1) & (long long) (slowSize - 1)) + slowSize + 1;
        const float* base = slow.data() + end - span;
        const float e0 = dotProduct (base, base, span);
        if (e0 / (float) span < gateEnergy)
            return false;
        const int first = tauMax, last = lowTauMax + 1;
        float e = dotProduct (base - first, base - first, span);
        for (int tau = first; tau <= last; ++tau)
        {
            const float c = dotProduct (base, base - tau, span);
            const float den = e0 + e;
            lowCurve[(size_t) tau] = den > 1.0e-12f ? 1.0f - 2.0f * c / den : 1.0f;
            const float leaving = base[span - 1 - tau], arriving = base[-tau - 1];
            e = std::max (0.0f, e + arriving * arriving - leaving * leaving);
        }
        float lowest = 2.0f;
        for (int tau = first + 1; tau <= lowTauMax; ++tau)
        {
            const float a = lowCurve[(size_t) tau - 1], b = lowCurve[(size_t) tau], c = lowCurve[(size_t) tau + 1];
            if (b <= a && b < c)
                lowest = std::min (lowest, b);
        }
        if (lowest > 0.9f)
            return false;
        const float limit = lowest * 1.5f + 0.03f;
        for (int tau = first + 1; tau <= lowTauMax; ++tau)
        {
            const float a = lowCurve[(size_t) tau - 1], b = lowCurve[(size_t) tau], c = lowCurve[(size_t) tau + 1];
            if (b <= a && b < c && b <= limit)
            {
                const float den = a - 2.0f * b + c;
                const float shift = den > 1.0e-9f ? std::clamp (0.5f * (a - c) / den, -0.5f, 0.5f) : 0.0f;
                tauOut = (double) tau + shift;
                valueOut = b - 0.25f * (a - c) * shift;
                return true;
            }
        }
        return false;
    }

    // Is one period much deeper than another? Asked at full rate, because at 12 kHz a period falling between two samples
    // reads worse than a multiple of it that falls on a sample, even when both fit equally well.
    bool clearlyDeeper (double longTau, double shortTau, int windows = 1) const noexcept
    {
        const float a = fineDip (longTau * decimation, windows), b = fineDip (shortTau * decimation, windows);
        return a < 0.35f * b - 0.002f;
    }

    // A breathy high note: aspiration at about the voice's own level fills the dips at its period and at half of it alike, and with a strong 2nd
    // harmonic the half period wins the first deep dip rule, an octave high for the first 60 ms. The odd harmonics still make the true period
    // a little deeper, and over three windows that little holds. Asked only of readings over 600 Hz, where the octave high error lands.
    bool noisyOctave (double shortTau, double longTau) const noexcept
    {
        if (slowRate / shortTau < 600.0)
            return false;
        const float shortDip = fineDip (shortTau * decimation, 3);
        float longDip = 2.0f;
        for (double f = 0.96; f <= 1.0401; f += 0.02)   // the coarse period of a noisy high note can be a few % off
            longDip = std::min (longDip, fineDip (longTau * f * decimation, 3));
        return longDip < shortDip - 0.015f;
    }

    float fineDip (double lagExact, int windows = 1) const noexcept
    {
        const int span = std::min (window * decimation * windows, fastSize / 2);
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

    // How well the even harmonics repeat a period later, as a normalised difference over two windows at full rate. The sound plus itself half
    // a period later cancels the fundamental and every odd harmonic, and with them a lone resonance at that frequency.
    float evenDip (double lagExact) const noexcept
    {
        const int span = std::min (window * decimation * 2, fastSize / 4);
        const int lag = (int) std::lround (lagExact), half = (int) std::lround (0.5 * lagExact);
        const int stride = std::max (1, span / 400);
        const long long start = now() - span + 1;
        if (start - lag - half - 2 < 0 || lag < 6)
            return 0.0f;
        float best = 2.0f;
        for (int l = lag - 1; l <= lag + 1; ++l)
        {
            const float* a = fastAt (start);
            const float* ah = fastAt (start - half);
            const float* b = fastAt (start - l);
            const float* bh = fastAt (start - l - half);
            float c = 0.0f, ea = 0.0f, eb = 0.0f;
            for (int i = 0; i < span; i += stride)
            {
                const float x = a[i] + ah[i], y = b[i] + bh[i];
                c += x * y; ea += x * x; eb += y * y;
            }
            const float den = ea + eb;
            best = std::min (best, den > 1.0e-12f ? 1.0f - 2.0f * c / den : 1.0f);
        }
        return best;
    }

    static constexpr float evenLimit = 1.0f;   // a start's even harmonics must repeat at least this well (1 is no match at all)
    static constexpr float residualLimit = 0.45f, plainlyPeriodicDip = 0.05f, heldCleanDip = 0.12f, nearlyPeriodic = 0.35f;

    // How well the newest window repeats a period later once its formants are taken out: linear prediction (order 10, Hann window, fitted over
    // at least 24 ms so a narrow first formant is resolved), then what prediction leaves, smoothed over 7 samples so a pulse that moves by a
    // sample still lines up, then the normalised difference at the period. Only asked at a start, so its cost stays out of a held note.
    float residualDip (double tauSlow) noexcept
    {
        const int tauI = (int) std::lround (tauSlow);
        const int n = window + tauI + 2;
        const int total = std::max (n + lpcOrder, (int) (0.024 * slowRate));
        if (slowCount < total + 2 || tauI < 2 || total > (int) lpcIn.size())
            return 0.0f;
        const int end = (int) ((slowCount - 1) & (long long) (slowSize - 1)) + slowSize + 1;
        const float* src = slow.data() + end - total;
        float mean = 0.0f;
        vDSP_meanv (src, 1, &mean, (vDSP_Length) total);
        const float negMean = -mean;
        vDSP_vsadd (src, 1, &negMean, lpcIn.data(), 1, (vDSP_Length) total);

        // Hann window by a rotating phasor, then autocorrelation and Levinson
        const double step = 2.0 * M_PI / total, cs = std::cos (step), sn = std::sin (step);
        double re = std::cos (0.5 * step), im = std::sin (0.5 * step);
        for (int i = 0; i < total; ++i)
        {
            lpcOut[(size_t) i] = lpcIn[(size_t) i] * (float) (0.5 - 0.5 * re);
            const double r2 = re * cs - im * sn;
            im = re * sn + im * cs;
            re = r2;
        }
        std::array<double, lpcOrder + 1> r {}, a {}, before {};
        for (int lag = 0; lag <= lpcOrder; ++lag)
            r[(size_t) lag] = dotProduct (lpcOut.data() + lag, lpcOut.data(), total - lag);
        if (r[0] <= 1.0e-12)
            return 1.0f;
        r[0] *= 1.0001;   // a little white noise, so a pure tone cannot make it unstable
        a[0] = 1.0;
        double err = r[0];
        for (int i = 1; i <= lpcOrder; ++i)
        {
            double acc = r[(size_t) i];
            for (int j = 1; j < i; ++j) acc += a[(size_t) j] * r[(size_t) (i - j)];
            const double k = -acc / err;
            before = a;
            for (int j = 1; j < i; ++j) a[(size_t) j] = before[(size_t) j] + k * before[(size_t) (i - j)];
            a[(size_t) i] = k;
            err *= 1.0 - k * k;
            if (err <= 0.0)
                return 1.0f;
        }
        std::array<float, lpcOrder + 1> reversed {};
        for (int j = 0; j <= lpcOrder; ++j) reversed[(size_t) j] = (float) a[(size_t) (lpcOrder - j)];
        // What prediction leaves, the newest n samples, on the unwindowed signal
        for (int i = total - n; i < total; ++i)
            lpcOut[(size_t) (i - (total - n))] = dotProduct (lpcIn.data() + i - lpcOrder, reversed.data(), lpcOrder + 1);
        // Triangle smoothing, weights 1 2 3 4 3 2 1
        for (int i = 0; i < n; ++i)
        {
            float acc = 0.0f, weights = 0.0f;
            for (int k = -3; k <= 3; ++k)
            {
                const int j = i + k;
                if (j < 0 || j >= n) continue;
                const float w = (float) (4 - std::abs (k));
                acc += w * lpcOut[(size_t) j];
                weights += w;
            }
            lpcIn[(size_t) i] = acc / weights;
        }
        const float* e0 = lpcIn.data() + n - window;
        const float ea = dotProduct (e0, e0, window);
        float best = 2.0f;
        for (int lag = tauI - 1; lag <= tauI + 1; ++lag)
        {
            const float c = dotProduct (e0, e0 - lag, window), eb = dotProduct (e0 - lag, e0 - lag, window);
            const float den = ea + eb;
            best = std::min (best, den > 1.0e-18f ? 1.0f - 2.0f * c / den : 1.0f);
        }
        return best;
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
        // The 1.5.2 rule asks for the longer period twice in a row: an analysis with no pitch in between breaks the row (1.6.0,
        // choir-dsp-r3-4). Kept through a pause, one vote from before it and one after it called a start one analysis early.
        longerVotes = 0;
        dipCurrent = 1.0f;
        voicedVotes = 0;
        plainVotes = 0;
        plainVotedAt = -1;
        nearSince = -1;
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
        const double back = (double) (window + tauCurrent) * decimation + (double) lateSamples;   // and a start written back reads that far
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
        // On a breathy start (see steadied) the marks keep to the steadier period too, each one moved by under 1% to stay on the waveform.
        // Left to themselves they lost the match every few periods there and were set down afresh at another point of the waveform, which
        // a voice built on them sings as a lurch in pitch.
        const bool steady = steadied > 0.0;
        const double period = steady ? steadied : finePeriod;
        const double predicted = lastMarkPosition + period;
        const int reach = steady ? std::max (1, (int) std::ceil (0.008 * period)) : std::max (2, (int) std::ceil (0.08 * period));
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
            if (! steady && ++weakMarks >= 3)
                markValid = false;
            return;
        }
        weakMarks = 0;

        finePeriod = std::clamp (lagExact, periodMin, periodMax);
        double position = lastMarkPosition + lagExact;

        // Keep marks near the loudest part of each period. Marks that slide against the waveform bend a shifted voice's pitch by as much, at this
        // rate at most 1.7 cents. The 1% in Smooth Tune measured up to 8.6 cents on a sustained note, far over the limit here.
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
    std::vector<float> slow, fast, curve, lowCurve;
    int lowTauMax = 0;   // longest period looked at under the range, in slow samples. 0: none
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
    int pendingVotes = 0, voicedVotes = 0, unvoicedVotes = 0, mismatchVotes = 0, voicedAnalyses = 0;
    bool voiced = false;
    int whyNow = whyQuiet, startRuleNow = 0;
    int longerVotes = 0;
    int quietAnalyses = 0, forgetAfterQuiet = 75;   // analyses in a row under the gate, and how many make a pause (100 ms)
    bool longerHeldOk = false;
    long long lowEndedAt = -1;                     // when the last followed note under the range ended, in slow samples
    static constexpr double lowEndedWithin = 0.1;  // seconds

    double longerTau = 1.0;
    int plainVotes = 0;
    double plainTau = 1.0;
    float plainDipSum = 0.0f;
    long long plainVotedAt = -1, lateSamples = 0, nearSince = -1;
    static constexpr int lpcOrder = 10;
    std::vector<float> lpcIn, lpcOut;

    std::array<Mark, maxMarks> marks {};
    long long markSeq = 0;
    bool markValid = false;
    double finePeriod = 0.0, lastMarkPosition = -1.0e12, steadied = 0.0;
    static constexpr float breathyDip = 0.15f;
    static constexpr int steadyOver = 8;
    std::array<double, steadyOver> recentTau {};
    int recentCount = 0;
    bool breathyStart = false;
    int weakMarks = 0;
};
} // namespace smoothharmony
