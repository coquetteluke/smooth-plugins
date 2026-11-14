#pragma once
// The voice changer, one sample at a time:
//
//   input > pre-emphasis > whitening filter > grain shifter > breath or Robot pulses > filter put back > de-emphasis > level match > mix
//
// The tracker hears the input as it arrives. The whitening filter needs half a window of look ahead, so it runs behind that, which
// also gives the tracker a head start: the marks grains need are already known, and the total delay stays near 24 ms.
//
// With nothing asked of it, grains read the excitation at the reported delay and the two filters are exact opposites, so the output
// is the delayed input to rounding. Unvoiced sound takes that same path whatever Pitch says, so consonants aren't shifted at all.
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
        bool peakGuard = false;     // on: the output never peaks higher than the input has (times Output)
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
        airFall = smooth (0.003);
        rangeSmooth = smooth (0.150);
        learnRate = smooth (1.5);
        breathRate = 1.0 - std::exp (-1.0 / (0.006 * sr));
        levelRate = 1.0 - std::exp (-1.0 / (0.025 * sr));
        levelGainRate = 1.0 - std::exp (-1.0 / (0.015 * sr));
        levelGainRateOnset = 1.0 - std::exp (-1.0 / (0.002 * sr));
        levelRateOnset = 1.0 - std::exp (-1.0 / (0.005 * sr));
        onsetSamples = (int) std::lround (0.030 * sr);
        memoryRate = 1.0 - std::exp (-1.0 / (0.300 * sr / tickEvery));
        levelRelease = 1.0 - std::exp (-1.0 / (0.003 * sr));
        guardRelease = 1.0 - std::exp (-1.0 / (0.100 * sr));
        fastRate = 1.0 - std::exp (-1.0 / (0.005 * sr));
        burstRelease = 1.0 - std::exp (-1.0 / (0.015 * sr));
        tilt = std::exp (-2.0 * M_PI * 200.0 / sr);
        for (auto& f : airFilter) f.highPass (4500.0, sr, 0.707);
        historySize = 1;
        while (historySize < total / tickEvery + 4) historySize *= 2;
        history.assign ((size_t) historySize, 0);
        periodHistory.assign ((size_t) historySize, 0.0);
        spacingSmooth = smooth (0.002);
        leapHold = std::max (1, (int) std::lround (0.020 * tickRate));
        settingTrusted = std::max (1, (int) std::lround (0.5 * tickRate));
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
        wasVoicedOut = false;
        leapTicks = 0;
        airWasVoiced = false;
        airStartLeft = 0;
        settingHeldTicks = 0;
        lastSettingKey = 0.0;
        historyAt = 0;
        learned = 0.0;
        learnedFrames = 0;
        rangeHeld = 0.0;
        rangeNow = 0.0;
        rangeWasActive = rangeFast = false;
        current = {};
        started = false;
        formantRatio = sharpen = 1.0;
        whisperKeep = 1.0;
        whisperBreath = 0.0;
        control = {};
        jitterHeld = 0.0;
        breathPower.fill (0.0);
        levelIn = levelOut = 0.0;
        levelGain = 1.0;
        gainMemory = 1.0;
        logGainMemory = 0.0;
        levelWasVoiced = false;
        onsetLeft = 0;
        heldPeak = 0.0;
        guardGain = 1.0;
        fastIn = fastOut = dryHold = 0.0;
        burstGain = 1.0;
        fastRing.fill (0.0);
        fastAt = 0;
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
    // For the tests only (1.2.6): see PsolaShifter::furthestPastNewestPulse.
    double grainsPastNewestPulse() const noexcept { return shifter.furthestPastNewestPulse(); }
    double grainsPastNewestLouderPulse() const noexcept { return shifter.furthestPastNewestLouderPulse(); }

    // Runs in place. Input is cleaned here: not finite goes to 0, the rest is clamped to +/-16.
    void process (float* const* audio, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float x[2] = { clean (audio[0][i]), channels > 1 ? clean (audio[1][i]) : 0.0f };
            const float mono = channels > 1 ? 0.5f * (x[0] + x[1]) : x[0];
            // Peak Guard's ceiling: the highest peak the input has had. The input runs one delay ahead of the output, so it already
            // covers the sound now coming out. Kept whether the guard is on or not, so switching it on mid song works at once.
            heldPeak = std::max (heldPeak, (double) std::max (std::abs (x[0]), std::abs (x[1])));

            if (--tickPhase <= 0)
            {
                tickPhase = tickEvery;
                tick();
            }

            fastIn += fastRate * ((double) x[0] * x[0] + (double) x[1] * x[1] - fastIn);
            tracker.push (mono);
            // Pre-emphasis takes the source's tilt out before the fit, so formants high up are fitted as well as low ones,
            // and they alone move when formants do. The de-emphasis after the filter puts the tilt back.
            const float tilted = mono - (float) tilt * lastMono;
            lastMono = mono;
            envelope.push (tilted, formantRatio, control.voiced ? sharpen : 1.0, control.voiced && control.inputPeriod > 0.0 ? sr / control.inputPeriod : 0.0, ! control.noCorrection, tracker.isVoiced(), fullEnvelope);

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
                if (channels == 2)
                {
                    const double raw0 = (double) dryLine[0][(size_t) readAt], raw1 = (double) dryLine[1][(size_t) readAt];
                    double e0, e1;
                    latticeAnalyse2 (raw0 - tilt * tiltIn[0], raw1 - tilt * tiltIn[1], analysisK.data(), analysisState[0].data(), analysisState[1].data(), order, e0, e1);
                    tiltIn[0] = raw0;
                    tiltIn[1] = raw1;
                    shifter.write (0, (float) e0);
                    shifter.write (1, (float) e1);
                    shifter.writeVoice ((float) (0.5 * (raw0 + raw1)));
                }
                else
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        const double raw = (double) dryLine[(size_t) ch][(size_t) readAt];
                        const double e = latticeAnalyse (raw - tilt * tiltIn[(size_t) ch], analysisK.data(), analysisState[(size_t) ch].data(), order);
                        tiltIn[(size_t) ch] = raw;
                        shifter.write (ch, (float) e);
                    }
                if (channels == 1)
                    shifter.writeVoice (dryLine[0][(size_t) readAt]);
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
            const double noise = current.whisper > 0.0 ? gaussianAt (input) : 0.0;
            const double keep = whisperKeep, breath = whisperBreath;

            const int dryAt = (int) ((input - total) & (long long) (delaySize - 1));
            double wet[2] = { 0.0, 0.0 };
            double powerIn = 0.0, powerOut = 0.0;
            const bool hasDry = input >= total;
            float dry[2] = { 0.0f, 0.0f };
            // One channel's excitation with the breath, Robot's pulses and Whisper's noise mixed in, ready for the filter.
            const auto excite = [&] (int ch) noexcept
            {
                double e = (double) excitation[ch];
                // A voice that is part noise (a Z, a breathy note) has a top that is mostly hiss, which grains spread thin or pile up. So on
                // those sounds the top of the excitation stays as it was at the output's own moment. A clean voice's top is harmonics and
                // is left to the grains. With nothing shifted the two are the same and this adds nothing.
                const double top = airFilter[(size_t) ch].process ((double) shifter.dry (ch) - e);
                if (airFilter[(size_t) ch].bad())
                    airFilter[(size_t) ch].clear();
                else
                    e += (1.0 - current.robot) * airShare * top;
                breathPower[(size_t) ch] += breathRate * (e * e - breathPower[(size_t) ch]);
                if (current.robot > 0.0)
                {
                    // Robot's buzz: the voiced part of the sound comes from a pure pulse train at the set note, all of it at Character Amount 100.
                    const double share = current.robot * current.robot * pulseGate;
                    e = (1.0 - share) * e + share * pulse * std::sqrt (std::max (breathPower[(size_t) ch], 0.0) * pulsePeriod);
                }
                if (current.whisper > 0.0)
                    e = keep * e + breath * noise * std::sqrt (std::max (breathPower[(size_t) ch], 0.0));
                return e;
            };
            // The filter's output for one channel: a value that is not finite or runs away clears every filter, and that sample is 0.
            bool cleared = false;
            const auto land = [&] (int ch, double filtered) noexcept
            {
                double y = filtered + tilt * tiltOut[(size_t) ch];
                tiltOut[(size_t) ch] = y;
                if (! std::isfinite (y) || std::abs (y) > 64.0)
                {
                    clearFilters();
                    cleared = true;
                    y = 0.0;
                }
                return y;
            };
            for (int ch = 0; ch < channels; ++ch)
                dry[ch] = hasDry ? dryLine[(size_t) ch][(size_t) dryAt] : 0.0f;
            if (channels == 2)
            {
                // Both channels through the filter side by side. The second channel's excitation is worked out before the first channel's
                // filter has run, which is the same unless the first one goes bad: that clears every filter (the second channel's breath
                // filter among them) before the second channel is worked out, so then the second channel is done again from there.
                const double e0 = excite (0);
                const double breathBefore = breathPower[1];
                const double e1 = excite (1);
                double f0, f1;
                latticeSynth2 (synthesisGain * e0, synthesisGain * e1, synthesisK.data(), synthesisState[0].data(), synthesisState[1].data(), order, f0, f1);
                const double y0 = land (0, f0);
                double y1;
                if (! cleared)
                    y1 = land (1, f1);
                else
                {
                    breathPower[1] = breathBefore;
                    const double again = excite (1);
                    y1 = land (1, latticeSynth (synthesisGain * again, synthesisK.data(), synthesisState[1].data(), order));
                }
                wet[0] = y0;
                wet[1] = y1;
            }
            else
                wet[0] = land (0, latticeSynth (synthesisGain * excite (0), synthesisK.data(), synthesisState[0].data(), order));
            for (int ch = 0; ch < channels; ++ch)
            {
                powerIn += (double) dry[ch] * dry[ch];
                powerOut += wet[ch] * wet[ch];
            }

            // Level match, shared by both sides so the stereo image stays
            const double levelRateNow = onsetLeft > 0 ? levelRateOnset : levelRate;
            levelIn += levelRateNow * (powerIn - levelIn);
            levelOut += levelRateNow * (powerOut - levelOut);
            if (control.voiced)
            {
                // A voiced sound that starts (after a consonant, a breath or silence) starts from the gain the notes before it needed,
                // and the match follows fast for its first 30 ms. From 1 and at the slow rate, every phrase start came out quiet and
                // late for 40 ms where the changed voice is quieter than the voice going in: at Robot the first 30 ms read 3.7 dB under
                // the dry and half the peak came 18 ms late (median, real takes) (1.2.2).
                if (! levelWasVoiced)
                {
                    levelOut = levelIn / gainMemory; // half way, in dB: a start is a guess
                    onsetLeft = onsetSamples;
                }
                // The guard against 0 / 0 is far under any real level, so the match is the same for a voice at any level (1.2.1)
                const double target = std::clamp (std::sqrt ((levelIn + 1.0e-30) / (levelOut + 1.0e-30)), 0.125, 8.0);
                levelGain += (onsetLeft > 0 || target < levelGain ? levelGainRateOnset : levelGainRate) * (target - levelGain);
                if (onsetLeft > 0)
                    --onsetLeft;
            }
            else
            {
                // Consonants pass unchanged, so nothing to match, and a gain left over from the vowel must not colour them.
                levelOut = levelIn;
                levelGain += levelRelease * (1.0 - levelGain);
                onsetLeft = 0;
            }
            levelWasVoiced = control.voiced;
            if (control.neutral)
            {
                // With everything back at 0 the changed voice is the voice going in, so there is nothing to match: the gain is exactly
                // 1 and the memory of the gains other settings needed is let go. In 1.2.2 that memory set every later phrase start a
                // hair off 1, and after moving a control and back the output stayed -108 to -118 dB away from the delayed input on
                // real takes instead of passing straight through (-142 dB in 1.2.1) (1.2.3).
                levelGain += levelRelease * (1.0 - levelGain); // a 3 ms glide, so the last of a changed voice still on its way out cannot step
                if (std::abs (levelGain - 1.0) < 1.0e-9)
                    levelGain = 1.0;
                levelOut = levelIn;
                gainMemory = 1.0;
                logGainMemory = 0.0;
                onsetLeft = 0;
            }

            const double wetAmount = current.mix * current.engaged;
            // Hiss passes unshifted, so on a sound that is part noise the level match, which answers to the voiced part, is held back from it.
            const double matched = 1.0 + (levelGain - 1.0) * (1.0 - airShare);
            // Burst limit (1.2.0). The voice coming out should be about as loud as the voice going in. The level match sees to that,
            // but it needs 40 ms, and a filter pair that stops matching for a few frames (a note that starts or jumps, a harmonic
            // landing on a peak of the fit, a low steady note that moves) put out 20 dB more than went in on real takes before it
            // noticed. So the voice coming out, over about 5 ms, may be no more than 6 dB over the loudest 5 ms the input has within
            // 20 ms either side (the input is a whole delay ahead, so both sides are known). Anything under that is never touched:
            // the gain here is then exactly 1.
            fastOut += fastRate * (matched * matched * powerOut - fastOut);
            const double burstWant = fastOut > burstAllowed * dryHold ? std::sqrt (burstAllowed * dryHold / fastOut) : 1.0;
            // Lets go towards a little over 1 and stops at 1, so it is exactly 1 again about 60 ms after a burst, not nearly 1 for ever.
            burstGain = std::min (burstWant, burstGain + burstRelease * (1.01 - burstGain));
            const double wetGain = matched * burstGain;
            const double overall = 1.0 + current.engaged * (current.outputGain - 1.0);
            double out[2] = { 0.0, 0.0 };
            for (int ch = 0; ch < channels; ++ch)
                out[ch] = ((1.0 - wetAmount) * (double) dry[ch] + wetAmount * wetGain * wet[ch]) * overall;
            if (wanted.peakGuard || guardGain < 1.0)
            {
                // Moving pitch down or formants about changes the shape of the wave: fewer, stronger pulses, so peaks come out higher
                // than they went in at the same level (7 dB over full scale at Giant on a take that peaks just under it). On, the gain
                // drops at once on a sample that would pass the ceiling and comes back over about 100 ms. Switched off, it lets go at
                // that same rate and then nothing here runs: the output is the same samples as without the control.
                // Never over the input's own highest peak: Output may turn the voice down under it but not past it. With Output
                // allowed through, Deep Double's +2.5 dB put 18 samples over full scale on a hot take with the guard on (1.2.3).
                const double ceiling = heldPeak * std::min (overall, 1.0);
                const double biggest = std::max (std::abs (out[0]), std::abs (out[1]));
                const double want = wanted.peakGuard && biggest > ceiling ? ceiling / biggest : 1.0;
                guardGain = std::min (want, guardGain + guardRelease * (1.0 - guardGain));
                if (! wanted.peakGuard && guardGain > 1.0 - 1.0e-4)
                    guardGain = 1.0; // let go: under a thousandth of a dB is left, and from here nothing runs
                if (guardGain < 1.0)
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        out[ch] *= guardGain;
                        if (wanted.peakGuard) out[ch] = std::clamp (out[ch], -ceiling, ceiling); // the last bit of rounding in the product
                    }
            }
            for (int ch = 0; ch < channels; ++ch)
                audio[ch][i] = (float) std::clamp (out[ch], -16.0, 16.0);
            ++input;
        }
        // Bypassed, what comes out is the input: Output Pitch names its pitch and Range Match moves nothing (1.2.6).
        const double detected = control.voiced && control.inputPeriod > 0.0 ? sr / control.inputPeriod : 0.0;
        readDetected.store ((float) detected, std::memory_order_relaxed);
        readOutput.store ((float) (control.voiced ? (wanted.bypass ? detected : readingOutHz) : 0.0), std::memory_order_relaxed);
        readRange.store ((float) (wanted.bypass ? 0.0 : rangeHeld), std::memory_order_relaxed);
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
        for (auto& f : airFilter) f.clear();
        airShare = 0.0;
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

    // Random numbers that belong to a moment of the input rather than to the order they were asked for: the value for input sample
    // (or tick) n is always the same. So Whisper's breath and Monster's shake are the same noise whenever they were switched on, and a
    // setting reached by moving a control ends up as exactly the same sound as that setting from the start. Before 1.2.1 the noise
    // went on from wherever it was, and two renders of Whisper 100 differed by up to 3.5 dB in a 10 ms frame (1.2.1).
    static uint64_t mix64 (uint64_t z) noexcept
    {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    static double uniformAt (long long n, uint64_t stream) noexcept
    {
        return (double) (mix64 ((uint64_t) n * 2u + stream) >> 11) * (1.0 / 9007199254740992.0);
    }

    // Sum of six uniforms, as before: near Gaussian, variance 1. Two 64 bit draws give the six 21 bit uniforms.
    static double gaussianAt (long long n) noexcept
    {
        const uint64_t a = mix64 ((uint64_t) n * 2u), b = mix64 ((uint64_t) n * 2u + 1u);
        constexpr double unit = 1.0 / 2097152.0;
        constexpr uint64_t mask = (1u << 21) - 1u;
        double s = 0.0;
        for (int k = 0; k < 3; ++k)
            s += ((double) ((a >> (21 * k)) & mask) + 0.5) * unit + ((double) ((b >> (21 * k)) & mask) + 0.5) * unit;
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

        // For the burst limit: the loudest the input is within 20 ms either side of what is coming out now
        fastRing[(size_t) (fastAt & (fastRingSize - 1))] = fastIn;
        ++fastAt;
        {
            const int centre = total / tickEvery, reach = (int) (0.020 * sr) / tickEvery;
            double hold = 0.0;
            for (int k = std::max (0, centre - reach); k <= centre + reach && k < fastRingSize; ++k)
                hold = std::max (hold, fastRing[(size_t) ((fastAt - 1 - k) & (fastRingSize - 1))]);
            dryHold = hold;
        }

        // Range Match: learn where this voice sits, then move it there in whole semitones
        if (voicedNow && period > 0.0)
        {
            const double l = std::log2 (sr / period);
            learned = learnedFrames < 300 ? (learned * learnedFrames + l) / (learnedFrames + 1) : learned + learnRate * (l - learned);
            if (learnedFrames < 300) ++learnedFrames;
        }
        double rangeWanted = 0.0;
        const bool rangeActive = s.rangeMatch && learnedFrames >= 150 && c.robot <= 0.0;
        if (rangeActive)
        {
            const double ideal = 12.0 * (std::log2 (std::max (s.rangeCentreHz, 20.0)) - learned);
            if (std::abs (ideal - rangeHeld) > 0.75)
                rangeHeld = std::clamp (std::round (ideal), -24.0, 24.0);
            rangeWanted = rangeHeld;
        }
        else
            rangeHeld = 0.0;
        // The slow glide is for the whole semitone steps Range Match takes as a voice drifts. Switching it on or off (or a Robot
        // taking over) is a move of the Pitch kind and goes at Pitch's speed: at the slow rate the voice took 1.4 s to come back to
        // its own pitch after Range Match was switched off (1.2.1).
        if (rangeActive != rangeWasActive)
            rangeFast = true;
        rangeWasActive = rangeActive;
        rangeNow += (rangeFast ? pitchSmooth : rangeSmooth) * (rangeWanted - rangeNow);

        const double wantPitch = s.pitchSemitones + amount * c.pitchSemitones;
        const double wantFormant = s.formantSemitones + amount * c.formantSemitones;
        fullEnvelope = std::abs (wantFormant) >= 2.0;
        {
            // How long the setting asked for has held, for the air share at starts below
            const double key = wantPitch + 3.1 * wantFormant + 7.7 * amount * c.robot + 13.3 * amount * c.monster + 17.9 * s.whisper + 23.3 * s.vibratoCents;
            settingHeldTicks = key == lastSettingKey ? std::min (settingHeldTicks + 1, 1 << 20) : 0;
            lastSettingKey = key;
        }
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
        // What the level match has been asking for on voiced sound lately (about 300 ms, as a ratio), for the next phrase start
        if (control.voiced && onsetLeft == 0 && levelGain > 0.0)
        {
            logGainMemory += memoryRate * (std::log (levelGain) - logGainMemory);
            gainMemory = std::exp (logGainMemory);
        }
        // Land exactly, so a voice with everything back at 0 passes straight through.
        if (std::abs (current.pitch - wantPitch) < 1.0e-3) current.pitch = wantPitch;
        if (std::abs (current.formant - wantFormant) < 1.0e-3) current.formant = wantFormant;
        if (std::abs (current.robot - amount * c.robot) < 1.0e-3) current.robot = amount * c.robot;
        if (std::abs (current.monster - amount * c.monster) < 1.0e-3) current.monster = amount * c.monster;
        if (std::abs (current.whisper - s.whisper) < 1.0e-3) current.whisper = s.whisper;
        if (std::abs (current.vibrato - s.vibratoCents) < 1.0e-2) current.vibrato = s.vibratoCents;
        if (std::abs (rangeNow - rangeWanted) < 1.0e-3) { rangeNow = rangeWanted; rangeFast = false; }

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
        // Each period is measured on its own and carries a little jitter, which grain spacing should not inherit. But a voiced
        // stretch starts on its own period, and a jump of over a semitone is taken at once: gliding there from the last stretch's
        // period hands the envelope, for a frame or two, pitches that are not in the sound, and the fit to their "harmonics" made a
        // burst 20 dB over the voice on a real take (1.2.0).
        // A voice does not leap more than an octave and a bit up inside one stretch. Where a note ends or breaks the tracker can land on a
        // period near 1 kHz for a few ms (take 01: 108 > 883 to 978 Hz at 100.27 s, 175 > 938 Hz at 21.67 s), and the grains and the
        // envelope's fit followed it: the output 13 to 23 dB over the dry there. Through such a leap the grains keep the period of the note
        // it came from until the leap has held for 20 ms (1.2.5). Passing the input through instead left notes unshifted where the voice
        // went on (Male To Female, low notes unshifted 0.74 > 1.00%, limit 1).
        const bool leapUp = control.voiced && wasVoicedOut && smoothedPeriod > 0.0 && thenPeriod < 0.45 * smoothedPeriod;
        const bool leaping = leapUp && leapTicks < leapHold;
        if (leaping)
            ++leapTicks;
        else if (! leapUp)
            leapTicks = 0;
        if (! leaping)
        {
            const bool glide = control.voiced && wasVoicedOut && smoothedPeriod > 0.0 && std::abs (thenPeriod / smoothedPeriod - 1.0) < 0.06;
            smoothedPeriod = glide ? smoothedPeriod + spacingSmooth * (thenPeriod - smoothedPeriod) : thenPeriod;
            wasVoicedOut = control.voiced;
        }
        control.inputPeriod = smoothedPeriod;
        const double fromInput = (smoothedPeriod > 0.0 ? std::log2 (sr / smoothedPeriod) : 0.0) + (current.pitch + rangeNow) / 12.0;
        const double fromRobot = std::log2 (std::max (s.robotHz, 20.0)) + current.userPitch / 12.0;
        control.periodScale = std::pow (2.0, -(current.pitch + rangeNow) / 12.0);
        control.robot = current.robot;
        control.robotPeriod = sr / std::pow (2.0, fromRobot);
        control.vibratoScale = std::pow (2.0, -vibrato / 1200.0);
        control.vibratoReach = std::pow (2.0, std::abs (current.vibrato) / 1200.0);
        // The pitch that comes out, held inside the shifter's own limits as the grains are, so Output Pitch and the window name what is
        // heard (1.2.6). Inside them this is the sum it always was.
        double moved = fromInput + current.robot * (fromRobot - fromInput);
        // Robot's buzz keeps the note asked for, as up to 1.2.5: it is a pulse train of its own, not grains. Held with the readout in
        // 1.2.6, it went up with the input's reading where the tracker heard a high pitch at a phrase start or in a consonant, and on
        // round 1's real vocal check that broke Robot's and Robot Announcer's phrase starts (wrong frames 4.935 > 5.004%, limit 5) (1.2.7).
        const double buzz = moved;
        if (smoothedPeriod > 0.0)
        {
            const double inputOctaves = std::log2 (sr / smoothedPeriod), reach = std::log2 (control.vibratoReach);
            const double highest = inputOctaves - std::log2 (PsolaShifter::shortestScale) - reach, lowest = inputOctaves - std::log2 (PsolaShifter::longestScale) + reach;
            if (moved > highest) moved = highest;
            if (moved < lowest) moved = lowest;
        }
        readingOutHz = std::pow (2.0, moved + vibrato / 1200.0);
        wantedOutputPeriod = sr / std::pow (2.0, buzz + vibrato / 1200.0);
        pulseGate += fadeSmooth * ((control.voiced ? 1.0 : 0.0) - pulseGate);
        // How much of the sound is noise, gliding over 15 ms. In the first 30 ms after a voiced start it falls in 3 ms instead: the voice is
        // periodic at once, and while the share glided down from the silence before it the level match was held back for the first 30 ms
        // of every phrase, which with formants moved by the bigger fit came out 1 to 2 dB short (1.2.5). Not with Whisper on: there the start
        // took Old Singer's peak 0.8 dB higher (2.37 > 3.17 dB over the input's, limit 3). Nor in the half second after the setting
        // changes, while the gain remembered from the old setting is still the start's guess: a start 190 ms after a move into Monster
        // came out 3.1 dB over the voice (limit 3).
        const double airWanted = std::clamp (((double) tracker.difference() - 0.05) / 0.15, 0.0, 1.0);
        if (control.voiced && ! airWasVoiced && current.whisper <= 0.0 && settingHeldTicks > settingTrusted)
            airStartLeft = (int) std::lround (0.030 * sr / tickEvery);
        airWasVoiced = control.voiced;
        airShare += (airStartLeft > 0 && airWanted < airShare ? airFall : fadeSmooth) * (airWanted - airShare);
        if (airStartLeft > 0)
            --airStartLeft;
        control.noCorrection = std::abs (current.pitch + rangeNow) < 1.0e-3 && current.robot <= 0.0 && current.monster <= 0.0 && std::abs (current.vibrato) <= 0.0;
        control.neutral = control.noCorrection && std::abs (current.formant) < 1.0e-3 && current.whisper < 1.0e-6;
        control.alternate = 0.10 * current.monster;
        control.gainAlternate = 0.35 * current.monster;
        if (current.monster > 0.0)
            jitterHeld += 0.15 * ((2.0 * uniformAt (historyAt, 1u) - 1.0) - jitterHeld);
        control.jitter = 0.04 * current.monster * jitterHeld;
        formantRatio = std::pow (2.0, current.formant / 12.0);
        sharpen = 1.0 + 0.5 * current.robot;
        // Whisper's balance moves only here, so it is worked out here rather than at every sample (1.2.4). The cosine and the sine stay
        // two calls, as they were: one combined call can differ in the last bit, and then the output would not be the same samples.
        const double angle = 0.5 * M_PI * current.whisper;
        whisperKeep = std::cos (angle);
        volatile double sameAngle = angle;
        whisperBreath = std::sin (sameAngle);
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
    int leapTicks = 0, leapHold = 30, airStartLeft = 0;
    bool airWasVoiced = false, fullEnvelope = false;
    int settingHeldTicks = 0, settingTrusted = 750;
    double lastSettingKey = 0.0;
    double airFall = 0.1;
    bool wasVoicedOut = false;

    Settings wanted;
    Smoothed current;
    double pitchSmooth = 0.1, formantSmooth = 0.1, blendSmooth = 0.1, fadeSmooth = 0.1, rangeSmooth = 0.01, learnRate = 0.001;
    double whisperKeep = 1.0, whisperBreath = 0.0;
    double readingOutHz = 0.0, wantedOutputPeriod = 0.0, pulseNext = 0.0, pulsePeriod = 100.0, pulseGate = 0.0;
    static constexpr int pulseTaps = 6, pulseRingSize = 32;
    std::array<double, pulseRingSize> pulseRing {};
    long long pulseClock = 0;
    double lfoPhase = 0.0, lastPeriod = 0.0, learned = 0.0, rangeHeld = 0.0, rangeNow = 0.0, jitterHeld = 0.0;
    int learnedFrames = 0;
    bool rangeWasActive = false, rangeFast = false;
    std::array<double, 2> breathPower {};
    double tilt = 0.97;
    std::array<Biquad, 2> airFilter;
    double airShare = 0.0;
    std::array<double, 2> tiltIn {}, tiltOut {};
    float lastMono = 0.0f;
    double levelRateOnset = 0.01, levelGainRateOnset = 0.01, memoryRate = 0.001, gainMemory = 1.0, logGainMemory = 0.0;
    int onsetSamples = 0, onsetLeft = 0;
    bool levelWasVoiced = false;
    double breathRate = 0.01, levelRate = 0.001, levelGainRate = 0.001, levelRelease = 0.01, levelIn = 0.0, levelOut = 0.0, levelGain = 1.0;
    double heldPeak = 0.0, guardGain = 1.0, guardRelease = 0.001;
    static constexpr int fastRingSize = 256;      // ticks: reaches a whole delay and 20 ms back at every rate
    static constexpr double burstAllowed = 4.0;   // 6 dB, as power
    std::array<double, fastRingSize> fastRing {};
    long long fastAt = 0;
    double fastIn = 0.0, fastOut = 0.0, dryHold = 0.0, burstGain = 1.0, fastRate = 0.01, burstRelease = 0.01;
    std::atomic<float> readDetected { 0.0f }, readOutput { 0.0f }, readRange { 0.0f };
};
} // namespace smoothmorph
