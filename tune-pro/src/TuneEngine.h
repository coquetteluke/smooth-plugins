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
#include "HarmonicShifter.h"
#include "PitchCorrector.h"
#include "PitchTracker.h"
#include "PsolaShifter.h"

#ifndef MODEL_FOLLOWS_GRAINS
 #define MODEL_FOLLOWS_GRAINS true
#endif
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
    // Poles in each model of the vocal tract, at 48 kHz.
    static constexpr double lowLatencyOrder = 32.0, highQualityOrder = 48.0;
    // How much corrected voice the level trim averages over, by engine. Low Latency needs longer since 1.4.0 holds the shape through a note
    // start: at 0.6 its worst level change read 1.53 dB, at 2.0 it reads 1.45. In High Quality 1.4.0's grains moved high notes' formants past
    // their limit at 2.0. The harmonic shifter takes 1.0: the trim follows the voice's own rise and fall as harmonics cross a formant, which
    // weights some moments over others and reads as a formant move (third held out set 4.35% at 0.6, 3.73% at 1.0, limit 4), while longer
    // lets the level drift further where a glide starts (fourth held out set 1.27 dB at 1.0, limit 1.5).
    static constexpr double lowLatencyLevelMemorySeconds = 2.0, highQualityLevelMemorySeconds = 1.0;
    // High Quality's tracker keeps a period that fits at least this many times better than half of it. See PitchTracker::setStrictOctaveUp.
    static constexpr double highQualityStrictOctaveUp = 2.0;
    // How much input each model of the vocal tract sees.
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
        harmonic.prepare (sr, channels);
        envelope.prepare (sr, channels);
        corrector.prepare (sr);
        drySize = 1;
        while (drySize < (int) (sr * 0.25)) drySize *= 2;
        for (auto& d : dry) d.assign ((size_t) drySize, 0.0f);
        tickSamples = std::max (1, (int) std::lround (sr * 0.001));
        tickSeconds = tickSamples / sr;
    }

    // Starts this engine clean on a new range, delay and mode. No allocation, safe on the audio thread.
    // belowHz: notes under lowHz that are still followed, at the delay lowHz sets. 0 for none. See PitchTracker::setRange.
    void configure (int inputTypeIndex, double lowHz, double highHz, int modeIndex, double belowHz = 0.0) noexcept
    {
        inputType = inputTypeIndex;
        mode = modeIndex;
        latency = latencyFor (lowHz, mode, sr);
        baseLatency = latencyFor (lowHz, lowLatency, sr);
        tracker.reset();
        tracker.setRange (lowHz, highHz, belowHz);
        tracker.setStrictOctaveUp (mode == highQuality ? highQualityStrictOctaveUp : 0.0);
        lastVoicingLimit = -1.0f;
        // High Quality's sweep memory model has half as many poles again. With the memory showing it the peaks between the harmonics, the finer
        // model follows them closely, so less of the vocal tract is left in the grains to move with the pitch. Other models keep 32, see EnvelopeFilter.
        envelope.setOrder (mode == highQuality ? highQualityOrder : lowLatencyOrder, mode == highQuality ? EnvelopeFilter::maxOrder : 64);
        if (mode == highQuality)
            envelope.configure (highQualityWindowSeconds, 0.5 * highQualityWindowSeconds, 0.5);
        else
            envelope.configure (lowLatencyWindowSeconds, 0.0, lowLatencyFallShare);
        // Without lookahead the direct fit can lock on to a harmonic sitting on the first formant, and its level then swings when the harmonic moves away.
        envelope.setHarmonicModelFrom (mode == highQuality ? 300.0 : 130.0);
        // High Quality remembers the harmonics as the pitch moves, which shows the formant peaks far better, so it needs much less sharpening.
        envelope.setSharpen (mode == highQuality ? 0.1 : 0.35);
        envelope.setSweepMemory (mode == highQuality ? 0.2 : 0.0, 400.0);
        ahead = envelope.getLookahead();
        levelGain = 1.0;
        wetLevel = dryLevel = levelSeen = 0.0;
        passMix = 1.0;
        passStep = 1.0 / (0.003 * sr);
        correctingFor = 0;
        levelMemory = (mode == highQuality ? highQualityLevelMemorySeconds : lowLatencyLevelMemorySeconds) * sr;
        matchEase = 1.0 / levelMemory;
        levelFollow = 1.0 - std::exp (-1.0 / (0.03 * sr));
        levelRelease = 1.0 - std::exp (-1.0 / (0.003 * sr));
        shifter.reset();
        shifter.setLatency (latency - ahead);
        harmonic.reset();
        harmonic.setLatency (latency);
        harmonicControl = {};
        corrector.reset();
        control = {};
        for (auto& d : dry) std::fill (d.begin(), d.end(), 0.0f);
        dryAt = 0;
        tickPhase = 0;
        ringAt = 0;
        ring.fill (ControlPoint {});
        const double extra = mode == highQuality ? highQualityExtraSeconds : 0.0;
        voicedHold = 0;
        harmonicShare = 1.0;
        harmonicShareFresh = true;
        useHarmonic = true;
        grainsIdle = harmonicIdle = false;
        grainsAwake = harmonicAwake = 1 << 20;   // both run from the start
        grainsRestFor = harmonicRestFor = 0;
        pastLineFor = 0;
        grainsPower = harmonicPower = 0.0;
        grainsMatch = harmonicMatch = 1.0;
        matchedForHarmonic = true;
        bothRunning = 0;
        pathFollow = 1.0 - std::exp (-1.0 / (0.1 * sr));
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
        if (mode == highQuality)
        {
            harmonic.write (0, l);
            if (channels > 1) harmonic.write (1, r);
            harmonic.advance (tracker);
        }

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
            if (grainsIdle)
            {
                // Not heard and not about to be: the grains' delay line is kept filled, nothing more.
                shifter.write (0, dry[0][(size_t) (index & last)]);
                if (channels > 1) shifter.write (1, dry[1][(size_t) (index & last)]);
                shifter.advance();
            }
            else
            {
                envelope.loadAnalysisFor (index);
                shifter.write (0, (float) envelope.whiten (0, (double) dry[0][(size_t) (index & last)]));
                if (channels > 1) shifter.write (1, (float) envelope.whiten (1, (double) dry[1][(size_t) (index & last)]));
                shifter.advance();
                shifter.step (tracker, control, wet);
            }

            const long long at = dryAt - latency;
            if (at >= 0)
            {
                if (grainsIdle)
                {
                    wet[0] = dryOut[0];
                    wet[1] = dryOut[1];
                }
                else
                {
                    // The vocal tract model is put back for the moment the grains were read from, not for the moment they are heard. A grain
                    // is cut up to 0.6 of a period from where it is heard, and in Alto/Tenor from 98 to about 140 Hz, where the mark for the
                    // nearer period is not known in time, up to a whole period (10 ms). Each sample was whitened with the model of its own
                    // moment; shaped with a later one, any quick change in the model between the two came out as a fault. A tick in the voice
                    // is the plain case: the model that whitened it had not seen it yet, the model 7 ms later has, and is flatter for it, so
                    // the tick came out 11 to 21 dB louder above 9 kHz and the vowel under it 10 dB down for 5 to 15 ms (real takes: 02 at
                    // 7.085 s, 01 at 163.325 s). High Quality is left as it was: its settings were tuned with the model at the output's moment.
                    const long long modelAt = modelFollowsGrains && mode != highQuality ? std::max (0LL, at + (long long) std::llround (shifter.readOffset())) : at;
                    envelope.loadSynthesisFor (modelAt);
                    wet[0] = (float) envelope.shape (0, (double) wet[0]);
                    if (channels > 1) wet[1] = (float) envelope.shape (1, (double) wet[1]);
                }
                // High Quality, unless Classic: below about 220 Hz the grains, above it the harmonic shifter, see harmonicShare. Both always run,
                // so either is ready the moment it is wanted. The harmonic shifter hands back to the input while not voiced, and while a voiced
                // stretch has not been corrected at all, since then the input is exactly what it would give.
                bool harmonicPass = false;
                if (mode == highQuality && ! control.plainGrains)
                {
                    float h[2] = { 0.0f, 0.0f };
                    const bool ok = harmonic.step (harmonicControl, dryOut, h);
                    const double w = harmonicShare;
                    float hw[2] = { ok ? h[0] : dryOut[0], ok ? (channels > 1 ? h[1] : h[0]) : dryOut[1] };
                    if (channels < 2) wet[1] = wet[0];
                    // The two paths do not give quite the same level, and the level trim follows only slowly, so where one hands over to the
                    // other the level stepped, up to 10 dB for 150 ms on a glide across the crossover. Both paths' levels are followed over
                    // 0.1 s while both run, and at a hand over the incoming one is matched to the outgoing one; the match then eases back to
                    // 1 as the trim catches up.
                    if (! grainsIdle && ok && control.voiced)
                    {
                        grainsPower += pathFollow * ((double) wet[0] * wet[0] + (double) wet[1] * wet[1] - grainsPower);
                        harmonicPower += pathFollow * ((double) hw[0] * hw[0] + (double) hw[1] * hw[1] - harmonicPower);
                        bothRunning = std::min (bothRunning + 1, 1 << 30);
                    }
                    else
                        bothRunning = 0;
                    if (useHarmonic != matchedForHarmonic)
                    {
                        matchedForHarmonic = useHarmonic;
                        // Only from levels both paths have given for a while; otherwise no match.
                        if (bothRunning > (int) (0.1 * sr) && grainsPower > 1.0e-12 && harmonicPower > 1.0e-12)
                        {
                            const double r = std::sqrt (grainsPower / harmonicPower);
                            // Keep the output where it is: the incoming path takes the outgoing one's level.
                            if (useHarmonic) { harmonicMatch = std::clamp (r * grainsMatch, 0.5, 2.0); grainsMatch = 1.0; }
                            else { grainsMatch = std::clamp (harmonicMatch / r, 0.5, 2.0); harmonicMatch = 1.0; }
                        }
                        else
                            grainsMatch = harmonicMatch = 1.0;
                    }
                    harmonicMatch += matchEase * (1.0 - harmonicMatch);
                    grainsMatch += matchEase * (1.0 - grainsMatch);
                    wet[0] = (float) (w * harmonicMatch * hw[0] + (1.0 - w) * grainsMatch * wet[0]);
                    wet[1] = (float) (w * harmonicMatch * hw[1] + (1.0 - w) * grainsMatch * wet[1]);
                    harmonicPass = w >= 1.0 && (! ok || (control.noCorrection && ! harmonic.hasCorrected()));
                }
                else if (mode == highQuality)
                    harmonic.restart();
                holdLevel (wet, dryOut);

                // Through a consonant the filters ring on for a few ms from the vowel before it. Nothing is being changed then, so the true input takes over.
                const bool voicedNow = mode == highQuality && ! control.plainGrains && harmonicShare > 0.0 ? harmonicControl.voiced : control.voiced;
                const bool pass = (! voicedNow && ! control.plainGrains) || harmonicPass;
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

    // Moving the harmonics changes how many land on a formant, which changes the level on average. This trims that average, over about a second (two in Low Latency) of
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
            {
                wanted = std::clamp (std::sqrt (dryLevel / wetLevel), 0.25, 4.0);
                // High Quality trusts the average only as far as its memory has filled: a fraction of a second says little about it. Where
                // harmonics cross a narrow first formant, the input and a real voice at the new pitch swing up to 12 dB in opposite directions
                // within 0.3 s. Fully trusted, the first 0.2 s of a glide, input loud and output rightly quiet, set the gain 8 dB up, and it
                // stayed up through the next second, when the input went quiet (seventh held out set, Korean wi at 237 Hz up 4: 1.59 dB louder).
                // So the correction in dB is scaled by how full the memory is.
                if (mode == highQuality)
                    wanted = std::pow (wanted, levelSeen / levelMemory);
            }
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
        // The pitch itself is read from further back still, so that it is in line with the audio the grains are giving out. See pitchInLine.
        if (alignPitch && (alignHighQuality || mode != highQuality) && period > 0.0)
            period = pitchInLine (period, (ringAt + size - controlDelayTicks) % size);
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
            harmonicControl.formantScale = scale;
        }
        else
        {
            control.grainRate = 1.0;
            control.noCorrection = true;
        }
        control.plainGrains = shared.settings.classic;
        // The tracker can flicker between voiced and not for a period or two where a voice starts. Each flicker would hand over to the
        // input and back, and the harmonic shifter's timing sits up to half a period from the input's, so the two would partly cancel.
        // Gaps up to 6 ms or a period, whichever is longer, are bridged. High Quality decides voicing 10 ms ahead, so an S after a vowel
        // over 100 Hz still starts untouched.
        if (control.voiced) { voicedHold = std::max (harmonicHoldTicks, (int) std::lround (period / tickSamples)); }
        else if (voicedHold > 0) --voicedHold;
        harmonicControl.voiced = control.voiced || voicedHold > 0;
        if (control.voiced) harmonicControl.outputPeriod = control.outputPeriod;
        else if (voicedHold <= 0) harmonicControl.outputPeriod = 0.0;
        if (control.voiced) harmonicControl.inputPeriod = control.inputPeriod;
        else if (voicedHold <= 0) harmonicControl.inputPeriod = 0.0;
        // Throat changes the voice even where the pitch is left alone.
        harmonicControl.noCorrection = control.noCorrection && std::abs (harmonicControl.formantScale - 1.0) < 1.0e-4;
        // High Quality's grains sit on the tracker's marks, not on the pulses. The pulse offset is found once where a voiced stretch starts, before
        // the model has settled, and then held: on a glide across a narrow first formant it sat 0.27 to 0.49 of a period off the pulses found
        // later, so each grain split a pulse in two and the voice above the second formant lost 4 to 9 dB (a u with F1 and F2 close, 149 Hz
        // moved down 3). Following the new readings instead did worse than no centring. Since 1.5.0 these grains only run below about 220 Hz.
        // Measured on dev voices (EngineHarness speak8, men 94 to 149 Hz and women 187 to 231 Hz, shifts of 1, 3 and 4 both ways, the six dev
        // vowels plain and styled and 36 random plausible vowels): worst men's formant move 6.51% to 5.76%, cases over 3.5% 20 to 7, women's
        // 6.32% to 4.77%; dev formant test 2.11% to 2.30%, first formant flatter 3.92 to 4.32 dB, big shifts and held notes as before.
        control.centreOnPulses = mode != highQuality;
        // Low Latency's model lags the voice, which matters most on low voices, whose periods are long against it. It leaves those alone.
        control.pulseLowestHz = mode == highQuality ? 0.0 : 130.0;
        control.downReadPower = mode == highQuality ? 0.0 : 0.2;
        // Which path High Quality gives out. The grains read the shape of each period, which holds the vocal tract's ring as long as a
        // period outlasts it: a first formant 70 Hz wide rings for about 1 / (pi x 70) = 4.5 ms, the period at 220 Hz. Above that a grain cuts the
        // ring short and the harmonic shifter does better. Measured on the dev vowels, +/-2 and +/-3, glides and held notes (EngineHarness
        // cross): grains better at 110 and 147 Hz (held notes 0.95 and 1.21 dB against 3.24 and 1.95), about even at 196 and 247, the harmonic
        // shifter better from 294 up (first formant 2.6 dB flatter than a real voice against 7.5). The two give the same pitch but not the same
        // timing within a period, so a lasting blend partly cancels (held notes 6.5 dB at an even blend): the choice switches with 1.2 semitones
        // either side of 220 Hz to spare, so vibrato does not flip it, and crosses over in 20 ms. On dev glides across the crossover the deepest
        // 10 ms dip against a real voice reads 8.6 dB, as on glides that never cross (8.7), with the level match in process(). A faster
        // cross over dips less (5.6 dB at 5 ms) but the jump between two waveforms then reads as noise in a quiet upper band (formant move 7.6%
        // on a regression voice at 5 ms, 2.9% at 10 ms, 1.9% at 20 ms). A voiced stretch starts on whichever side it is.
        //
        // Running both costs about 7% of a core. So a path that is not heard and is well away from being wanted rests: the grains above
        // 280 Hz stop entirely, and below 190 Hz the harmonic shifter only listens, so its memory of the voiced stretch is kept. Each wakes
        // when the pitch comes back inside that, and is only switched to once it has been awake long enough to give out something whole:
        // the grains a latency's worth, the harmonic shifter 30 ms.
        if (mode == highQuality && ! shared.settings.classic)
        {
            if (control.voiced)
            {
                const double f0 = sr / period;
                if (harmonicShareFresh)
                {
                    useHarmonic = f0 > std::sqrt (grainsBelowHz * harmonicAboveHz);
                    pastLineFor = 0;
                    // A path that has only just woken has nothing whole to give yet: start on the other one.
                    if (! useHarmonic && grainsAwake < latency / tickSamples) useHarmonic = true;
                    else if (useHarmonic && harmonicAwake < harmonicWakeTicks && grainsAwake >= latency / tickSamples) useHarmonic = false;
                    harmonicShare = useHarmonic ? 1.0 : 0.0;
                    matchedForHarmonic = useHarmonic;
                    grainsMatch = harmonicMatch = 1.0;
                    harmonicShareFresh = false;
                }
                else
                {
                    // Only once the pitch has stayed past the line for a while: a tracker that jumps an octave for a few ms, as on some
                    // real recordings, would otherwise flip the path back and forth, and each flip blends the two for a moment.
                    pastLineFor = (useHarmonic ? f0 < grainsBelowHz : f0 > harmonicAboveHz) ? pastLineFor + 1 : 0;
                    if (pastLineFor >= switchAfterTicks)
                    {
                        if (useHarmonic && grainsAwake >= latency / tickSamples) { useHarmonic = false; pastLineFor = 0; }
                        else if (! useHarmonic && harmonicAwake >= harmonicWakeTicks) { useHarmonic = true; pastLineFor = 0; }
                    }
                    const double step = tickSeconds / pathSwitchSeconds;
                    harmonicShare = useHarmonic ? std::min (1.0, harmonicShare + step) : std::max (0.0, harmonicShare - step);
                }
                // Only once it has held for a while, so a moment's misreading does not put a path to rest.
                grainsRestFor = harmonicShare >= 1.0 && f0 > grainsRestAboveHz ? grainsRestFor + 1 : 0;
                harmonicRestFor = harmonicShare <= 0.0 && f0 < harmonicRestBelowHz ? harmonicRestFor + 1 : 0;
                const bool grainsRest = grainsRestFor >= restAfterTicks, harmonicRest = harmonicRestFor >= restAfterTicks;
                if (grainsIdle && ! grainsRest) envelope.forgetHistory();
                grainsIdle = grainsRest;
                harmonicIdle = harmonicRest;
            }
            else
            {
                if (voicedHold <= 0)
                    harmonicShareFresh = true;
                if (grainsIdle) envelope.forgetHistory();
                grainsIdle = harmonicIdle = false;
                grainsRestFor = harmonicRestFor = 0;
            }
            grainsAwake = grainsIdle ? 0 : std::min (grainsAwake + 1, 1 << 20);
            harmonicAwake = harmonicIdle ? 0 : std::min (harmonicAwake + 1, 1 << 20);
        }
        else
            grainsIdle = harmonicIdle = false;
        harmonic.setQuiet (harmonicIdle);
        // Classic has plain grains, so it needs no filters. The harmonic shifter needs none either, but High Quality's grains do.
        envelope.setStrength (shared.settings.classic || grainsIdle ? 0.0 : 1.0);
    }

    // The grains give out the input one whole latency late: 1.33 times the Input Type's longest period, so that even its lowest note has
    // its mark in time. The pitch the tracker reports is younger than that. It is the spacing of its two newest marks, which by the
    // time a decision is heard, half a grain later, is about two of the voice's own periods old, not of the
    // longest one (see the ages below). So on every note above the bottom of the range the pitch control ran ahead of the audio, by the difference: the
    // output began to bend up to 11 ms before the sound it belonged to (Low Male and Wide Range, measured on real takes: 4 to 11 ms; Alto/Tenor about
    // 1 ms). The control is now read that much further back, a little more on a short period and nothing at the bottom of the range.
    // Whether a sound is voiced at all is still decided where it was, so a note is not picked up any later.
    double pitchInLine (double period, int nominal) const noexcept
    {
        // How old the reported pitch is by the time it is heard, in periods, measured on a voice with vibrato at 147, 196 and 262 Hz
        // (testTiming): 1.89 to 1.94 for the grains in Low Latency and Classic, 1.67 to 1.83 for High Quality's grains, 1.39 to 1.41
        // for its harmonic shifter, which reads a whole period at once.
        const double age = mode != highQuality ? grainPitchAge : (harmonicShare > 0.5 && ! control.plainGrains ? harmonicPitchAge : highQualityGrainPitchAge);
        const double lead = ((double) baseLatency - age * period) / (double) tickSamples;
        if (lead <= 0.0)
            return period;
        const int size = (int) ring.size();
        const int whole = (int) lead;
        const double part = lead - whole;
        const double a = ring[(size_t) ((nominal + 2 * size - whole) % size)].period;
        const double b = ring[(size_t) ((nominal + 2 * size - whole - 1) % size)].period;
        if (a > 0.0 && b > 0.0)
            return a + part * (b - a);
        if (a > 0.0)
            return a;
        // The note is younger than that: the oldest reading it has
        for (int k = whole - 1; k > 0; --k)
        {
            const double p = ring[(size_t) ((nominal + 2 * size - k) % size)].period;
            if (p > 0.0)
                return p;
        }
        return period;
    }

    static constexpr bool modelFollowsGrains = MODEL_FOLLOWS_GRAINS;
    static constexpr bool alignPitch = true;
    // High Quality is left as it was, for now. Its control runs ahead in the same way (73 samples at the median of Luke's takes in
    // Alto/Tenor, 261 in Low Male), and reading it in line fixes that (20 samples late in Low Male), but High Quality's vocal tract
    // settings were all tuned with the lead in place: with it gone the ninth held out set's speaking formant move reads 5.00% against
    // 4.62% (limit 4). So that needs High Quality's settings looked at again, on a held out set that has not been seen.
    static constexpr bool alignHighQuality = false;
    // On Luke's real takes the readings are a little older than on the steady test voice (weak marks hold their period, and under
    // about 140 Hz in Alto/Tenor the reading is older than the whole latency, which nothing can make up for): with the test voice's
    // 1.92 the pitch came 68 samples late at the median of 11 takes in Alto/Tenor and 19 in Low Male. 2.02 keeps the test voice
    // within 1 ms and brings the takes nearer.
    static constexpr double grainPitchAge = 2.02, highQualityGrainPitchAge = 1.75, harmonicPitchAge = 1.40;
    double sr = 48000.0, tickSeconds = 0.001;
    int baseLatency = 0;
    int channels = 2, tickSamples = 48, tickPhase = 0, latency = 0, inputType = 1, mode = lowLatency;
    PitchTracker tracker;
    PsolaShifter shifter;
    HarmonicShifter harmonic;
    HarmonicShifter::Control harmonicControl;
    EnvelopeFilter envelope;
    PitchCorrector corrector;
    PsolaShifter::Control control;
    std::array<std::vector<float>, 2> dry;
    int drySize = 16384;
    long long dryAt = 0;
    std::array<ControlPoint, 128> ring {};
    int ringAt = 0, controlDelayTicks = 0, leadTicks = 0, ahead = 0;
    float lastVoicingLimit = -1.0f;
    int correctingFor = 0, voicedHold = 0;
    static constexpr int harmonicHoldTicks = 6;
    static constexpr double grainsBelowHz = 205.0, harmonicAboveHz = 235.0, pathSwitchSeconds = 0.02;
    double harmonicShare = 1.0;   // High Quality: 0 gives out the grains, 1 the harmonic shifter
    bool harmonicShareFresh = true, useHarmonic = true, grainsIdle = false, harmonicIdle = false;
    static constexpr double grainsRestAboveHz = 280.0, harmonicRestBelowHz = 190.0;
    static constexpr int harmonicWakeTicks = 30;
    static constexpr int restAfterTicks = 100, switchAfterTicks = 40;
    int pastLineFor = 0;
    int grainsAwake = 0, harmonicAwake = 0, grainsRestFor = 0, harmonicRestFor = 0;
    double grainsPower = 0.0, harmonicPower = 0.0, grainsMatch = 1.0, harmonicMatch = 1.0, pathFollow = 0.001, matchEase = 0.001;
    bool matchedForHarmonic = true;
    int bothRunning = 0;
    double passMix = 1.0, passStep = 0.01;
    double levelGain = 1.0, wetLevel = 0.0, dryLevel = 0.0, levelSeen = 0.0, levelMemory = 48000.0, levelFollow = 0.001, levelRelease = 0.01;
};
} // namespace smoothtune
