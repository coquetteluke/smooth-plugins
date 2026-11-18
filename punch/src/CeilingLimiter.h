#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "PunchEngine.h"

// Output ceiling. Works inside the plug-in's existing delay, so it adds no latency of its own.
// process() gets the level of the sample entering the delay, which comes out lookAhead samples later, and returns the gain for the sample coming out now.
class CeilingLimiter
{
public:
    static constexpr double holdTime = 0.012;     // Longer than half a cycle down to about 42 Hz, so a steady tone gets a steady gain, not a distorting one.
    static constexpr double rampTime = 0.001;     // How long the gain takes to come down. Must not be longer than the look ahead.
    static constexpr double releaseTime = 0.080;

    void prepare (int lookAhead, double sampleRate)
    {
        hold.prepare (std::max (lookAhead + 1, (int) std::lround (holdTime * sampleRate)));
        rampLength = std::max (1, std::min (lookAhead, (int) std::lround (rampTime * sampleRate)));
        ramp.assign ((size_t) rampLength, 1.0f);
        release = (float) (1.0 - std::exp (-1.0 / (releaseTime * sampleRate)));
        reset();
    }

    void reset()
    {
        hold.reset();
        std::fill (ramp.begin(), ramp.end(), 1.0f);
        rampSum = (double) rampLength;
        rampPos = 0;
        envelope = 1.0f;
    }

    float process (float level, float ceiling)
    {
        const float needed = level > ceiling ? ceiling / level : 1.0f;
        const float lowest = 1.0f - hold.push (1.0f - needed); // Sliding minimum of the gain needed.
        envelope = lowest < envelope ? lowest : envelope + release * (lowest - envelope);

        rampSum += (double) envelope - (double) ramp[(size_t) rampPos];
        ramp[(size_t) rampPos] = envelope;
        if (++rampPos >= rampLength)
            rampPos = 0;
        return (float) (rampSum / rampLength);
    }

private:
    SlidingMax hold;
    std::vector<float> ramp;
    double rampSum = 1.0;
    int rampLength = 1, rampPos = 0;
    float release = 1.0f, envelope = 1.0f;
};
