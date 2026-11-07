#pragma once
// One complete tuning path: tracker, corrector, grain shifter and delay lines. The plug-in owns two of these. Normally only one runs.
// When Engine or Input Type changes, the other is set up and warmed while the first keeps playing, and then the two crossfade, so a
// change of latency never leaves a gap or a click. This knows nothing about the host.
//
// Low Latency: the shifter sits as close to the input as the longest period allows.
// High Quality: everything sits further behind the input, so the tracker and corrector are looking at the future of what is being played.
// That means a note is corrected from its first grain, and a consonant is known to be one before it reaches the output.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "PitchCorrector.h"
#include "PitchTracker.h"
#include "PsolaShifter.h"

namespace smoothtune
{
class TuneEngine
{
public:
    struct Shared   // everything that comes from the plug-in's controls, for one sample or block
    {
        PitchCorrector::Settings settings;
        HeldNotes held;
        bool formantPreserve = true;
        double throatRate = 1.0;     // reading speed: above 1 moves formants up
        float voicingLimit = 0.3f;
    };

    enum Mode { lowLatency = 0, highQuality = 1 };

    // Extra delay in High Quality, on top of what the Input Type needs.
    static constexpr double highQualityExtraSeconds = 0.045;
    // How far ahead High Quality looks for a change between voiced and not. The tracker takes this long to say so.
    static constexpr double highQualityLeadSeconds = 0.016;

    static int latencyFor (double lowestHz, int mode, double sampleRate) noexcept
    {
        const int base = PitchTracker::requiredLatency (sampleRate, lowestHz, PsolaShifter::interpolatorReach);
        return mode == highQuality ? base + (int) std::lround (highQualityExtraSeconds * sampleRate) : base;
    }

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        channels = std::clamp (numChannels, 1, 2);
        tracker.prepare (sr);
        shifter.prepare (sr, channels);
        corrector.prepare (sr);
        drySize = 1;
        while (drySize < (int) (sr * 0.25)) drySize *= 2;
        for (auto& d : dry) d.assign ((size_t) drySize, 0.0f);
        tickSamples = std::max (1, (int) std::lround (sr * 0.001));
        tickSeconds = tickSamples / sr;
    }

    // Starts this engine clean on a new range, delay and mode. No allocation, safe on the audio thread.
    void configure (int inputTypeIndex, double lowHz, double highHz, int modeIndex) noexcept
    {
        inputType = inputTypeIndex;
        mode = modeIndex;
        latency = latencyFor (lowHz, mode, sr);
        tracker.reset();
        tracker.setRange (lowHz, highHz);
        lastVoicingLimit = -1.0f;
        shifter.reset();
        shifter.setLatency (latency);
        corrector.reset();
        control = {};
        for (auto& d : dry) std::fill (d.begin(), d.end(), 0.0f);
        dryAt = 0;
        tickPhase = 0;
        ringAt = 0;
        ring.fill (ControlPoint {});
        const double extra = mode == highQuality ? highQualityExtraSeconds : 0.0;
        controlDelayTicks = std::clamp ((int) std::lround (extra / tickSeconds), 0, (int) ring.size() - 1);
        leadTicks = mode == highQuality ? std::min (controlDelayTicks, (int) std::lround (highQualityLeadSeconds / tickSeconds)) : 0;
    }

    int getInputType() const noexcept { return inputType; }
    int getMode() const noexcept { return mode; }
    int getLatency() const noexcept { return latency; }

    // One sample in, one sample out. wet is the tuned voice, dryOut is the input at the same delay.
    void process (float l, float r, const Shared& shared, float* wet, float* dryOut) noexcept
    {
        const int last = drySize - 1;
        dry[0][(size_t) (dryAt & last)] = l;
        dry[1][(size_t) (dryAt & last)] = r;
        dryOut[0] = dry[0][(size_t) ((dryAt - latency) & last)];
        dryOut[1] = dry[1][(size_t) ((dryAt - latency) & last)];
        ++dryAt;

        shifter.write (0, l);
        if (channels > 1) shifter.write (1, r);
        shifter.advance();

        if (shared.voicingLimit != lastVoicingLimit)
        {
            tracker.setVoicingLimit (shared.voicingLimit);
            lastVoicingLimit = shared.voicingLimit;
        }
        tracker.push (0.5f * (l + r));

        if (++tickPhase >= tickSamples)
        {
            tickPhase = 0;
            tick (shared);
        }

        wet[0] = wet[1] = 0.0f;
        shifter.step (tracker, control, wet);
    }

    // Reading the control path, which is what the window and the readings show.
    const PitchCorrector& getCorrector() const noexcept { return corrector; }
    const PsolaShifter::Control& getControl() const noexcept { return control; }

private:
    struct ControlPoint { double period = 0.0; HeldNotes held; };

    void tick (const Shared& shared) noexcept
    {
        ring[(size_t) (ringAt % (int) ring.size())] = { tracker.period(), shared.held };
        const int size = (int) ring.size();
        const auto& point = ring[(size_t) ((ringAt + size - controlDelayTicks) % size)];

        // The pitch stays in line with the audio. Only the decision voiced or not comes from further ahead, so a note is treated
        // as a note from its start and a consonant as a consonant from its start.
        double period = point.period;
        if (leadTicks > 0)
        {
            const double ahead = ring[(size_t) ((ringAt + size - controlDelayTicks + leadTicks) % size)].period;
            if (period > 0.0 && ahead <= 0.0)
                period = 0.0;
            else if (period <= 0.0 && ahead > 0.0)
                period = ahead;
        }
        ringAt = (ringAt + 1) % size;

        corrector.update (period, shared.settings, point.held, tickSeconds);
        control.voiced = period > 0.0;
        if (control.voiced)
        {
            control.inputPeriod = period;
            control.outputPeriod = corrector.outputPeriod (shared.settings);
            control.noCorrection = corrector.isIdentity();
            double rate = shared.throatRate;
            if (! shared.formantPreserve && control.outputPeriod > 0.0)
                rate *= period / control.outputPeriod;
            control.grainRate = std::clamp (rate, 0.5, 2.0);
        }
        else
        {
            control.grainRate = 1.0;
            control.noCorrection = true;
        }
    }

    double sr = 48000.0, tickSeconds = 0.001;
    int channels = 2, tickSamples = 48, tickPhase = 0, latency = 0, inputType = 1, mode = lowLatency;
    PitchTracker tracker;
    PsolaShifter shifter;
    PitchCorrector corrector;
    PsolaShifter::Control control;
    std::array<std::vector<float>, 2> dry;
    int drySize = 16384;
    long long dryAt = 0;
    std::array<ControlPoint, 128> ring {};
    int ringAt = 0, controlDelayTicks = 0, leadTicks = 0;
    float lastVoicingLimit = -1.0f;
};
} // namespace smoothtune
