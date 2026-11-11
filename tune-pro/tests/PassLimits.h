#pragma once
// Pass limits for Smooth Tune. Written and committed before any DSP existed, so no limit was chosen by looking at a result.
// If one of these ever changes, the reason goes next to it and into the report.
namespace limits
{
    // Pitch tracker on synthetic voices, 80 Hz to 1 kHz
    constexpr double trackSteadyMedianCents = 3.0;    // median absolute error, steady notes
    constexpr double trackSteadyP95Cents = 8.0;       // 95th percentile, steady notes
    constexpr double trackVibratoMedianCents = 8.0;   // median absolute error against the true pitch, 5.5 Hz vibrato of +/-50 cents
    constexpr double octaveErrorsPerMinute = 1.0;     // frames over 600 cents off, counted as events, voiced material only
    constexpr double speechOctaveErrorsPerMinute = 30.0; // real speech against a slow two-sided reference, which is not ground truth

    // Output pitch after correction
    constexpr double outputInstantCents = 5.0;        // the task's limit: 95th percentile of frames at Retune Speed 0
    constexpr double speechOutputMedianCents = 12.0;  // real speech at instant speed, median distance from the nearest note
    constexpr double sungOutputMedianCents = 8.0;     // sung-style say material at instant speed

    // Stand-ins for ears on real material. Added before these were measured.
    constexpr double speechFrameLevelDb = 3.0;        // 99th percentile change in 20 ms frame level, in against out at instant speed
    constexpr double speechJumpRatio = 2.0;           // biggest sample to sample step, out against in: a click would show here

    // Retune Speed: measured 63% time against the set value
    constexpr double retuneToleranceFraction = 0.20;
    constexpr double retuneToleranceMs = 5.0;         // the bigger of the two applies: measuring pitch needs a few periods

    // Formants at +/-3 semitones of correction
    constexpr double formantShiftFraction = 0.05;
    constexpr double levelChangeDb = 1.5;             // loudness must stay put when correcting +/-3 semitones
    constexpr double noteStartOverDb = 3.0;           // a burst at a note start, output against input. An untouched 110 Hz note reads up to 2.2 here

    // Vibrato (input +/-40 cents at 5.5 Hz unless stated)
    constexpr double vibratoKeptFlex = 0.85;          // kept with Flex Amount 60 cents
    constexpr double vibratoKeptHumanize = 0.70;      // kept with Humanize 100, on the sustained part of the note
    constexpr double vibratoKeptSlowRetune = 0.70;    // kept with Retune Speed 400 ms
    constexpr double vibratoFlatHard = 0.10;          // left at hard settings

    // Hard step between notes
    constexpr double hardStepMs = 15.0;               // 10% to 90% of the step, with Retune Speed 0

    // Unvoiced sounds
    constexpr double unvoicedNullDb = -80.0;          // output against the delayed input, noise only material
    constexpr double consonantNullDb = -30.0;         // the middle of an S between two corrected vowels

    // Artifacts on a corrected sustained tone
    constexpr double nonHarmonicDb = -35.0;           // non-harmonic energy against total, 220 Hz +30 cents, corrected
    constexpr double warblePitchCents = 2.0;          // standard deviation of output pitch, sustained tone, corrected
    constexpr double warbleLevelDb = 1.0;             // peak to peak output level in 50 ms frames, sustained tone, corrected

    // Latency
    constexpr double lowestLatencyMs = 10.0;          // lowest latency Input Type at 48 kHz

    // CPU, percent of one core, 48 kHz stereo, voiced material
    constexpr double cpuPercent = 5.0;

    // Added controls
    constexpr double transposeCents = 10.0;           // output against the transposed target, median
    constexpr double referenceCents = 5.0;            // A4 = 432 target against the output, median
    constexpr double naturalVibratoLess = 0.45;       // Natural Vibrato -100 leaves at most this much
    constexpr double naturalVibratoMore = 1.6;        // Natural Vibrato +100 gives at least this much
    constexpr double throatFormantMinMove = 0.10;     // Throat Length 130% moves the first formants down at least 10%
    constexpr double formantOffMinMove = 0.10;        // Formant Preserve off, +3 semitones, formants move up at least 10%
    constexpr double flexHoldCents = 8.0;             // a note 30 cents sharp with Flex 50 stays where it was sung, within this
    constexpr double flexClampCents = 10.0;           // a note 80 cents sharp with Flex 50 ends 50 cents sharp, within this
    constexpr double customNoteCents = 10.0;          // custom scale Remove and Bypass, output against the expected pitch
    constexpr double trackingChoosyPercent = 20.0;    // Tracking 0 corrects at most this share of a noisy voice
    constexpr double trackingRelaxedPercent = 70.0;   // Tracking 100 corrects at least this share of the same voice
}

// Smooth Tune Pro. Set before any Tune Pro DSP was written. Everything above carries over from Smooth Tune and is applied to both engines
// unless a test says otherwise. Never loosen one of these to get a pass: if a limit is truly wrong, the reason goes next to it and into the report.
namespace prolimits
{
    // Formants. Smooth Tune measured 4.98% and 11.6 dB, so the new numbers must beat those where they matter.
    constexpr double hqFormantMove = 0.04;            // High Quality, +/-1 and +/-3 semitones, output against input, worst move
    constexpr double llFormantMove = 0.05;            // Low Latency, same test
    constexpr double hqF1Flatter = 7.0;               // dB: first formant flatter than a real voice at that pitch, worst case, High Quality
    constexpr double llF1Flatter = 11.6;              // dB: Low Latency must at least not be worse than Smooth Tune
    // Changed after measuring, because the first version could not be met by anything. It read the output against the input, at 8%, but on high notes the
    // input and a real voice with the same vocal tract already read 14.9% apart: the tool has no better resolution there. Now: no more than 2% beyond that floor.
    constexpr double highNoteMoveOverFloor = 0.02;    // High Quality, voice around 440 and 660 Hz, +/-3 semitones
    // Same reason, floor 10.5%. Kept at 8% but read against a real voice at the new pitch, which is the goal.
    constexpr double bigShiftMove = 0.08;             // High Quality, voice around 165 and 247 Hz, +/-7 semitones
    constexpr double highNoteF1Flatter = 9.0;         // dB, High Quality, high notes and big shifts
    constexpr double hqLevelChangeDb = 1.5;           // level at the same shifts as above, High Quality
    // Added for version 1.3, before any of its DSP. A peak that overshoots a real voice is as wrong as a flat one, and could otherwise pass the limit above.
    constexpr double highNoteF1Sharper = 6.0;         // dB, High Quality, high notes
    // Held out voices, 392 to 740 Hz, held notes, each harmonic against a real voice with the same vocal tract at the new pitch, mean RMS. 1.2.1 reads 4.59 dB here, so this asks for over 1 dB better.
    constexpr double heldNoteHarmonicMeanDb = 3.5;

    // Engines
    constexpr double modeSwitchStepRatio = 2.0;       // biggest sample step during an engine switch against the same voice's own
    constexpr double modeSwitchHoleDb = 12.0;         // deepest fall of 5 ms level during a switch, worst of several pitches
    constexpr double hqOnsetOnPitchShare = 0.80;      // share of 10 to 40 ms after a note starts that is within 10 cents, High Quality
    constexpr double hqConsonantStartNullDb = -20.0;  // first 10 ms of an S straight after a corrected vowel, High Quality
    constexpr double hqCpuPercent = 8.0;              // one core, 48 kHz stereo
    constexpr double llCpuPercent = 5.0;

    // MIDI targets
    constexpr double midiTargetCents = 5.0;           // output against a held note, 95th percentile
    constexpr double midiNoNotesNullDb = -60.0;       // no notes held, MIDI Targets on: output against the delayed input
    constexpr double midiReleaseMs = 60.0;            // correction gone this soon after the last note off, at Retune Speed 0

    // Per note control
    constexpr double perNoteCents = 5.0;              // Remove and Bypass, output against the expected pitch

    // Classic
    constexpr double classicStepMs = 8.0;             // 10% to 90% of a step between two notes
    constexpr double classicAgainstRetuneRatio = 0.5; // must be at most this share of the same step at Retune Speed 20

    // Transpose, tested from -24 to +24
    constexpr double transposeCents = 5.0;

    // Throat
    constexpr double throatPitchCents = 5.0;          // pitch must not move
    constexpr double throatScaleError = 0.05;         // formant scale against the setting, as a fraction

    // Vibrato generator, added after checking what the original offers
    constexpr double vibratoDepthError = 0.15;        // measured depth against the setting
    constexpr double vibratoRateError = 0.05;         // measured rate against the setting
    constexpr double vibratoDelayErrorMs = 60.0;      // when it reaches half depth against Vibrato Delay plus half its fade

    // Mix
    constexpr double mixDryNullDb = -120.0;           // Mix 0 against the delayed input
    constexpr double mixHalfLevelDb = 1.5;            // Mix 50 on a voice with nothing to correct: no level change, so dry and wet line up

    // Timing, added 2026-11-11 after two faults found on real takes. Both limits are the real vocal tool's (realvox tune-pro.cpp, d95cdff).
    constexpr double pitchInLineMs = 1.0;             // pitch movement against the sound it belongs to, either way
    constexpr double uncorrectedNullDb = -90.0;       // an uncorrected voice against the input at the reported delay, once settled
    // A note sung on the line between two notes, added 2026-11-11 from real takes. Set before the rule was run.
    constexpr double lineWaverCents = 20.0;           // the output's waver may not be bigger than the +/-20 cents that were sung
    constexpr double lineSettleCents = 15.0;          // the output's median from the nearer note: it has settled on one
    constexpr double flatStepMs = 20.0;               // a semitone step landing 25 cents flat reaches the new note no later than in 1.5.1 (10 ms, 4 in High Quality) plus 10
    constexpr double flatGlideStepMs = 14.0;          // the same step as a 60 ms glide: 1.5.1 reads 4 ms after the glide ends (2 in High Quality), plus 10
}
