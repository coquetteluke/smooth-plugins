#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// The detector. Takes one rectified sidechain sample and returns the gain to apply, in dB, to the delayed audio.
// Attack and Sustain only ever compare envelopes with each other as a ratio, and every envelope here is a
// linear follower, so scaling the input scales every envelope by the same amount and the ratios do not move.
// That is what makes the shaper independent of input level. Punch sets its threshold from the recent peak level
// for the same reason.
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
    static constexpr double compRise = 0.0005;
    static constexpr double compFall = 0.040;
    static constexpr double peakRefFall = 1.5;     // How long the compressor remembers the loudest recent level.
    static constexpr double powerTime = 0.300;     // Averaging for the level match.
    static constexpr double makeupTime = 0.200;
    static constexpr double gainTime = 0.0007;     // Final gain smoothing.

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

        window = std::max (1, (int) std::lround (holdTime * sampleRate));
        queueSize = window + 1;
        queueValue.assign ((size_t) queueSize, 0.0f);
        queueIndex.assign ((size_t) queueSize, 0L);
    }

    void reset (float punch)
    {
        std::fill (queueValue.begin(), queueValue.end(), 0.0f);
        std::fill (queueIndex.begin(), queueIndex.end(), 0L);
        head = tail = count = 0;
        now = 0;
        attackFast = attackSlow = sustainFast = sustainSlow = 0.0f;
        compEnv = peakRef = 0.0f;
        inPower = outPower = 0.0f;
        gainDb = 0.0f;
        makeupDb = restingMakeupDb (punch);
    }

    // Attack and Sustain in dB, punch from 0 to 1.
    float process (float rectified, float attackDb, float sustainDb, float punch)
    {
        const float x = rectified < 1.0e6f ? rectified : 0.0f; // Also catches NaN, so bad input can't poison the envelopes.
        const float hold = slidingMax (x);

        follow (attackFast, hold, cFastRise, cAttackFall);
        follow (attackSlow, hold, cAttackSlowRise, cAttackFall);
        follow (sustainFast, hold, cFastRise, cSustainFastFall);
        follow (sustainSlow, hold, cFastRise, cSustainSlowFall);
        follow (compEnv, hold, cCompRise, cCompFall);
        follow (peakRef, hold, 1.0f, cPeakRefFall);

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

        const float total = attackDb * startAmount + sustainDb * tailAmount - reductionDb + makeupDb;
        gainDb += cGain * (std::min (maxGainDb, std::max (minGainDb, total)) - gainDb);
        return gainDb;
    }

    static float dbToGain (float db) { return std::exp (db * dbToLn); }

    // What a level at the recent peak gets reduced by. Depth is how far below the recent peak the threshold sits.
    static float depthDb (float punch) { return 24.0f * punch; }
    static float ratio (float punch) { return 1.0f + 5.0f * punch; }

private:
    static constexpr float dbToLn = 0.11512925464970229f;
    static constexpr float epsilon = 1.0e-6f;         // -120 dBFS.
    static constexpr float deadZoneDb = 0.25f;        // So tiny peak differences on a steady tone do nothing at all.
    static constexpr float startRangeDb = 6.0f;
    static constexpr float tailRangeDb = 2.5f;
    static constexpr float kneeDb = 6.0f;
    static constexpr float peakRefFloorDb = -60.0f;   // Below this Punch stops chasing the level, so noise is left alone.
    static constexpr float gateFloorPower = 1.0e-7f;  // -70 dBFS.
    static constexpr float maxMakeupDb = 30.0f;
    static constexpr float maxGainDb = 40.0f;
    static constexpr float minGainDb = -60.0f;

    static float toDb (float x) { return 8.685889638f * std::log (x + epsilon); }
    static float clamp01 (float x) { return x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : x); }
    static void follow (float& env, float target, float rise, float fall) { env += (target > env ? rise : fall) * (target - env); }

    static float restingMakeupDb (float punch)
    {
        const float r = ratio (punch);
        return 0.5f * depthDb (punch) * (1.0f - 1.0f / r);
    }

    float compReductionDb (float punch) const
    {
        if (punch <= 0.0f)
            return 0.0f;
        const float threshold = std::max (toDb (peakRef), peakRefFloorDb) - depthDb (punch);
        const float over = toDb (compEnv) - threshold;
        float above = 0.0f;
        if (over >= 0.5f * kneeDb)
            above = over;
        else if (over > -0.5f * kneeDb)
            above = (over + 0.5f * kneeDb) * (over + 0.5f * kneeDb) / (2.0f * kneeDb);
        return above * (1.0f - 1.0f / ratio (punch));
    }

    // Largest value in the last `window` samples, O(1) on average, with no allocation.
    float slidingMax (float x)
    {
        while (count > 0)
        {
            const int back = (tail + queueSize - 1) % queueSize;
            if (queueValue[(size_t) back] > x)
                break;
            tail = back;
            --count;
        }
        queueValue[(size_t) tail] = x;
        queueIndex[(size_t) tail] = now;
        tail = (tail + 1) % queueSize;
        ++count;
        while (queueIndex[(size_t) head] <= now - window)
        {
            head = (head + 1) % queueSize;
            --count;
        }
        ++now;
        return queueValue[(size_t) head];
    }

    float cFastRise = 1, cAttackSlowRise = 1, cAttackFall = 1, cSustainFastFall = 1, cSustainSlowFall = 1;
    float cCompRise = 1, cCompFall = 1, cPeakRefFall = 1, cPower = 1, cMakeup = 1, cGain = 1;

    float attackFast = 0, attackSlow = 0, sustainFast = 0, sustainSlow = 0, compEnv = 0, peakRef = 0;
    float inPower = 0, outPower = 0, makeupDb = 0, gainDb = 0;

    std::vector<float> queueValue;
    std::vector<long> queueIndex;
    int window = 1, queueSize = 2, head = 0, tail = 0, count = 0;
    long now = 0;
};
