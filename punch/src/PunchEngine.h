#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Largest value in the last `window` samples, O(1) on average, with no allocation. rescale() multiplies every value in the window at
// once (0.6.2, for a Gain move): the values are kept divided by the scale, which is exactly 1 until then.
struct SlidingMax
{
    void prepare (int windowSamples)
    {
        window = windowSamples;
        size = window + 1;
        value.assign ((size_t) size, 0.0f);
        index.assign ((size_t) size, 0L);
        reset();
    }

    void reset()
    {
        std::fill (value.begin(), value.end(), 0.0f);
        std::fill (index.begin(), index.end(), 0L);
        head = tail = count = 0;
        now = 0;
        scale = inverse = 1.0f;
    }

    void rescale (float r)
    {
        scale *= r;
        inverse = 1.0f / scale;
    }

    float push (float in)
    {
        const float x = in * inverse;
        while (count > 0)
        {
            const int back = (tail + size - 1) % size;
            if (value[(size_t) back] > x)
                break;
            tail = back;
            --count;
        }
        value[(size_t) tail] = x;
        index[(size_t) tail] = now;
        tail = (tail + 1) % size;
        ++count;
        while (index[(size_t) head] <= now - window)
        {
            head = (head + 1) % size;
            --count;
        }
        ++now;
        return value[(size_t) head] * scale;
    }

    std::vector<float> value;
    std::vector<long> index;
    int window = 1, size = 2, head = 0, tail = 0, count = 0;
    long now = 0;
    float scale = 1.0f, inverse = 1.0f;
};

// How much of the sound's power came at each level against the recent peak, averaged over the level match's own time (0.6.2). Punch's
// reduction depends only on that level and on Punch, so from this the level match can be worked out for any Punch at once: when Punch
// moves, the match carries over to the new Punch instead of learning it again over a second. O(1) a sample: every bin fades by the same
// amount, so the bins are kept divided by that fade. Read only while Punch moves. No allocation.
struct LevelHistogram
{
    static constexpr int numBins = 93;              // one for everything 40 dB or more under the peak, where no Punch acts, then -40 to +6 dB
    static constexpr float lowestDb = -40.0f, stepDb = 0.5f;

    void prepare (float coefficient) { add = coefficient; keep = 1.0 - (double) coefficient; grow = 1.0 / keep; reset(); }
    void reset() { power.fill (0.0); level.fill (0.0); scale = inverse = 1.0; }

    void push (float p, float levelDb)
    {
        scale *= keep;
        inverse *= grow;
        if (scale < 1.0e-200)
        {
            for (int k = 0; k < numBins; ++k) { power[(size_t) k] *= scale; level[(size_t) k] *= scale; }
            scale = inverse = 1.0;
        }
        const int k = ! (levelDb >= lowestDb) ? 0 : std::min (numBins - 1, 1 + (int) ((levelDb - lowestDb) * (1.0f / stepDb)));
        const double a = (double) add * (double) p * inverse;
        power[(size_t) k] += a;
        level[(size_t) k] += a * (double) levelDb;
    }

    // A change of level: every power by r squared. The levels against the peak do not move.
    void rescale (double r2) { scale *= r2; inverse = 1.0 / scale; }

    // The power after a reduction, sum over the bins of power times gainSquared (level), for the bins at or above fromDb.
    template <typename GainSquared>
    double after (GainSquared gainSquared, float fromDb = -1.0e9f) const
    {
        double sum = 0.0;
        for (int k = 0; k < numBins; ++k)
            if (power[(size_t) k] > 0.0)
            {
                const float db = (float) (level[(size_t) k] / power[(size_t) k]);
                if (db >= fromDb)
                    sum += power[(size_t) k] * (double) gainSquared (db);
            }
        return sum * scale;
    }

    std::array<double, numBins> power {}, level {};
    double scale = 1.0, inverse = 1.0, keep = 1.0, grow = 1.0;
    float add = 0.0f;
};

// Whether the sound has a pitch right now. Steady Notes only: a note is a sound with a pitch, and its tail is what comes right after
// the pitch stops. Read every 5 ms the way a pitch tracker reads it: the last 20 ms against the same 20 ms one period earlier, for every
// period from 1100 Hz down to 60 Hz, at 12 kHz. No allocation after prepare. While a note is held only the periods next to the last one
// are tried, so a steady voice costs next to nothing; the full search runs when the pitch is lost.
class PitchSense
{
public:
    void prepare (double sampleRate)
    {
        drop = std::max (1, (int) std::lround (sampleRate / 12000.0));
        const double rate = sampleRate / drop;
        lowpass = 1.0 - std::exp (-2.0 * 3.14159265358979323846 * 2500.0 / sampleRate);
        tauMin = std::max (2, (int) (rate / 1100.0));
        tauMax = (int) (rate / 60.0) + 1;
        window = tauMax + std::max (8, (int) std::lround (rate / 300.0));
        hop = std::max (1, (int) std::lround (0.005 * rate));
        need = window + tauMax + 2;
        buffer.assign ((size_t) (need + hop), 0.0f);
        squares.assign ((size_t) need + 1, 0.0);
        difference.assign ((size_t) tauMax + 3, 1.0f);
        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        stage1 = stage2 = 0.0;
        count = 0;
        filled = need;
        lastTau = 0;
        pitchedFrames = plainFrames = 0;
        voiced = false;
    }

    void push (float sample)
    {
        stage1 += lowpass * ((double) sample - stage1);
        stage2 += lowpass * (stage1 - stage2);
        if (++count < drop)
            return;
        count = 0;
        buffer[(size_t) filled++] = (float) stage2;
        if (filled < need + hop)
            return;
        const bool pitched = analyse (buffer.data() + hop);
        std::copy (buffer.begin() + hop, buffer.end(), buffer.begin());
        filled = need;
        // Two frames with a pitch make a voice, three without end it: a pitch tracker bridges a gap of 10 ms the same way.
        if (pitched) { plainFrames = 0; if (++pitchedFrames >= 2) voiced = true; }
        else         { pitchedFrames = 0; if (++plainFrames >= 3) voiced = false; }
    }

    bool voiced = false;

private:
    static constexpr float clearEnough = 0.35f;     // How unlike its own copy one period earlier the sound may be and still count as pitched.
    static constexpr double quietest = 1.0e-10;     // Mean square under which nothing has a pitch: -100 dBFS.

    float differenceAt (const float* newest, double energy, int tau) const
    {
        const float* lagged = newest - tau;
        float x0 = 0.0f, x1 = 0.0f, x2 = 0.0f, x3 = 0.0f;
        int i = 0;
        for (; i + 4 <= window; i += 4)
        {
            x0 += newest[i] * lagged[i];
            x1 += newest[i + 1] * lagged[i + 1];
            x2 += newest[i + 2] * lagged[i + 2];
            x3 += newest[i + 3] * lagged[i + 3];
        }
        for (; i < window; ++i)
            x0 += newest[i] * lagged[i];
        const int from = tauMax + 2 - tau;
        const double laggedEnergy = squares[(size_t) (from + window)] - squares[(size_t) from];
        return (float) (1.0 - 2.0 * (double) (x0 + x1 + x2 + x3) / (energy + laggedEnergy + 1.0e-20));
    }

    bool analyse (const float* s)
    {
        squares[0] = 0.0;
        for (int i = 0; i < need; ++i)
            squares[(size_t) i + 1] = squares[(size_t) i] + (double) s[i] * (double) s[i];
        const float* newest = s + tauMax + 2;
        const double energy = squares[(size_t) need] - squares[(size_t) (tauMax + 2)];
        if (energy / window <= quietest)
        {
            lastTau = 0;
            return false;
        }
        if (lastTau > 0)
        {
            const int lo = std::max (tauMin - 1, lastTau - 2), hi = std::min (tauMax + 1, lastTau + 2);
            for (int tau = lo; tau <= hi; ++tau)
                difference[(size_t) tau] = differenceAt (newest, energy, tau);
            for (int tau = std::max (tauMin, lo + 1); tau <= std::min (tauMax, hi - 1); ++tau)
                if (difference[(size_t) tau] < clearEnough && difference[(size_t) tau] < difference[(size_t) tau - 1] && difference[(size_t) tau] <= difference[(size_t) tau + 1])
                {
                    lastTau = tau;
                    return true;
                }
        }
        for (int tau = tauMin - 1; tau <= tauMax + 1; ++tau)
            difference[(size_t) tau] = differenceAt (newest, energy, tau);
        // A pitch has to follow on from the one before: a jump costs half its size in octaves, the way a pitch tracker charges for it.
        // Noise and room hum throw up a clear period now and then, but never the same one twice.
        int best = 0, clearest = 0;
        float bestCost = clearEnough;
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            const float d = difference[(size_t) tau];
            if (d >= clearEnough || d >= difference[(size_t) tau - 1] || d > difference[(size_t) tau + 1])
                continue;
            if (clearest == 0 || d < difference[(size_t) clearest])
                clearest = tau;
            const float cost = d + (lastTau > 0 ? 0.5f * std::abs (std::log2 ((float) tau / (float) lastTau)) : 0.0f);
            if (cost < bestCost)
            {
                best = tau;
                bestCost = cost;
            }
        }
        lastTau = best > 0 ? best : clearest; // With no pitch that follows on, the clearest period is kept for the next frame to confirm.
        return best > 0;
    }

    std::vector<float> buffer, difference;
    std::vector<double> squares;
    double lowpass = 1.0, stage1 = 0.0, stage2 = 0.0;
    int drop = 4, count = 0, tauMin = 10, tauMax = 201, window = 241, hop = 60, need = 444, filled = 0, lastTau = 0;
    int pitchedFrames = 0, plainFrames = 0;
};

// The detector. Takes one rectified sidechain sample and returns the gain to apply, in dB, to the delayed audio.
// Attack and Sustain only ever compare envelopes with each other as a ratio, and every envelope here is a
// linear follower, so scaling the input scales every envelope by the same amount and the ratios do not move.
// That is what makes the shaper independent of input level. Punch sets its threshold from the recent peak level
// for the same reason.
// Impact is the one part that does care about level, on purpose: it has a fixed threshold, so turning up Gain drives it harder.
class PunchEngine
{
public:
    // Fixed times, in seconds. Not on the panel.
    static constexpr double holdTime = 0.015;      // Peak hold window. Covers half a cycle down to 33 Hz, so a steady tone gives a flat envelope.
    static constexpr double fastRise = 0.0002;
    static constexpr double attackSlowRise = 0.008; // How long the "start of a sound" lasts.
    static constexpr double attackFall = 0.012;
    static constexpr double sustainFastFall = 0.015;
    static constexpr double sustainSlowFall = 0.300;
    static constexpr double compHoldTime = 0.008;  // Punch has its own shorter hold: half a cycle down to 62 Hz. Its release has to be quick to change the shape of a sound.
    static constexpr double compRise = 0.0002;
    static constexpr double compFall = 0.008;
    static constexpr double peakRefFall = 1.5;     // How long the compressor remembers the loudest recent level.
    static constexpr double powerTime = 0.300;     // Averaging for the level match.
    static constexpr double makeupTime = 0.200;
    static constexpr double gainTime = 0.0005;     // Final gain smoothing.
    static constexpr double impactAttack = 0.005;
    static constexpr double impactFastRelease = 0.090;  // After a short peak.
    static constexpr double impactSlowRise = 0.400;     // How long a level has to last before it counts as sustained.
    static constexpr double impactSlowRelease = 0.500;  // After a sustained level, so a held note doesn't let go with a jump, but the next phrase isn't held down.
    static constexpr double guardRise = 0.005;
    static constexpr double guardFall = 0.200;
    // Steady Notes only. Off, none of these is used and Sustain and Punch are exactly what they were.
    static constexpr double noteTime = 0.080;      // How long a sound has to keep its pitch before it counts as a note.
    static constexpr double tailBoostFull = 0.070; // After the pitch stops, a boost of the tail stays in full this long
    static constexpr double tailBoostEnd = 0.105;  // and is gone by here, so the breath after a word is not lifted with it.
    static constexpr double tailCutFull = 0.110;   // A cut of the tail lasts longer: it is the end of the word that is being shortened,
    static constexpr double tailCutEnd = 0.130;    // and nothing is made louder by it.
    static constexpr double noteFall = 1.0;        // How slowly the level a note is remembered at comes down while it is sung.
    static constexpr double sunkGrace = 0.040;     // How long a note that has sunk right down may go on before it is over.
    static constexpr double voiceGone = 1.0;       // With no note for this long there is no voice to care for, and Sustain works as with
    static constexpr double voiceGoneRamp = 0.5;   // Steady Notes off, coming in over this long (0.6.1: it did nothing at all to drums).
    static constexpr double sagWindow = 0.100;     // A held note's sag is measured from the highest it stood in this long (0.5.1).
    static constexpr double gateRamp = 0.005;      // How fast Sustain comes in when a note begins, so the gain never steps.
    static constexpr double restartTime = 0.002;   // How fast a sharp start pulls the tail's reference down to the new sound.
    static constexpr double brightRise = 0.001;
    static constexpr double brightFall = 0.010;
    static constexpr double brightLowHz = 4650.0;  // A sound whose sample to sample step is as large as a tone's at this pitch starts to be spared,
    static constexpr double brightHighHz = 6100.0; // and at this one Sustain leaves it alone: an S is nearly all step, a vowel is not.
    static constexpr double levelTime = 0.015;     // Punch on a held note reads the level over this long,
    static constexpr double crestHeldTime = 1.0;   // and lets the peaks inside the note move its reading only this slowly,
    static constexpr double crestFreeTime = 0.003; // while anything that is not a held note is read by its peaks, as ever.
    static constexpr double makeupHeldTime = 3.0;  // How slowly Punch's level match moves once it is right.
    static constexpr double pauseHold = 0.5;       // How long after the voice the level match stays up,
    static constexpr double pauseFade = 0.5;       // and how long it takes to go.
    static constexpr double plainFade = 0.020;     // Steady Notes: how fast Sustain's plain reading leaves when a pitch is heard, and comes back if it was not a note (0.6.2).
    static constexpr int rebaseEvery = 16;         // While Punch moves, its level match is carried over every this many samples (0.6.2).

    void prepare (double sampleRate)
    {
        const auto coef = [sampleRate] (double t) { return (float) (1.0 - std::exp (-1.0 / (t * sampleRate))); };
        cFastRise = coef (fastRise);
        cAttackSlowRise = coef (attackSlowRise);
        cAttackFall = coef (attackFall);
        cSustainFastFall = coef (sustainFastFall);
        cSustainSlowFall = coef (sustainSlowFall);
        cCompRise = coef (compRise);
        cCompFall = coef (compFall);
        cPeakRefFall = coef (peakRefFall);
        cPower = coef (powerTime);
        cMakeup = coef (makeupTime);
        cGain = coef (gainTime);
        cImpactAttack = coef (impactAttack);
        cImpactFastRelease = coef (impactFastRelease);
        cImpactSlowRise = coef (impactSlowRise);
        cImpactSlowRelease = coef (impactSlowRelease);
        cGuardRise = coef (guardRise);
        cGuardFall = coef (guardFall);

        noteSamples = std::max (1, (int) std::lround (noteTime * sampleRate));
        boostFull = (int) std::lround (tailBoostFull * sampleRate);
        boostEnd = std::max (boostFull + 1, (int) std::lround (tailBoostEnd * sampleRate));
        cutFull = (int) std::lround (tailCutFull * sampleRate);
        cutEnd = std::max (cutFull + 1, (int) std::lround (tailCutEnd * sampleRate));
        gateStep = (float) (1.0 / (gateRamp * sampleRate));
        startDeadUpStep = (heldStartDb - deadZoneDb) / (float) (0.030 * sampleRate);
        startDeadDownStep = (heldStartDb - deadZoneDb) / (float) (0.005 * sampleRate);
        cNoteFall = coef (noteFall);
        graceSamples = std::max (1, (int) std::lround (sunkGrace * sampleRate));
        voiceGoneSamples = std::max (1, (int) std::lround (voiceGone * sampleRate));
        voiceGoneRampSamples = std::max (1, (int) std::lround (voiceGoneRamp * sampleRate));
        cRestart = coef (restartTime);
        cBrightRise = coef (brightRise);
        cBrightFall = coef (brightFall);
        const auto stepOf = [sampleRate] (double hz) { return (float) (2.0 * std::sin (3.14159265358979323846 * std::min (0.5, hz / sampleRate))); };
        brightLow = stepOf (brightLowHz);
        brightHigh = std::max (brightLow + 1.0e-3f, stepOf (brightHighHz));
        cLevel = coef (levelTime);
        cCrestHeld = coef (crestHeldTime);
        cCrestFree = coef (crestFreeTime);
        cMakeupHeld = coef (makeupHeldTime);
        pauseHoldSamples = std::max (1, (int) std::lround (pauseHold * sampleRate));
        pauseFadeSamples = std::max (1, (int) std::lround (pauseFade * sampleRate));
        plainStep = (float) (1.0 / std::max (1.0, plainFade * sampleRate));
        plainLevels.prepare (cPower);
        steadyLevels.prepare (cPower);
        pitch.prepare (sampleRate);

        shaperHold.prepare (std::max (1, (int) std::lround (holdTime * sampleRate)));
        noteRecent.prepare (std::max (1, (int) std::lround (sagWindow * sampleRate)));
        compHold.prepare (std::max (1, (int) std::lround (compHoldTime * sampleRate)));
    }

    void reset (float punch)
    {
        shaperHold.reset();
        compHold.reset();
        attackFast = attackSlow = sustainFast = sustainSlow = sustainSlowPlain = 0.0f;
        compEnv = peakRef = 0.0f;
        inPower = outPower = steadyIn = steadyOut = 0.0f;
        gainDb = 0.0f;
        makeupDb = steadyMakeup = restingMakeupDb (punch);
        handoverDb = 0.0f;
        plainLevels.reset();
        steadyLevels.reset();
        lastPunch = rebasedPunch = punch;
        sinceRebase = 0;
        lastImpact = 0.0f;
        impactLevelDb = -120.0f;
        plainQuietFor = 0;
        fresh = true;
        impactFast = impactSlow = impactNowDb = quietGuard = 0.0f;
        pitch.reset();
        noteRecent.reset();
        notedFor = 0;
        noteOn = false;
        sinceNote = farFromANote;
        tailGate = noteLevel = 0.0f;
        startDeadDb = deadZoneDb;
        sunkFor = 0;
        stepEnv = sizeEnv = lastSample = 0.0f;
        levelPower = crestDb = overDb = 0.0f;
        makeupGuard = 1.0f;
        givenDb = 0.0f;
        steadyLevelDb = 0.0f;
        plainWeight = 1.0f;
        noteAge = 0;
        matched = false;
        quietFor = 0;
        steadyWasOn = false;
    }

    // Attack and Sustain in dB, punch and impact from 0 to 1. steadyNotes turns Sustain's care for a voice on; sample is the signal
    // itself (one channel or the two mixed), which only that reads.
    float process (float rectified, float attackDb, float sustainDb, float punch, float impact, bool steadyNotes = false, float sample = 0.0f)
    {
        const float x = rectified < 1.0e6f ? rectified : 0.0f; // Also catches NaN, so bad input can't poison the envelopes.
        const float hold = shaperHold.push (x);
        const float compPeak = compHold.push (x);

        follow (attackFast, hold, cFastRise, cAttackFall);
        follow (attackSlow, hold, cAttackSlowRise, cAttackFall);
        follow (sustainFast, hold, cFastRise, cSustainFastFall);
        follow (sustainSlow, hold, cFastRise, cSustainSlowFall);
        follow (sustainSlowPlain, hold, cFastRise, cSustainSlowFall);
        follow (compEnv, compPeak, cCompRise, cCompFall);
        follow (peakRef, compPeak, 1.0f, cPeakRefFall);
        // sustainSlow is Steady Notes' own: a sharp start pulls it down to the new sound. sustainSlowPlain never is, and is what Sustain
        // reads with Steady Notes off (0.6.2), so switching Steady Notes off lands on that reading at once. With Steady Notes off from the
        // start the two are the same.
        if (steadyNotes && ! steadyWasOn)
            sustainSlow = sustainSlowPlain;

        // Start of a sound: the fast envelope is ahead of the slow one. Tail: the slow one is above the fast one.
        const float startDb = toDb (attackFast) - toDb (attackSlow);
        const float tailDb = toDb (steadyNotes ? sustainSlow : sustainSlowPlain) - toDb (sustainFast);
        float startAmount = clamp01 ((startDb - deadZoneDb) / startRangeDb);
        float tailAmount = clamp01 ((tailDb - deadZoneDb) / tailRangeDb);
        if (steadyNotes)
        {
            // Where there is no voice Sustain reads the level plainly (tailAmount), but from Steady Notes' own slow envelope, which a sharp
            // start pulls down (rule 4 below): a phrase start after a pause, a breath or a mouth sound is a new sound, not the tail of what
            // came before. (0.6.2 tried the reading with Steady Notes off here, for drums, where a soft hit right after a loud one gets no
            // tail with Steady Notes on; on Luke's rap take 16 that gave phrase starts Sustain's full 9 dB, so it was not kept.)
            tailAmount = steadyTailAmount (startDb, sample, sustainDb, tailAmount);
            startAmount = steadyStartAmount (startDb);
        }

        const float reductionDb = steadyNotes ? steadyReductionDb (punch, x) : compReductionDb (punch);

        // 0.6.2. A Punch move is a change of setting, not of the sound: the level match carries over to the new Punch at once, worked out
        // from how much of the sound came at each level (LevelHistogram), instead of lagging behind it for a second. Every few samples
        // while Punch moves, and once more when it stops. With Punch still, none of this runs.
        if (punch != rebasedPunch && (punch == lastPunch || ++sinceRebase >= rebaseEvery))
        {
            carryMatchOver (rebasedPunch, punch);
            rebasedPunch = punch;
            sinceRebase = 0;
        }
        lastPunch = punch;

        // Level match from average power with and without the reduction. Held when there is nothing to measure.
        const float reductionGain = std::exp (-reductionDb * dbToLn);
        const float inP = x * x;
        const float outP = inP * reductionGain * reductionGain;
        float makeupNowDb = 0.0f;
        if (steadyNotes)
        {
            if (! steadyWasOn)
                steadyLevels = plainLevels;
            makeupNowDb = steadyMakeupDb (punch, inP, outP);
            steadyLevels.push (inP, steadyLevelDb);
        }
        // The plain level match runs always, with Steady Notes on too, so that switching it off lands on what the plain match has been
        // all along (0.6.2: it took over the Steady Notes match and fell away for a second). With Steady Notes off this is 0.4.0's.
        // The level against the recent peak, read with a quick logarithm (within 0.01 dB): it sorts the sound into half dB bins, and with
        // Steady Notes on it gives the plain match's reduction, which only that match uses. With Steady Notes off the match uses the exact
        // reduction, as ever.
        const float plainLevelDb = 6.02059991f * quickLog2 ((compEnv + epsilon) / std::max (peakRef + epsilon, peakRefFloor));
        const float plainOutP = steadyNotes ? inP * quickPowerLeft (plainLevelDb, punch) : outP;
        plainLevels.push (inP, plainLevelDb);
        // How long the sound has been under the level Punch works at, kept with Steady Notes off too, so that Steady Notes switched on
        // between phrases knows it is between phrases (0.6.2).
        plainQuietFor = punch > 0.0f && plainLevelDb + depthNow (punch) >= -guardKneeDb ? 0 : std::min (plainQuietFor + 1, pauseHoldSamples + pauseFadeSamples);
        inPower += cPower * (inP - inPower);
        outPower += cPower * (plainOutP - outPower);
        float makeupTarget = restingMakeupDb (punch);
        if (inPower > gateFloorPower && outPower > 0.0f)
            makeupTarget = std::min (maxMakeupDb, std::max (0.0f, 10.0f * std::log10 (inPower / outPower)));
        makeupDb += cMakeup * (makeupTarget - makeupDb);
        if (! steadyNotes)
        {
            // Steady Notes just went off: from what it gave to the plain match, at the plain match's own speed.
            if (steadyWasOn)
                handoverDb = givenDb - makeupDb;
            makeupNowDb = makeupDb + handoverDb;
            handoverDb -= cMakeup * handoverDb;
        }

        // Impact: two release speeds. The slow stage follows the level itself, not the fast stage's release, so it only builds up on a level that lasts and a short peak lets go quickly.
        // 0.6.2: when Impact moves, the reduction it holds goes to what the new Impact holds for the same sound, so it leaves with its makeup
        // and does not let go over a second after the makeup has gone.
        if (impact != lastImpact)
        {
            moveImpact (lastImpact, impact, 0.0f);
            lastImpact = impact;
        }
        const float heldDb = toDb (hold);
        const float impactTarget = impactReductionDb (heldDb, impact);
        impactFast += (impactTarget > impactFast ? cImpactAttack : cImpactFastRelease) * (impactTarget - impactFast);
        impactSlow += (impactTarget > impactSlow ? cImpactSlowRise : cImpactSlowRelease) * (impactTarget - impactSlow);
        impactNowDb = std::max (impactFast, impactSlow);
        // The level that built the reduction Impact holds: followed while the slow stage rises or holds, kept while it lets go. Only read
        // when Impact or Gain moves.
        if (impactTarget >= impactSlow)
            impactLevelDb += cImpactSlowRise * (heldDb - impactLevelDb);
        // Impact's makeup fades out on near silence, so room noise between phrases isn't lifted with the voice.
        const float guardTarget = clamp01 ((heldDb - guardFloorDb) / guardRangeDb);
        quietGuard += (guardTarget > quietGuard ? cGuardRise : cGuardFall) * (guardTarget - quietGuard);

        const float total = attackDb * startAmount + sustainDb * tailAmount - reductionDb + makeupNowDb - impactNowDb + impactMakeupDb (impact) * quietGuard;
        steadyWasOn = steadyNotes;
        fresh = false;
        gainDb += cGain * (std::min (maxGainDb, std::max (minGainDb, total)) - gainDb);
        return gainDb;
    }

    // 0.6.2. Gain moved: the level going in is now r times what it was. Gain is a level in front of everything, so the detector takes the
    // new level as the level it has always had. Attack, Sustain and Punch ignore level, so a Gain move changes nothing they do; Impact goes
    // to the new level's reduction without letting go late. Only called when Gain moves.
    void levelChanged (float r)
    {
        if (! (r > 0.0f && r < 1.0e6f))
            return;
        const float r2 = r * r;
        for (float* e : { &attackFast, &attackSlow, &sustainFast, &sustainSlow, &sustainSlowPlain, &compEnv, &peakRef, &noteLevel })
            *e *= r;
        for (float* e : { &inPower, &outPower, &steadyIn, &steadyOut, &levelPower })
            *e *= r2;
        plainLevels.rescale ((double) r2);
        steadyLevels.rescale ((double) r2);
        shaperHold.rescale (r);
        compHold.rescale (r);
        noteRecent.rescale (r);
        moveImpact (lastImpact, lastImpact, 20.0f * std::log10 (r));
    }

    // For the window's drawing of a hit, which is made from levels and has no sound to hear a pitch in: every sound counts as pitched.
    void setLevelsOnly (bool on) { levelsOnly = on; }

    static float dbToGain (float db) { return std::exp (db * dbToLn); }

    // What a level at the recent peak gets reduced by. Depth is how far below the recent peak the threshold sits.
    static float depthDb (float punch) { return 36.0f * curve (punch); }
    static float ratio (float punch) { return 1.0f + 9.0f * curve (punch); }

    // Impact's fixed curve. More Impact means a lower threshold, a higher ratio and more makeup, all from one control.
    static float impactThresholdDb (float impact) { return -6.0f - 30.0f * impact; }
    static float impactRatio (float impact) { return 1.0f + 5.0f * impact; }
    static float impactMakeupDb (float impact) { return 0.8f * -impactThresholdDb (impact) * (1.0f - 1.0f / impactRatio (impact)); }

    // How far Impact is pulling the level down right now, in dB. For the meter.
    float impactReductionNowDb() const { return impactNowDb; }

private:
    static constexpr float dbToLn = 0.11512925464970229f;
    static constexpr float epsilon = 1.0e-6f;         // -120 dBFS.
    static constexpr float deadZoneDb = 0.25f;        // So tiny peak differences on a steady tone do nothing at all.
    static constexpr float startRangeDb = 6.0f;
    static constexpr float tailRangeDb = 2.5f;
    static constexpr float heldDepthDb = 6.0f;        // Steady Notes: how far a note may sag before Sustain calls it a tail.
    static constexpr int farFromANote = 1 << 28;      // Steady Notes: samples since the last note, when there has been none.
    static constexpr float guardKneeDb = 3.0f;        // Steady Notes: Punch's level match is given in full from this far under its threshold
    static constexpr float guardSpanDb = 15.0f;       // and not at all this much further down.
    static constexpr float trimReachDb = 18.0f;
    static constexpr float matchedWithinDb = 0.05f;   // Steady Notes: the level match counts as right this close (0.5.0 took 0.5 dB, and a steady tone stayed 0.19 dB low).
    static constexpr float afterNoteDb = 1.0f;        // Steady Notes: after the note a sound has to be this much further under it to be its tail:
                                                      // a quieter word that starts right after is not.
    static constexpr float heldStartDb = 4.0f;        // Steady Notes: inside a sounding note a start has to rise this far before Attack acts.
    static constexpr float sharpStartDb = 3.0f;       // Steady Notes: a start this sharp is a new sound.
    static constexpr float noteStartGain = 0.5f;      // Steady Notes: a note starts within 6 dB of where the level is.
    static constexpr float noteDepthGain = 0.0631f;   // Steady Notes: a pitch 24 dB under the note's level is no longer the note.
    static constexpr float kneeDb = 6.0f;
    static constexpr float peakRefFloorDb = -60.0f;   // Below this Punch stops chasing the level, so noise is left alone.
    static constexpr float gateFloorPower = 1.0e-7f;  // -70 dBFS.
    static constexpr float maxMakeupDb = 36.0f;
    static constexpr float maxGainDb = 40.0f;
    static constexpr float minGainDb = -60.0f;
    static constexpr float guardFloorDb = -70.0f;     // Impact's makeup is gone at this level, measured after Gain.
    static constexpr float guardRangeDb = 20.0f;      // And fully back this far above it.

    // Punch to the power 1.25. Spreads the audible change more evenly across the control.
    static float curve (float punch) { return punch * std::sqrt (std::sqrt (punch)); }
    static float toDb (float x) { return 8.685889638f * std::log (x + epsilon); }
    static constexpr float peakRefFloor = 1.0e-3f;    // peakRefFloorDb as a level
    // 2 to the y for y from about -100 to 0, within about 0.07 %: the integer part into the exponent, and a cubic on the rest.
    static float quickExp2 (float y)
    {
        y = std::max (y, -120.0f);
        const float fl = std::floor (y), f = y - fl;
        const float m = ((0.07944024f * f + 0.22741418f) * f + 0.69314718f) * f + 1.0f;   // 2^f on [0, 1) to within 0.07 %
        const std::uint32_t bits = (std::uint32_t) ((int) fl + 127) << 23;
        float e;
        std::memcpy (&e, &bits, sizeof e);
        return m * e;
    }

    // powerLeft with the quick exponent and Punch's curve worked out once per Punch: for the plain level match kept while Steady Notes is on.
    float quickPowerLeft (float levelDb, float punch)
    {
        if (punch <= 0.0f)
            return 1.0f;
        return quickExp2 (-softKneeAbove (levelDb + depthNow (punch)) * slopeNow * 0.33219281f);
    }

    float depthNow (float punch)
    {
        if (punch != curvePunch)
        {
            curvePunch = punch;
            curveDepth = depthDb (punch);
            slopeNow = 1.0f - 1.0f / ratio (punch);
        }
        return curveDepth;
    }

    // log2 for a positive normal float, within about 0.0015 (0.01 dB as a level): the exponent, and a cubic on the mantissa.
    static float quickLog2 (float x)
    {
        std::uint32_t bits;
        std::memcpy (&bits, &x, sizeof bits);
        const float e = (float) ((int) ((bits >> 23) & 255u) - 127);
        bits = (bits & 0x7FFFFFu) | 0x3F800000u;
        float m;
        std::memcpy (&m, &bits, sizeof m);
        return e + ((0.15824871f * m - 1.05187502f) * m + 3.04788335f) * m - 2.15458178f;
    }
    static float clamp01 (float x) { return x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : x); }
    static void follow (float& env, float target, float rise, float fall) { env += (target > env ? rise : fall) * (target - env); }

    static float restingMakeupDb (float punch)
    {
        const float r = ratio (punch);
        return 0.35f * depthDb (punch) * (1.0f - 1.0f / r); // Deliberately low, so quiet noise after a long silence isn't lifted much.
    }

    // Sustain for a voice (Steady Notes). On Luke's real takes (2026-11-11) Sustain moved held vowels in and out by its full amount, gave a
    // quiet phrase start the tail gain of the louder phrase before it, lifted every S, and lifted the breath after a word as if it were
    // the word's tail. So Sustain now follows the note, not a level curve alone:
    // 1. A note is a sound with a pitch that has lasted noteTime. While it has its pitch it may sag by heldDepthDb before Sustain acts,
    //    so a held vowel stays still and only a note that is really dying away is shaped.
    // 2. The tail is what comes right after the pitch stops: Sustain stays on it for a set time and then lets go. A breath, a mouth sound
    //    or the room after that is not the word's tail and is left alone. A boost lets go sooner than a cut.
    // 3. Inside that time a new sound is still part of the tail unless it comes back up to the note's own level.
    // 4. A sound with no note before it gets nothing: a sharp start pulls the reference down to it, so the next note is read on its own.
    // 5. A bright sound (an S) is spared: its sample to sample step is nearly as large as the sample itself.
    // Everything is a ratio or a time, so level still does not matter.
    float steadyTailAmount (float startDb, float sample, float sustainDb, float plainAmount)
    {
        const float clean = sample > -1.0e6f && sample < 1.0e6f ? sample : 0.0f; // Also catches NaN.
        if (! steadyWasOn)
        {
            pitch.reset();
            noteRecent.reset();
            notedFor = 0;
            noteOn = false;
            sinceNote = farFromANote;
            tailGate = 0.0f;
            noteLevel = sustainSlow;
            startDeadDb = deadZoneDb;
            sunkFor = 0;
            plainWeight = 1.0f;
            noteAge = 0;
        }
        if (! levelsOnly)
            pitch.push (clean);

        const int full = sustainDb < 0.0f ? cutFull : boostFull, end = sustainDb < 0.0f ? cutEnd : boostEnd;

        // A note starts as a pitch that has lasted noteTime, and lasts while the pitch does. A room hums, and its hum has a pitch too, so
        // level matters as well: a pitch counts only while it stands within noteDepth of where the level was (for a note that is
        // sounding, of the note's own level). A note that has sunk further than that is over a moment later, whatever pitch the
        // room goes on humming. And a note can only start near where the level is, not far down in what is left of the sound before:
        // that is how the hum of the room, once the voice has gone, is never taken for a note that is dying away.
        // The window's drawing of a hit has only levels, no pitch. There the hit's body counts as the note, from its start, until it has
        // sagged heldDepthDb under the note's level; that is where the drawing puts "the pitch stops", so the tail window is drawn after it.
        // (Up to 0.5.1 every sound counted as pitched, but a hit that falls 6 dB within 80 ms never became a note, so Sustain was drawn
        // doing nothing at all with Steady Notes on.)
        const bool pitched = levelsOnly ? sustainFast > 0.0f && sustainFast >= noteLevel * noteStartGain : pitch.voiced;
        const bool alive = pitched && sustainFast >= noteLevel * noteDepthGain;
        sunkFor = alive ? 0 : std::min (sunkFor + 1, graceSamples);
        if (! pitched || sunkFor >= graceSamples)
        {
            noteOn = false;
            notedFor = 0;
        }
        else if (! noteOn && alive && sustainFast >= noteLevel * noteStartGain)
        {
            if (notedFor < noteSamples)
                ++notedFor;
            noteOn = levelsOnly || notedFor >= noteSamples;
        }
        if (noteOn)
            sinceNote = 0;
        else if (sinceNote < farFromANote)
            ++sinceNote;
        noteAge = noteOn ? std::min (noteAge + 1, noteSamples) : 0;

        const bool inTail = ! noteOn && sinceNote < end;
        const float gateTarget = noteOn ? 1.0f : (sinceNote <= full ? 1.0f : (sinceNote >= end ? 0.0f : (float) (end - sinceNote) / (float) (end - full)));
        tailGate = tailGate < gateTarget ? std::min (gateTarget, tailGate + gateStep) : std::max (gateTarget, tailGate - gateStep);

        if (startDb > sharpStartDb && ! inTail && sustainSlow > sustainFast)
            sustainSlow += cRestart * (sustainFast - sustainSlow);

        // What the tail is measured against. While the note has its pitch: where its level just was, as ever. After the pitch has stopped:
        // the level the note had, held still, so what follows is judged against the note and not against its own dying away.
        if (noteOn)
            noteLevel = sustainSlow > noteLevel ? sustainSlow : noteLevel + cNoteFall * (sustainSlow - noteLevel);
        else if (! inTail || sustainSlow > noteLevel)
            noteLevel = sustainSlow;
        // Inside a note the sag is also measured from the highest the note stood in the last sagWindow, not only from the slow envelope:
        // after a loud start the slow envelope is still coming down when the note has settled, and a small dip of the settled note
        // read as a note dying away (0.5.0 gave such a held vowel up to 7.6 dB for 30 ms on Luke's take 04 at 149.86 s).
        const float recentDb = toDb (noteRecent.push (sustainFast));
        const float tailDb = noteOn ? std::min (toDb (sustainSlow), recentDb) - toDb (sustainFast) : toDb (noteLevel) - toDb (sustainFast) - afterNoteDb;

        const float step = std::abs (clean - lastSample), size = std::abs (clean);
        lastSample = clean;
        follow (stepEnv, step, cBrightRise, cBrightFall);
        follow (sizeEnv, size, cBrightRise, cBrightFall);
        const float bright = clamp01 ((stepEnv / (sizeEnv + 1.0e-9f) - brightLow) / (brightHigh - brightLow));

        // Only a boost spares a bright sound: a cut that shortens the end of a word shortens its S with it and makes nothing louder.
        const float voiced = clamp01 ((tailDb - heldDepthDb) / tailRangeDb) * tailGate * (sustainDb > 0.0f ? 1.0f - bright : 1.0f);
        // 6. No voice: with no note for voiceGone (or none since Steady Notes came on, as on a drum track) Sustain is what it is with
        //    Steady Notes off, the plain reading of the level, faded in over voiceGoneRamp. Breaths and mouth sounds between phrases come
        //    well within voiceGone of a note and are still left alone.
        //    0.6.2: a pitch at the level where a note starts is a voice coming, so the plain reading leaves over plainFade as soon as one is
        //    heard, not all at once when the note is confirmed 80 ms later (that stepped the gain by Sustain's full amount in half a
        //    millisecond, in the first vowel of every phrase after a rest). If it was not a note after all it comes back as fast.
        if (levelsOnly)
            return voiced;
        const float plainTarget = sinceNote < voiceGoneSamples || notedFor > 0 ? 0.0f
                                : (sinceNote >= farFromANote ? 1.0f : std::min (1.0f, (float) (sinceNote - voiceGoneSamples) / (float) voiceGoneRampSamples));
        plainWeight = plainWeight < plainTarget ? std::min (plainTarget, plainWeight + plainStep) : std::max (plainTarget, plainWeight - plainStep);
        if (plainWeight <= 0.0f)
            return voiced;
        return voiced + plainWeight * (plainAmount - voiced);
    }

    // Attack for a voice (Steady Notes). Inside a held vowel the peaks jump by a few dB now and then while the level stays where it is
    // (on Luke's takes by 1.6 to 4.8 dB), and Attack took each jump for a start. While a note is sounding a start has to rise by
    // heldStartDb before Attack acts. The first sound after a pause, and any sound that is not a note yet, is read as ever.
    // (0.6.2: the window's drawing counts a hit as a note from its first moment, so there the dead zone waits until the note is as old as
    // a note the audio would have confirmed; it rose within the hit's first 30 ms and drew Attack smaller than the audio gives it.)
    float steadyStartAmount (float startDb)
    {
        const float target = noteOn && (! levelsOnly || noteAge >= noteSamples) ? heldStartDb : deadZoneDb;
        startDeadDb = startDeadDb < target ? std::min (target, startDeadDb + startDeadUpStep) : std::max (target, startDeadDb - startDeadDownStep);
        return clamp01 ((startDb - startDeadDb) / startRangeDb);
    }

    // Punch for a voice (Steady Notes). Punch reads peaks over 8 ms, and inside a held vowel the peaks move while the level does not
    // (the mouth changes shape, the pitch moves), so at 50 and above the gain wobbled a note that was steady. While a note is held the
    // reading is the level over levelTime plus how far the peaks stood over that level when the note began, and that distance moves only
    // slowly. Anything that is not a held note is read by its peaks, so the start of every word is shaped as before.
    float steadyReductionDb (float punch, float x)
    {
        const float peakDb = toDb (compEnv);
        if (! steadyWasOn)
        {
            levelPower = compEnv * compEnv;
            crestDb = 0.0f;
        }
        levelPower += cLevel * (x * x - levelPower);
        const float levelDb = 4.342944819f * std::log (levelPower + 1.0e-12f);
        crestDb += (noteOn ? cCrestHeld : cCrestFree) * (peakDb - levelDb - crestDb);
        steadyLevelDb = levelDb + crestDb - 6.02059991f * quickLog2 (std::max (peakRef + epsilon, peakRefFloor));
        if (punch <= 0.0f)
        {
            overDb = -100.0f;
            return 0.0f;
        }
        const float threshold = std::max (toDb (peakRef), peakRefFloorDb) - depthDb (punch);
        overDb = levelDb + crestDb - threshold;
        return softKneeAbove (overDb) * (1.0f - 1.0f / ratio (punch));
    }

    // Punch's level match for a voice (Steady Notes). The match used to be worked out again over every half second, so it fell away in
    // each pause and climbed back over the first third of a second of the next phrase: a held vowel at the start of a phrase rose by
    // several dB while it was sung. Now the match is learned on the voice only, and once it is right it moves only very slowly, so a held
    // note keeps one gain. It is then given to a sound by how near that sound is to the level Punch works at: in full at that level,
    // none of it guardSpanDb further down and none once the voice has been gone for a while. So it comes in with the first word of a
    // phrase and not after it, and the room and a far-off breath are not lifted with the voice.
    // 0.6.2: it has its own powers and match (steadyIn, steadyOut, steadyMakeup), taken over from the plain match when Steady Notes comes
    // on, as before. A Punch move no longer makes it learn again (from the level after the match, which locked it several dB low): the
    // match carries over to the new Punch (carryMatchOver) and is trimmed on from there.
    float steadyMakeupDb (float punch, float inP, float outP)
    {
        if (! steadyWasOn)
        {
            matched = false;
            givenDb = 0.0f;
            // Switched on while playing (0.6.2), the match is given as it would be now: between phrases, not at all. It was given in full
            // and fell away over a second, so the first word after the switch still had some of it. From a reset, as ever.
            makeupGuard = fresh ? 1.0f : guardFor (overDb, plainQuietFor);
            quietFor = fresh ? 0 : plainQuietFor;
            steadyIn = inPower;
            steadyOut = outPower;
            steadyMakeup = makeupDb;
        }
        if (! matched)
        {
            // Until the match is first right it is found the old way, quickly, from the level with and without the reduction.
            steadyIn += cPower * (inP - steadyIn);
            steadyOut += cPower * (outP - steadyOut);
            const bool measurable = steadyIn > gateFloorPower && steadyOut > 0.0f;
            const float target = measurable ? std::min (maxMakeupDb, std::max (0.0f, 10.0f * std::log10 (steadyIn / steadyOut))) : restingMakeupDb (punch);
            steadyMakeup += cMakeup * (target - steadyMakeup);
            if (noteOn && measurable && std::abs (target - steadyMakeup) < matchedWithinDb)
            {
                matched = true;
                steadyOut *= std::exp (2.0f * givenDb * dbToLn); // From here on the level after the match is what is measured.
            }
        }
        else if (overDb >= std::max (-guardKneeDb, depthDb (punch) - trimReachDb))
        {
            // Then it is trimmed, slowly, until what goes out is as loud as what came in. Only sound at the level Punch works at and
            // within trimReachDb of the loudest the voice has just been has a say: the voice itself, not what lies between the words.
            steadyIn += cPower * (inP - steadyIn);
            steadyOut += cPower * (outP * std::exp (2.0f * givenDb * dbToLn) - steadyOut);
            if (steadyIn > gateFloorPower && steadyOut > 0.0f)
                steadyMakeup = std::min (maxMakeupDb, std::max (0.0f, steadyMakeup + cMakeupHeld * 10.0f * std::log10 (steadyIn / steadyOut)));
        }
        quietFor = overDb >= -guardKneeDb ? 0 : std::min (quietFor + 1, pauseHoldSamples + pauseFadeSamples);
        const float target = guardFor (overDb, quietFor);
        makeupGuard += (target > makeupGuard ? cGuardRise : cGuardFall) * (target - makeupGuard);
        givenDb = steadyMakeup * makeupGuard;
        return givenDb;
    }

    // How much of the match is given: by how near the sound is to the level Punch works at, and none once the voice has been gone a while.
    float guardFor (float over, int quiet) const
    {
        const float present = quiet <= pauseHoldSamples ? 1.0f : 1.0f - (float) (quiet - pauseHoldSamples) / (float) pauseFadeSamples;
        return clamp01 ((over + guardKneeDb + guardSpanDb) / guardSpanDb) * present;
    }

    // What Punch's reduction leaves of a sound's power, at a level levelDb against the recent peak: 10 to the -reduction / 10.
    static float powerLeft (float levelDb, float punch)
    {
        if (punch <= 0.0f)
            return 1.0f;
        return std::exp (-2.0f * softKneeAbove (levelDb + depthDb (punch)) * (1.0f - 1.0f / ratio (punch)) * dbToLn);
    }

    // A level match (powers in and out, and the makeup that follows them) carried from one Punch to another: the power out is what the
    // same sound gives with the new reduction, and the makeup moves with its target. Left alone with nothing measured: then it goes to
    // its resting value at its own speed, as it always has.
    static void carry (const LevelHistogram& levels, float in, float& out, float& makeup, float from, float to)
    {
        if (! (in > gateFloorPower && out > 0.0f))
            return;
        const double before = levels.after ([from] (float db) { return powerLeft (db, from); });
        const double now = levels.after ([to] (float db) { return powerLeft (db, to); });
        if (! (before > 0.0 && now > 0.0))
            return;
        const auto target = [in] (float o) { return std::min (maxMakeupDb, std::max (0.0f, 10.0f * std::log10 (in / o))); };
        const float was = target (out);
        out = (float) ((double) out * (now / before));
        makeup = std::min (maxMakeupDb, std::max (0.0f, makeup + target (out) - was));
    }

    void carryMatchOver (float from, float to)
    {
        carry (plainLevels, inPower, outPower, makeupDb, from, to);
        if (! steadyWasOn)
            return;  // Steady Notes coming on takes over the plain match, already carried
        if (! matched)
        {
            carry (steadyLevels, steadyIn, steadyOut, steadyMakeup, from, to);
            return;
        }
        // Once right, the match is held to the voice: only sound at the level Punch works at has a say, as in the trim.
        const float voiceDb = std::max (-guardKneeDb - depthDb (from), -trimReachDb);
        double before = steadyLevels.after ([from] (float db) { return powerLeft (db, from); }, voiceDb);
        double now = steadyLevels.after ([to] (float db) { return powerLeft (db, to); }, voiceDb);
        if (! (before > 0.0 && now > 0.0))
        {
            before = steadyLevels.after ([from] (float db) { return powerLeft (db, from); });
            now = steadyLevels.after ([to] (float db) { return powerLeft (db, to); });
        }
        if (before > 0.0 && now > 0.0)
            steadyMakeup = std::min (maxMakeupDb, std::max (0.0f, steadyMakeup + (float) (10.0 * std::log10 (before / now))));
    }

    // Impact's held reduction, taken from one Impact (and a level shiftDb louder) to another. The level the reduction stands for is read
    // back through the old curve. At or above the level that built it, the reduction is following the sound, and the new curve at that
    // level is the answer. Under it, the reduction is letting go, by a fixed share each sample, so what is left stands to the new curve
    // at the level that built it as it stood to the old one there, and it is scaled by the two curves' ratio. The two meet where the
    // reduction equals the old curve at that level.
    float movedReduction (float r, float from, float to, float shiftDb) const
    {
        if (! (r > 0.0f) || from <= 0.0f)
            return 0.0f;
        const float y = r / (1.0f - 1.0f / impactRatio (from));
        const float over = y >= 0.5f * kneeDb ? y : std::sqrt (2.0f * kneeDb * y) - 0.5f * kneeDb;
        const float standsFor = over + impactThresholdDb (from);
        if (standsFor >= impactLevelDb)
            return impactReductionDb (standsFor + shiftDb, to);
        return r * impactReductionDb (impactLevelDb + shiftDb, to) / impactReductionDb (impactLevelDb, from);
    }

    void moveImpact (float from, float to, float shiftDb)
    {
        impactFast = movedReduction (impactFast, from, to, shiftDb);
        impactSlow = movedReduction (impactSlow, from, to, shiftDb);
        impactNowDb = std::max (impactFast, impactSlow);
        impactLevelDb += shiftDb;
    }

    static float softKneeAbove (float over)
    {
        if (over >= 0.5f * kneeDb)
            return over;
        if (over > -0.5f * kneeDb)
            return (over + 0.5f * kneeDb) * (over + 0.5f * kneeDb) / (2.0f * kneeDb);
        return 0.0f;
    }

    static float impactReductionDb (float levelDb, float impact)
    {
        if (impact <= 0.0f)
            return 0.0f;
        return softKneeAbove (levelDb - impactThresholdDb (impact)) * (1.0f - 1.0f / impactRatio (impact));
    }

    float compReductionDb (float punch) const
    {
        if (punch <= 0.0f)
            return 0.0f;
        const float threshold = std::max (toDb (peakRef), peakRefFloorDb) - depthDb (punch);
        const float over = toDb (compEnv) - threshold;
        return softKneeAbove (over) * (1.0f - 1.0f / ratio (punch));
    }

    float cFastRise = 1, cAttackSlowRise = 1, cAttackFall = 1, cSustainFastFall = 1, cSustainSlowFall = 1;
    float cCompRise = 1, cCompFall = 1, cPeakRefFall = 1, cPower = 1, cMakeup = 1, cGain = 1;
    float cImpactAttack = 1, cImpactFastRelease = 1, cImpactSlowRise = 1, cImpactSlowRelease = 1, cGuardRise = 1, cGuardFall = 1;

    float attackFast = 0, attackSlow = 0, sustainFast = 0, sustainSlow = 0, sustainSlowPlain = 0, compEnv = 0, peakRef = 0;
    float inPower = 0, outPower = 0, makeupDb = 0, gainDb = 0;
    float steadyIn = 0, steadyOut = 0, steadyMakeup = 0, handoverDb = 0, steadyLevelDb = 0, lastPunch = 0, rebasedPunch = 0, lastImpact = 0;
    float plainWeight = 1, plainStep = 1, impactLevelDb = -120.0f;
    float curvePunch = -1.0f, curveDepth = 0.0f, slopeNow = 0.0f;
    int sinceRebase = 0, noteAge = 0, plainQuietFor = 0;
    bool fresh = true;
    LevelHistogram plainLevels, steadyLevels;
    float impactFast = 0, impactSlow = 0, impactNowDb = 0, quietGuard = 0;

    float cRestart = 1, cBrightRise = 1, cBrightFall = 1, brightLow = 0.33f, brightHigh = 0.45f, cLevel = 1, cCrestHeld = 1, cCrestFree = 1;
    float stepEnv = 0, sizeEnv = 0, lastSample = 0, tailGate = 0, gateStep = 1, levelPower = 0, crestDb = 0;
    int notedFor = 0, noteSamples = 1, sinceNote = farFromANote, boostFull = 0, boostEnd = 1, cutFull = 0, cutEnd = 1;
    float noteLevel = 0, cNoteFall = 1, startDeadDb = 0.25f, startDeadUpStep = 0, startDeadDownStep = 0;
    int sunkFor = 0, graceSamples = 1, voiceGoneSamples = 1, voiceGoneRampSamples = 1;
    float overDb = 0, makeupGuard = 1, cMakeupHeld = 1, givenDb = 0;
    int quietFor = 0, pauseHoldSamples = 1, pauseFadeSamples = 1;
    bool steadyWasOn = false, noteOn = false, matched = false, levelsOnly = false;
    PitchSense pitch;

    SlidingMax shaperHold, compHold, noteRecent;
};
