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

        shaperHold.prepare (std::max (1, (int) std::lround (holdTime * sampleRate)));
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
    }

    // Attack and Sustain in dB, punch and impact from 0 to 1.
    float process (float rectified, float attackDb, float sustainDb, float punch, float impact)
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
        const float startAmount = clamp01 ((startDb - deadZoneDb) / startRangeDb);
        const float tailAmount = clamp01 ((tailDb - deadZoneDb) / tailRangeDb);

        const float reductionDb = compReductionDb (punch);

        // Level match from average power with and without the reduction. Held when there is nothing to measure.
        const float reductionGain = std::exp (-reductionDb * dbToLn);
        const float inP = x * x;
        const float outP = inP * reductionGain * reductionGain;
        inPower += cPower * (inP - inPower);
        outPower += cPower * (outP - outPower);
        float makeupTarget = restingMakeupDb (punch);
        if (inPower > gateFloorPower && outPower > 0.0f)
            makeupTarget = std::min (maxMakeupDb, std::max (0.0f, 10.0f * std::log10 (inPower / outPower)));
        makeupDb += cMakeup * (makeupTarget - makeupDb);

        // Impact: two release speeds. The slow stage follows the level itself, not the fast stage's release, so it only builds up on a level that lasts and a short peak lets go quickly.
        const float heldDb = toDb (hold);
        const float impactTarget = impactReductionDb (heldDb, impact);
        impactFast += (impactTarget > impactFast ? cImpactAttack : cImpactFastRelease) * (impactTarget - impactFast);
        impactSlow += (impactTarget > impactSlow ? cImpactSlowRise : cImpactSlowRelease) * (impactTarget - impactSlow);
        impactNowDb = std::max (impactFast, impactSlow);
        // Impact's makeup fades out on near silence, so room noise between phrases isn't lifted with the voice.
        const float guardTarget = clamp01 ((heldDb - guardFloorDb) / guardRangeDb);
        quietGuard += (guardTarget > quietGuard ? cGuardRise : cGuardFall) * (guardTarget - quietGuard);

        const float total = attackDb * startAmount + sustainDb * tailAmount - reductionDb + makeupDb - impactNowDb + impactMakeupDb (impact) * quietGuard;
        gainDb += cGain * (std::min (maxGainDb, std::max (minGainDb, total)) - gainDb);
        return gainDb;
    }

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

    SlidingMax shaperHold, compHold;
};
