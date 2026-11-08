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
#include "EnvelopeFilter.h"
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
    static constexpr double highQualityLeadSeconds = 0.010;
    // How much input each model of the vocal tract sees.
    static constexpr double levelMemorySeconds = 0.6;
    static constexpr double highQualityWindowSeconds = 0.040, lowLatencyWindowSeconds = 0.040, lowLatencyFallShare = 0.3;

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
        envelope.prepare (sr, channels);
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
        if (mode == highQuality)
            envelope.configure (highQualityWindowSeconds, 0.5 * highQualityWindowSeconds, 0.5);
        else
            envelope.configure (lowLatencyWindowSeconds, 0.0, lowLatencyFallShare);
        // Without lookahead the direct fit can lock on to a harmonic sitting on the first formant, and its level then swings when the harmonic moves away.
        envelope.setHarmonicModelFrom (mode == highQuality ? 300.0 : 130.0);
        // High Quality remembers the harmonics as the pitch moves, which shows the formant peaks far better, so it needs much less sharpening.
        envelope.setSharpen (mode == highQuality ? 0.1 : 0.35);
        envelope.setSweepMemory (mode == highQuality ? 0.2 : 0.0, 350.0);
        ahead = envelope.getLookahead();
        levelGain = 1.0;
        passMix = 1.0;
        passStep = 1.0 / (0.003 * sr);
        wetLevel = dryLevel = levelSeen = 0.0;
        correctingFor = 0;
        levelMemory = levelMemorySeconds * sr;
        levelFollow = 1.0 - std::exp (-1.0 / (0.03 * sr));
        levelRelease = 1.0 - std::exp (-1.0 / (0.003 * sr));
        shifter.reset();
        shifter.setLatency (latency - ahead);
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

        const float mono = 0.5f * (l + r);
        envelope.setPeriod (tracker.period());
        envelope.push (mono);

        if (shared.voicingLimit != lastVoicingLimit)
        {
            tracker.setVoicingLimit (shared.voicingLimit);
            lastVoicingLimit = shared.voicingLimit;
        }
        tracker.push (mono);

        if (++tickPhase >= tickSamples)
        {
            tickPhase = 0;
            tick (shared);
        }

        wet[0] = wet[1] = 0.0f;
        // What goes into the grains is the whitened voice, as far behind the newest sample as the model needs to see ahead.
        const long long index = dryAt - ahead;
        if (index >= 0)
        {
            envelope.loadAnalysisFor (index);
            shifter.write (0, (float) envelope.whiten (0, (double) dry[0][(size_t) (index & last)]));
            if (channels > 1) shifter.write (1, (float) envelope.whiten (1, (double) dry[1][(size_t) (index & last)]));
            shifter.advance();
            shifter.step (tracker, control, wet);

            const long long at = dryAt - latency;
            if (at >= 0)
            {
                envelope.loadSynthesisFor (at);
                wet[0] = (float) envelope.shape (0, (double) wet[0]);
                if (channels > 1) wet[1] = (float) envelope.shape (1, (double) wet[1]);
                holdLevel (wet, dryOut);

                // Through a consonant the filters ring on for a few ms from the vowel before it. Nothing is being changed then, so the true input takes over.
                const bool pass = ! control.voiced && ! control.plainGrains;
                passMix = pass ? std::min (1.0, passMix + passStep) : std::max (0.0, passMix - passStep);
                if (passMix > 0.0)
                {
                    wet[0] = (float) (wet[0] + passMix * (dryOut[0] - wet[0]));
                    wet[1] = (float) (wet[1] + passMix * (dryOut[1] - wet[1]));
                }
            }
        }
        ++dryAt;
    }

    // Reading the control path, which is what the window and the readings show.
    const PitchCorrector& getCorrector() const noexcept { return corrector; }
    const PsolaShifter::Control& getControl() const noexcept { return control; }

private:
    struct ControlPoint { double period = 0.0; HeldNotes held; };

    // Moving the harmonics changes how many land on a formant, which changes the level on average. This trims that average, over about a second of
    // corrected voice, so a shift does not make a phrase louder or softer. It is too slow to touch the voice's own dynamics. With nothing being
    // corrected the gain goes straight back to exactly 1, so consonants and uncorrected notes are untouched.
    void holdLevel (float* wet, const float* dryNow) noexcept
    {
        if (control.plainGrains)
        {
            levelGain = 1.0;
            return;
        }
        const bool correcting = control.voiced && ! control.noCorrection;
        double wanted = 1.0;
        correctingFor = correcting ? correctingFor + 1 : 0;
        if (correcting)
        {
            // The start of a corrected stretch is still settling and says nothing about the average.
            if (correctingFor > (int) (0.08 * sr))
            {
            const double w = (double) wet[0] * wet[0] + (channels > 1 ? (double) wet[1] * wet[1] : 0.0);
            const double d = (double) dryNow[0] * dryNow[0] + (channels > 1 ? (double) dryNow[1] * dryNow[1] : 0.0);
            levelSeen = std::min (levelSeen + 1.0, levelMemory);
            wetLevel += (w - wetLevel) / levelSeen;
            dryLevel += (d - dryLevel) / levelSeen;
            }
            if (wetLevel > 1.0e-12 && dryLevel > 1.0e-12 && levelSeen > 0.05 * sr)
                wanted = std::clamp (std::sqrt (dryLevel / wetLevel), 0.25, 4.0);
        }
        levelGain += (correcting ? levelFollow : levelRelease) * (wanted - levelGain);
        if (! correcting && std::abs (levelGain - 1.0) < 1.0e-5)
            levelGain = 1.0;
        wet[0] = (float) (wet[0] * levelGain);
        wet[1] = (float) (wet[1] * levelGain);
    }

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
            // Formants move in the model, and in the grains as well: what the model did not catch stays in the grains, and it must move with the rest.
            double scale = shared.throatRate;
            if (! shared.formantPreserve && control.outputPeriod > 0.0)
                scale *= period / control.outputPeriod;
            envelope.setFormantScale (scale);
            envelope.setShift (control.outputPeriod > 0.0 ? period / control.outputPeriod : 1.0);
            control.grainRate = std::clamp (scale, 0.5, 2.0);
        }
        else
        {
            control.grainRate = 1.0;
            control.noCorrection = true;
        }
        control.plainGrains = shared.settings.classic;
        control.centreOnPulses = true;
        // Low Latency's model lags the voice, which matters most on low voices, whose periods are long against it. It leaves those alone.
        control.pulseLowestHz = mode == highQuality ? 0.0 : 130.0;
        control.downReadPower = mode == highQuality ? 0.0 : 0.2;
        envelope.setStrength (shared.settings.classic ? 0.0 : 1.0);
    }

    double sr = 48000.0, tickSeconds = 0.001;
    int channels = 2, tickSamples = 48, tickPhase = 0, latency = 0, inputType = 1, mode = lowLatency;
    PitchTracker tracker;
    PsolaShifter shifter;
    EnvelopeFilter envelope;
    PitchCorrector corrector;
    PsolaShifter::Control control;
    std::array<std::vector<float>, 2> dry;
    int drySize = 16384;
    long long dryAt = 0;
    std::array<ControlPoint, 128> ring {};
    int ringAt = 0, controlDelayTicks = 0, leadTicks = 0, ahead = 0;
    float lastVoicingLimit = -1.0f;
    int correctingFor = 0;
    double passMix = 1.0, passStep = 0.01;
    double levelGain = 1.0, wetLevel = 0.0, dryLevel = 0.0, levelSeen = 0.0, levelMemory = 48000.0, levelFollow = 0.001, levelRelease = 0.01;
};
} // namespace smoothtune
