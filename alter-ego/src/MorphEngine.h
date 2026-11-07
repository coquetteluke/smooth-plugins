#pragma once
// The voice changer, one sample at a time:
//
//   input > whitening filter > grain shifter > breath > putting-back filter > level match > mix
//
// The tracker hears the input as it arrives. The whitening filter needs half a window of look ahead, so it runs behind that, which
// also gives the tracker a head start: the marks grains need are already known, and the total delay stays near 25 ms.
//
// Everything is allocated in prepare(). process() is real time safe. Knows nothing about JUCE or the host.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Characters.h"
#include "EnvelopeEngine.h"
#include "PitchTracker.h"
#include "PsolaShifter.h"

namespace smoothmorph
{
class MorphEngine
{
public:
    static constexpr double lowestHz = 65.0, highestHz = 1300.0;

    struct Settings
    {
        double pitchSemitones = 0.0;
        double formantSemitones = 0.0;
        int character = 0;
        double characterAmount = 1.0;
        double robotHz = 110.0;
        double vibratoHz = 5.5;
        double vibratoCents = 0.0;
        double whisper = 0.0;       // 0 to 1
        double mix = 1.0;           // 0 to 1
        double outputGain = 1.0;
        bool rangeMatch = false;
        double rangeCentreHz = 220.0;
        bool bypass = false;
    };

    // Total delay at a sample rate, from input to output, including the dry path. Used before prepare() as well.
    static int latencyFor (double sampleRate) noexcept
    {
        int logN = 10;
        while ((1 << logN) < (int) std::lround (sampleRate * 0.0213)) ++logN;
        const int window = 1 << logN;
        return window / 2 + window / 8 + shifterDelayFor (sampleRate);
    }

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;
        channels = std::clamp (numChannels, 1, 2);
        envelope.prepare (sr);
        order = envelope.getOrder();
        tracker.prepare (sr);
        tracker.setRange (lowestHz, highestHz);
        shifter.prepare (sr, channels);
        shifterDelay = shifterDelayFor (sr);
        shifter.setLatency (shifterDelay);
        envelope.setSynthesisDelay (shifterDelay);
        whitenDelay = envelope.lookahead();
        total = whitenDelay + shifterDelay;

        delaySize = 1;
        while (delaySize < total + 64) delaySize *= 2;
        for (auto& d : dryLine) d.assign ((size_t) delaySize, 0.0f);

        tickEvery = std::max (1, (int) std::lround (sr / 1500.0));
        const double tickRate = sr / tickEvery;
        const auto smooth = [tickRate] (double seconds) { return 1.0 - std::exp (-1.0 / (seconds * tickRate)); };
        pitchSmooth = smooth (0.030);
        formantSmooth = smooth (0.030);
        blendSmooth = smooth (0.040);
        fadeSmooth = smooth (0.015);
        rangeSmooth = smooth (0.150);
        learnRate = smooth (1.5);
        breathRate = 1.0 - std::exp (-1.0 / (0.006 * sr));
        levelRate = 1.0 - std::exp (-1.0 / (0.025 * sr));
        levelGainRate = 1.0 - std::exp (-1.0 / (0.015 * sr));
        levelRelease = 1.0 - std::exp (-1.0 / (0.003 * sr));
        tilt = std::exp (-2.0 * M_PI * 200.0 / sr);
        historySize = 1;
        while (historySize < total / tickEvery + 4) historySize *= 2;
        history.assign ((size_t) historySize, 0);
        periodHistory.assign ((size_t) historySize, 0.0);
        spacingSmooth = smooth (0.002);
        reset();
    }

    void reset() noexcept
    {
        envelope.reset();
        tracker.reset();
        tracker.setRange (lowestHz, highestHz);
        shifter.reset();
        for (auto& d : dryLine) std::fill (d.begin(), d.end(), 0.0f);
        clearFilters();
        input = 0;
        tickPhase = 0;
        lfoPhase = 0.0;
        lastPeriod = 0.0;
        readingOutHz = 0.0;
        wantedOutputPeriod = 0.0;
        pulseNext = 0.0;
        pulsePeriod = 100.0;
        pulseGate = 0.0;
        pulseRing.fill (0.0);
        pulseClock = 0;
        std::fill (history.begin(), history.end(), 0);
        std::fill (periodHistory.begin(), periodHistory.end(), 0.0);
        smoothedPeriod = 0.0;
        historyAt = 0;
        learned = 0.0;
        learnedFrames = 0;
        rangeHeld = 0.0;
        rangeNow = 0.0;
        current = {};
        started = false;
        formantRatio = sharpen = 1.0;
        control = {};
        randomState = 0x9E3779B97F4A7C15ull;
        jitterHeld = 0.0;
        breathPower.fill (0.0);
        levelIn = levelOut = 0.0;
        levelGain = 1.0;
        readDetected.store (0.0f);
        readOutput.store (0.0f);
        readRange.store (0.0f);
    }

    void set (const Settings& s) noexcept { wanted = s; }

    int getLatency() const noexcept { return total; }
    float detectedHz() const noexcept { return readDetected.load (std::memory_order_relaxed); }
    float outputHz() const noexcept { return readOutput.load (std::memory_order_relaxed); }
    float rangeOffset() const noexcept { return readRange.load (std::memory_order_relaxed); }
    const EnvelopeEngine& envelopeReadout() const noexcept { return envelope; }

    // Runs in place. Input is cleaned here: not finite goes to 0, the rest is clamped to +/-16.
    void process (float* const* audio, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float x[2] = { clean (audio[0][i]), channels > 1 ? clean (audio[1][i]) : 0.0f };
            const float mono = channels > 1 ? 0.5f * (x[0] + x[1]) : x[0];

            if (--tickPhase <= 0)
            {
                tickPhase = tickEvery;
                tick();
            }

            tracker.push (mono);
            // Pre-emphasis takes the source's tilt out before the fit, so formants high up are fitted as well as low ones,
            // and they alone move when formants do. The de-emphasis after the filter puts the tilt back.
            const float tilted = mono - (float) tilt * lastMono;
            lastMono = mono;
            envelope.push (tilted, formantRatio, control.voiced ? sharpen : 1.0, control.voiced && control.inputPeriod > 0.0 ? sr / control.inputPeriod : 0.0, ! control.noCorrection);

            // Input and dry paths
            const int writeAt = (int) (input & (long long) (delaySize - 1));
            for (int ch = 0; ch < channels; ++ch)
                dryLine[(size_t) ch][(size_t) writeAt] = x[ch];

            // Whiten the sample that the look ahead has now reached
            const long long whitenIndex = input - whitenDelay;
            if (whitenIndex >= 0)
            {
                if ((whitenIndex & (EnvelopeEngine::updateEvery - 1)) == 0)
                    envelope.analysisAt (whitenIndex, analysisK.data());
                const int readAt = (int) (whitenIndex & (long long) (delaySize - 1));
                for (int ch = 0; ch < channels; ++ch)
                {
                    const double raw = (double) dryLine[(size_t) ch][(size_t) readAt];
                    const double e = latticeAnalyse (raw - tilt * tiltIn[(size_t) ch], analysisK.data(), analysisState[(size_t) ch].data(), order);
                    tiltIn[(size_t) ch] = raw;
                    shifter.write (ch, (float) e);
                }
                shifter.advance();
            }

            // Shift, breathe, put the vocal tract back
            float excitation[2] = { 0.0f, 0.0f };
            if (whitenIndex >= 0)
                shifter.step (tracker, control, excitation);

            const long long outputIndex = whitenIndex - shifterDelay;
            if (outputIndex >= 0 && (outputIndex & (EnvelopeEngine::updateEvery - 1)) == 0)
                envelope.synthesisAt (outputIndex, synthesisK.data(), synthesisGain);

            const double pulse = current.robot > 0.0 ? nextPulse() : 0.0;
            const double noise = current.whisper > 0.0 ? nextGaussian() : 0.0;
            const double keep = std::cos (0.5 * M_PI * current.whisper), breath = std::sin (0.5 * M_PI * current.whisper);

            const int dryAt = (int) ((input - total) & (long long) (delaySize - 1));
            double wet[2] = { 0.0, 0.0 };
            double powerIn = 0.0, powerOut = 0.0;
            const bool hasDry = input >= total;
            float dry[2] = { 0.0f, 0.0f };
            for (int ch = 0; ch < channels; ++ch)
            {
                dry[ch] = hasDry ? dryLine[(size_t) ch][(size_t) dryAt] : 0.0f;
                double e = (double) excitation[ch];
                breathPower[(size_t) ch] += breathRate * (e * e - breathPower[(size_t) ch]);
                if (current.robot > 0.0)
                {
                    // Robot's buzz: the voiced part of the sound comes mostly from a pure pulse train at the set note.
                    const double share = 0.85 * current.robot * current.robot * pulseGate;
                    e = (1.0 - share) * e + share * pulse * std::sqrt (std::max (breathPower[(size_t) ch], 0.0) * pulsePeriod);
                }
                if (current.whisper > 0.0)
                    e = keep * e + breath * noise * std::sqrt (std::max (breathPower[(size_t) ch], 0.0));
                double y = latticeSynth (synthesisGain * e, synthesisK.data(), synthesisState[(size_t) ch].data(), order) + tilt * tiltOut[(size_t) ch];
                tiltOut[(size_t) ch] = y;
                if (! std::isfinite (y) || std::abs (y) > 64.0)
                {
                    clearFilters();
                    y = 0.0;
                }
                wet[ch] = y;
                powerIn += (double) dry[ch] * dry[ch];
                powerOut += y * y;
            }

            // Level match, shared by both sides so the stereo image stays
            levelIn += levelRate * (powerIn - levelIn);
            levelOut += levelRate * (powerOut - levelOut);
            if (control.voiced)
            {
                const double target = std::clamp (std::sqrt ((levelIn + 1.0e-10) / (levelOut + 1.0e-10)), 0.125, 8.0);
                levelGain += levelGainRate * (target - levelGain);
            }
            else
            {
                // Consonants pass unchanged, so nothing to match, and a gain left over from the vowel must not colour them.
                levelOut = levelIn;
                levelGain += levelRelease * (1.0 - levelGain);
            }

            const double wetAmount = current.mix * current.engaged;
            for (int ch = 0; ch < channels; ++ch)
            {
                const double out = ((1.0 - wetAmount) * (double) dry[ch] + wetAmount * levelGain * wet[ch]) * (1.0 + current.engaged * (current.outputGain - 1.0));
                audio[ch][i] = (float) std::clamp (out, -16.0, 16.0);
            }
            ++input;
        }
        readDetected.store ((float) (control.voiced && control.inputPeriod > 0.0 ? sr / control.inputPeriod : 0.0), std::memory_order_relaxed);
        readOutput.store ((float) (control.voiced ? readingOutHz : 0.0), std::memory_order_relaxed);
        readRange.store ((float) rangeHeld, std::memory_order_relaxed);
    }

private:
    struct Smoothed
    {
        double pitch = 0.0, formant = 0.0, robot = 0.0, monster = 0.0, whisper = 0.0, mix = 1.0, outputGain = 1.0, engaged = 1.0, vibrato = 0.0, userPitch = 0.0;
    };

    // How far grains may read ahead. A mark can sit 0.6 periods from where a grain lands, at the lowest pitch tracked.
    static int shifterDelayFor (double sampleRate) noexcept
    {
        return (int) std::ceil (0.7 * sampleRate / lowestHz) + PsolaShifter::interpolatorReach + 2;
    }

    static float clean (float v) noexcept { return std::isfinite (v) ? std::clamp (v, -16.0f, 16.0f) : 0.0f; }

    void clearFilters() noexcept
    {
        for (auto& s : analysisState) s.fill (0.0);
        for (auto& s : synthesisState) s.fill (0.0);
        analysisK.fill (0.0);
        synthesisK.fill (0.0);
        synthesisGain = 1.0;
        tiltIn.fill (0.0);
        tiltOut.fill (0.0);
        lastMono = 0.0f;
    }

    // One sample of a band limited pulse train at the wanted output period. Each pulse has energy 1.
    double nextPulse() noexcept
    {
        const long long now = pulseClock;
        ++pulseClock;
        pulsePeriod = std::max (control.inputPeriod > 0.0 ? wantedOutputPeriod : control.robotPeriod, 8.0);
        if (pulseNext < (double) now + pulseTaps)
        {
            if (pulseNext < (double) now - pulseTaps)
                pulseNext = (double) now + pulseTaps; // first pulse, or starting over
            const long long whole = (long long) std::floor (pulseNext);
            const double fraction = pulseNext - (double) whole;
            for (int t = -pulseTaps + 1; t <= pulseTaps; ++t)
            {
                const double x = (double) t - fraction;
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (M_PI * x) / (M_PI * x);
                const double hann = 0.5 + 0.5 * std::cos (M_PI * x / (pulseTaps + 1));
                const long long at = whole + t;
                if (at >= now)
                    pulseRing[(size_t) (at & (pulseRingSize - 1))] += sinc * hann;
            }
            pulseNext += pulsePeriod;
        }
        double& slot = pulseRing[(size_t) (now & (pulseRingSize - 1))];
        const double out = slot;
        slot = 0.0;
        return out;
    }

    double nextUniform() noexcept
    {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 7;
        randomState ^= randomState << 17;
        return (double) (randomState >> 11) * (1.0 / 9007199254740992.0);
    }

    double nextGaussian() noexcept
    {
        double s = 0.0;
        for (int i = 0; i < 6; ++i)
            s += nextUniform();
        return (s - 3.0) * std::sqrt (2.0);
    }

    // About every 0.67 ms: everything that moves slowly.
    void tick() noexcept
    {
        const Settings& s = wanted;
        const auto& c = characters[std::clamp (s.character, 0, numCharacters - 1)];
        const double amount = std::clamp (s.characterAmount, 0.0, 1.0);

        const bool voicedNow = tracker.isVoiced();
        const double period = tracker.period();
        if (voicedNow && period > 0.0)
            lastPeriod = period;

        // Range Match: learn where this voice sits, then move it there in whole semitones
        if (voicedNow && period > 0.0)
        {
            const double l = std::log2 (sr / period);
            learned = learnedFrames < 300 ? (learned * learnedFrames + l) / (learnedFrames + 1) : learned + learnRate * (l - learned);
            if (learnedFrames < 300) ++learnedFrames;
        }
        double rangeWanted = 0.0;
        if (s.rangeMatch && learnedFrames >= 150 && c.robot <= 0.0)
        {
            const double ideal = 12.0 * (std::log2 (std::max (s.rangeCentreHz, 20.0)) - learned);
            if (std::abs (ideal - rangeHeld) > 0.75)
                rangeHeld = std::clamp (std::round (ideal), -24.0, 24.0);
            rangeWanted = rangeHeld;
        }
        else
            rangeHeld = 0.0;
        rangeNow += rangeSmooth * (rangeWanted - rangeNow);

        const double wantPitch = s.pitchSemitones + amount * c.pitchSemitones;
        const double wantFormant = s.formantSemitones + amount * c.formantSemitones;
        if (! started)
        {
            // Nothing has played yet, so the settings can apply at once.
            current.pitch = wantPitch;
            current.formant = wantFormant;
            current.robot = amount * c.robot;
            current.monster = amount * c.monster;
            current.whisper = s.whisper;
            current.mix = s.mix;
            current.outputGain = s.outputGain;
            current.engaged = s.bypass ? 0.0 : 1.0;
            current.vibrato = s.vibratoCents;
            current.userPitch = s.pitchSemitones;
            started = true;
        }
        current.pitch += pitchSmooth * (wantPitch - current.pitch);
        current.formant += formantSmooth * (wantFormant - current.formant);
        current.robot += blendSmooth * (amount * c.robot - current.robot);
        current.monster += blendSmooth * (amount * c.monster - current.monster);
        current.whisper += fadeSmooth * (s.whisper - current.whisper);
        current.mix += fadeSmooth * (s.mix - current.mix);
        current.outputGain += fadeSmooth * (s.outputGain - current.outputGain);
        current.engaged += fadeSmooth * ((s.bypass ? 0.0 : 1.0) - current.engaged);
        current.vibrato += blendSmooth * (s.vibratoCents - current.vibrato);
        current.userPitch += pitchSmooth * (s.pitchSemitones - current.userPitch);
        // Land exactly, so a voice with everything back at 0 passes straight through.
        if (std::abs (current.pitch - wantPitch) < 1.0e-3) current.pitch = wantPitch;
        if (std::abs (current.formant - wantFormant) < 1.0e-3) current.formant = wantFormant;
        if (std::abs (current.robot - amount * c.robot) < 1.0e-3) current.robot = amount * c.robot;
        if (std::abs (current.monster - amount * c.monster) < 1.0e-3) current.monster = amount * c.monster;
        if (std::abs (current.whisper - s.whisper) < 1.0e-3) current.whisper = s.whisper;
        if (std::abs (current.vibrato - s.vibratoCents) < 1.0e-2) current.vibrato = s.vibratoCents;
        if (std::abs (rangeNow - rangeWanted) < 1.0e-3) rangeNow = rangeWanted;

        lfoPhase += std::clamp (s.vibratoHz, 0.01, 40.0) * tickEvery / sr;
        if (lfoPhase >= 1.0) lfoPhase -= 1.0;
        const double vibrato = current.vibrato * std::sin (2.0 * M_PI * lfoPhase);

        // A sound that stops being voiced at the input is still voiced at the output for as long as the delay.
        history[(size_t) (historyAt & (historySize - 1))] = voicedNow ? 1 : 0;
        ++historyAt;
        const int back = std::min (total / tickEvery, historySize - 1);
        const bool voicedThen = history[(size_t) ((historyAt - 1 - back) & (historySize - 1))] != 0;
        const bool voicedOut = voicedNow || voicedThen;

        control.voiced = voicedOut && lastPeriod > 0.0;

        // The period for the next grain to land. The output is a whole delay behind the tracker, and the spacing chosen now only
        // shows one to two periods from now, so read it back from a little history. Best of the offsets tried on real speech.
        periodHistory[(size_t) ((historyAt - 1) & (historySize - 1))] = voicedNow && period > 0.0 ? period : 0.0;
        double thenPeriod = lastPeriod;
        if (lastPeriod > 0.0)
        {
            const int ticksBack = std::clamp ((int) ((total - 2.0 * lastPeriod) / tickEvery), 0, historySize - 1);
            const double older = periodHistory[(size_t) ((historyAt - 1 - ticksBack) & (historySize - 1))];
            if (older > 0.0)
                thenPeriod = older;
        }
        // Each period is measured on its own and carries a little jitter, which grain spacing should not inherit.
        smoothedPeriod = control.voiced && smoothedPeriod > 0.0 ? smoothedPeriod + spacingSmooth * (thenPeriod - smoothedPeriod) : thenPeriod;
        control.inputPeriod = smoothedPeriod;
        const double fromInput = (smoothedPeriod > 0.0 ? std::log2 (sr / smoothedPeriod) : 0.0) + (current.pitch + rangeNow) / 12.0;
        const double fromRobot = std::log2 (std::max (s.robotHz, 20.0)) + current.userPitch / 12.0;
        control.periodScale = std::pow (2.0, -(current.pitch + rangeNow) / 12.0);
        control.robot = current.robot;
        control.robotPeriod = sr / std::pow (2.0, fromRobot);
        control.vibratoScale = std::pow (2.0, -vibrato / 1200.0);
        readingOutHz = std::pow (2.0, fromInput + current.robot * (fromRobot - fromInput) + vibrato / 1200.0);
        wantedOutputPeriod = sr / readingOutHz;
        pulseGate += fadeSmooth * ((control.voiced ? 1.0 : 0.0) - pulseGate);
        control.noCorrection = std::abs (current.pitch + rangeNow) < 1.0e-3 && current.robot <= 0.0 && current.monster <= 0.0 && std::abs (current.vibrato) <= 0.0;
        control.alternate = 0.10 * current.monster;
        control.gainAlternate = 0.35 * current.monster;
        if (current.monster > 0.0)
            jitterHeld += 0.15 * ((2.0 * nextUniform() - 1.0) - jitterHeld);
        control.jitter = 0.04 * current.monster * jitterHeld;
        formantRatio = std::pow (2.0, current.formant / 12.0);
        sharpen = 1.0 + 0.3 * current.robot;
    }

    double sr = 48000.0;
    int channels = 2, order = 48, shifterDelay = 0, whitenDelay = 0, total = 0, delaySize = 1, tickEvery = 32, tickPhase = 0, historySize = 16;
    bool started = false;
    long long input = 0, historyAt = 0;
    EnvelopeEngine envelope;
    PitchTracker tracker;
    PsolaShifter shifter;
    PsolaShifter::Control control;
    std::array<std::vector<float>, 2> dryLine;
    std::array<std::array<double, EnvelopeEngine::maxOrder>, 2> analysisState {}, synthesisState {};
    std::array<double, EnvelopeEngine::maxOrder> analysisK {}, synthesisK {};
    double synthesisGain = 1.0, formantRatio = 1.0, sharpen = 1.0;
    std::vector<uint8_t> history;
    std::vector<double> periodHistory;
    double spacingSmooth = 0.1, smoothedPeriod = 0.0;

    Settings wanted;
    Smoothed current;
    double pitchSmooth = 0.1, formantSmooth = 0.1, blendSmooth = 0.1, fadeSmooth = 0.1, rangeSmooth = 0.01, learnRate = 0.001;
    double readingOutHz = 0.0, wantedOutputPeriod = 0.0, pulseNext = 0.0, pulsePeriod = 100.0, pulseGate = 0.0;
    static constexpr int pulseTaps = 6, pulseRingSize = 32;
    std::array<double, pulseRingSize> pulseRing {};
    long long pulseClock = 0;
    double lfoPhase = 0.0, lastPeriod = 0.0, learned = 0.0, rangeHeld = 0.0, rangeNow = 0.0, jitterHeld = 0.0;
    int learnedFrames = 0;
    uint64_t randomState = 0x9E3779B97F4A7C15ull;
    std::array<double, 2> breathPower {};
    double tilt = 0.97;
    std::array<double, 2> tiltIn {}, tiltOut {};
    float lastMono = 0.0f;
    double breathRate = 0.01, levelRate = 0.001, levelGainRate = 0.001, levelRelease = 0.01, levelIn = 0.0, levelOut = 0.0, levelGain = 1.0;
    std::atomic<float> readDetected { 0.0f }, readOutput { 0.0f }, readRange { 0.0f };
};
} // namespace smoothmorph
