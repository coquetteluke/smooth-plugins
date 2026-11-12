#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Largest value in the last `window` samples, O(1) on average, with no allocation.
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
    }

    float push (float x)
    {
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
        return value[(size_t) head];
    }

    std::vector<float> value;
    std::vector<long> index;
    int window = 1, size = 2, head = 0, tail = 0, count = 0;
    long now = 0;
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
        pitch.prepare (sampleRate);

        shaperHold.prepare (std::max (1, (int) std::lround (holdTime * sampleRate)));
        noteRecent.prepare (std::max (1, (int) std::lround (sagWindow * sampleRate)));
        compHold.prepare (std::max (1, (int) std::lround (compHoldTime * sampleRate)));
    }

    void reset (float punch)
    {
        shaperHold.reset();
        compHold.reset();
        attackFast = attackSlow = sustainFast = sustainSlow = 0.0f;
        compEnv = peakRef = 0.0f;
        inPower = outPower = 0.0f;
        gainDb = 0.0f;
        makeupDb = restingMakeupDb (punch);
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
        matchedPunch = punch;
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
        follow (compEnv, compPeak, cCompRise, cCompFall);
        follow (peakRef, compPeak, 1.0f, cPeakRefFall);

        // Start of a sound: the fast envelope is ahead of the slow one. Tail: the slow one is above the fast one.
        const float startDb = toDb (attackFast) - toDb (attackSlow);
        const float tailDb = toDb (sustainSlow) - toDb (sustainFast);
        float startAmount = clamp01 ((startDb - deadZoneDb) / startRangeDb);
        float tailAmount = clamp01 ((tailDb - deadZoneDb) / tailRangeDb);
        if (steadyNotes)
        {
            tailAmount = steadyTailAmount (startDb, sample, sustainDb, tailAmount);
            startAmount = steadyStartAmount (startDb);
        }

        const float reductionDb = steadyNotes ? steadyReductionDb (punch, x) : compReductionDb (punch);

        // Level match from average power with and without the reduction. Held when there is nothing to measure.
        const float reductionGain = std::exp (-reductionDb * dbToLn);
        const float inP = x * x;
        const float outP = inP * reductionGain * reductionGain;
        float makeupNowDb;
        if (steadyNotes)
        {
            makeupNowDb = steadyMakeupDb (punch, inP, outP);
        }
        else
        {
            inPower += cPower * (inP - inPower);
            outPower += cPower * (outP - outPower);
            float makeupTarget = restingMakeupDb (punch);
            if (inPower > gateFloorPower && outPower > 0.0f)
                makeupTarget = std::min (maxMakeupDb, std::max (0.0f, 10.0f * std::log10 (inPower / outPower)));
            makeupDb += cMakeup * (makeupTarget - makeupDb);
            makeupNowDb = makeupDb;
        }

        // Impact: two release speeds. The slow stage follows the level itself, not the fast stage's release, so it only builds up on a level that lasts and a short peak lets go quickly.
        const float heldDb = toDb (hold);
        const float impactTarget = impactReductionDb (heldDb, impact);
        impactFast += (impactTarget > impactFast ? cImpactAttack : cImpactFastRelease) * (impactTarget - impactFast);
        impactSlow += (impactTarget > impactSlow ? cImpactSlowRise : cImpactSlowRelease) * (impactTarget - impactSlow);
        impactNowDb = std::max (impactFast, impactSlow);
        // Impact's makeup fades out on near silence, so room noise between phrases isn't lifted with the voice.
        const float guardTarget = clamp01 ((heldDb - guardFloorDb) / guardRangeDb);
        quietGuard += (guardTarget > quietGuard ? cGuardRise : cGuardFall) * (guardTarget - quietGuard);

        const float total = attackDb * startAmount + sustainDb * tailAmount - reductionDb + makeupNowDb - impactNowDb + impactMakeupDb (impact) * quietGuard;
        steadyWasOn = steadyNotes;
        gainDb += cGain * (std::min (maxGainDb, std::max (minGainDb, total)) - gainDb);
        return gainDb;
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
        if (levelsOnly || sinceNote < voiceGoneSamples)
            return voiced;
        const float plain = sinceNote >= farFromANote ? 1.0f : std::min (1.0f, (float) (sinceNote - voiceGoneSamples) / (float) voiceGoneRampSamples);
        return voiced + plain * (plainAmount - voiced);
    }

    // Attack for a voice (Steady Notes). Inside a held vowel the peaks jump by a few dB now and then while the level stays where it is
    // (on Luke's takes by 1.6 to 4.8 dB), and Attack took each jump for a start. While a note is sounding a start has to rise by
    // heldStartDb before Attack acts. The first sound after a pause, and any sound that is not a note yet, is read as ever.
    float steadyStartAmount (float startDb)
    {
        const float target = noteOn ? heldStartDb : deadZoneDb;
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
    float steadyMakeupDb (float punch, float inP, float outP)
    {
        if (! steadyWasOn || punch != matchedPunch)
        {
            matched = false;
            matchedPunch = punch;
            if (! steadyWasOn)
            {
                makeupGuard = 1.0f;
                givenDb = 0.0f;
                quietFor = 0;
            }
        }
        if (! matched)
        {
            // Until the match is first right it is found the old way, quickly, from the level with and without the reduction.
            inPower += cPower * (inP - inPower);
            outPower += cPower * (outP - outPower);
            const bool measurable = inPower > gateFloorPower && outPower > 0.0f;
            const float target = measurable ? std::min (maxMakeupDb, std::max (0.0f, 10.0f * std::log10 (inPower / outPower))) : restingMakeupDb (punch);
            makeupDb += cMakeup * (target - makeupDb);
            if (noteOn && measurable && std::abs (target - makeupDb) < matchedWithinDb)
            {
                matched = true;
                outPower *= std::exp (2.0f * givenDb * dbToLn); // From here on the level after the match is what is measured.
            }
        }
        else if (overDb >= std::max (-guardKneeDb, depthDb (punch) - trimReachDb))
        {
            // Then it is trimmed, slowly, until what goes out is as loud as what came in. Only sound at the level Punch works at and
            // within trimReachDb of the loudest the voice has just been has a say: the voice itself, not what lies between the words.
            inPower += cPower * (inP - inPower);
            outPower += cPower * (outP * std::exp (2.0f * givenDb * dbToLn) - outPower);
            if (inPower > gateFloorPower && outPower > 0.0f)
                makeupDb = std::min (maxMakeupDb, std::max (0.0f, makeupDb + cMakeupHeld * 10.0f * std::log10 (inPower / outPower)));
        }
        quietFor = overDb >= -guardKneeDb ? 0 : std::min (quietFor + 1, pauseHoldSamples + pauseFadeSamples);
        const float present = quietFor <= pauseHoldSamples ? 1.0f : 1.0f - (float) (quietFor - pauseHoldSamples) / (float) pauseFadeSamples;
        const float target = clamp01 ((overDb + guardKneeDb + guardSpanDb) / guardSpanDb) * present;
        makeupGuard += (target > makeupGuard ? cGuardRise : cGuardFall) * (target - makeupGuard);
        givenDb = makeupDb * makeupGuard;
        return givenDb;
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

    float attackFast = 0, attackSlow = 0, sustainFast = 0, sustainSlow = 0, compEnv = 0, peakRef = 0;
    float inPower = 0, outPower = 0, makeupDb = 0, gainDb = 0;
    float impactFast = 0, impactSlow = 0, impactNowDb = 0, quietGuard = 0;

    float cRestart = 1, cBrightRise = 1, cBrightFall = 1, brightLow = 0.33f, brightHigh = 0.45f, cLevel = 1, cCrestHeld = 1, cCrestFree = 1;
    float stepEnv = 0, sizeEnv = 0, lastSample = 0, tailGate = 0, gateStep = 1, levelPower = 0, crestDb = 0;
    int notedFor = 0, noteSamples = 1, sinceNote = farFromANote, boostFull = 0, boostEnd = 1, cutFull = 0, cutEnd = 1;
    float noteLevel = 0, cNoteFall = 1, startDeadDb = 0.25f, startDeadUpStep = 0, startDeadDownStep = 0;
    int sunkFor = 0, graceSamples = 1, voiceGoneSamples = 1, voiceGoneRampSamples = 1;
    float overDb = 0, makeupGuard = 1, matchedPunch = 0, cMakeupHeld = 1, givenDb = 0;
    int quietFor = 0, pauseHoldSamples = 1, pauseFadeSamples = 1;
    bool steadyWasOn = false, noteOn = false, matched = false, levelsOnly = false;
    PitchSense pitch;

    SlidingMax shaperHold, compHold, noteRecent;
};
