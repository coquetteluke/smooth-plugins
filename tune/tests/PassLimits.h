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

// Added 2026-11-12, for the faults Tune Pro found on Luke's real takes (Rode NT1, 2026-11-10), in the engine Tune shares with it. The
// same values as Tune Pro's prolimits, set before any of these tests was run on Tune.
namespace rodelimits
{
    constexpr double pitchInLineMs = 1.0;             // pitch movement against the sound it belongs to, either way
    constexpr double uncorrectedNullDb = -90.0;       // an uncorrected voice against the input at the reported delay, once settled
    constexpr double lineWaverCents = 20.0;           // the output's waver may not be bigger than the +/-20 cents that were sung
    constexpr double lineSettleCents = 15.0;          // the output's median from the nearer note: it has settled on one
    constexpr double flatStepMs = 20.0;               // a semitone step landing 25 cents flat reaches the new note within this
    constexpr double flatGlideStepMs = 14.0;          // the same step as a 60 ms glide, from the end of the glide
    constexpr double slowSlideMs = 150.0;             // a held note sliding slowly across the line (13 cents per 100 ms) is on the new note within 150 ms of the line
    constexpr double beforeOnsetDb = -40.0;           // the output in the 10 ms before a note starts in the input, against the held note
}

// Added 2026-11-12, before the test was run: entries into a note that hold their pitch from the start (Tune Pro's testEntries), judged by
// the real vocal check's own start lag rule for Tune (time until the output is within 10 cents of the note for 15 ms) and its limits.
namespace entrylimits
{
    constexpr double instantMedianMs = 20.0, instantP90Ms = 40.0;   // Retune Speed 0
    constexpr double defaultMedianMs = 60.0, defaultP90Ms = 100.0;  // the default, Retune 20 and Humanize 30
}
