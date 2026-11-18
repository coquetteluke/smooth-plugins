#pragma once
// Pass limits for Smooth Tune FX. Written and committed before the plug-in's DSP, so no limit was chosen by looking at a result.
// If one of these ever changes, the reason goes next to it and into the report.
namespace limits
{
    // Tuning at default settings (hard)
    constexpr double hardTuneCents = 5.0;           // the task's limit: 95th percentile of frames, default settings
    constexpr double speechOutputMedianCents = 12.0; // say speech at default settings, median distance from the nearest note
    constexpr double sungOutputMedianCents = 8.0;    // sung-style say material at default settings

    // MIDI target notes
    constexpr double midiCents = 5.0;               // output against the held note, 95th percentile, voice up to 7 semitones away
    constexpr double midiTimingMs = 15.0;           // pitch reaches the new held note this close to the note's place in the audio
    constexpr double midiReleaseCents = 5.0;        // after note off, back to the nearest scale note

    // Tuning controls, same limits as Smooth Tune
    constexpr double retuneToleranceFraction = 0.20; // measured time constant against Retune Speed
    constexpr double retuneToleranceMs = 5.0;        // the bigger of the two applies
    constexpr double vibratoKeptHumanize = 0.70;     // Humanize 100 keeps at least this share of a vibrato
    constexpr double vibratoFlatHard = 0.10;         // default settings leave at most this much
    constexpr double referenceCents = 5.0;           // Reference A4 432
    constexpr double customNoteCents = 10.0;         // Custom scale notes
    constexpr double trackingChoosyPercent = 20.0;   // Tracking 0 calls a noisy voice pitched at most this share of the time
    constexpr double trackingRelaxedPercent = 70.0;  // Tracking 100 at least this share

    // Modules: on, off and main control
    constexpr double pitchCents = 5.0;              // Octave and Semitones, output against the wanted pitch, 95th percentile
    constexpr double pitchFormantSmallMove = 0.08;  // formants held at +/-5 semitones
    constexpr double pitchFormantOctaveMove = 0.15; // formants held at +/-12 semitones
    constexpr double realFormantMove = 4.43;        // percent, Pitch +/-1 octave on Luke's real takes, measured as the real vocal check does (its limit). Added 2026-11-12
    constexpr double pitchLevelDb = 3.0;            // level of a shifted voice against the same voice unshifted, +/-12 semitones. Added after Harmony read +2.4 dB at +7, before shifts were measured
    constexpr double formantToolFraction = 0.05;    // Throat Length and Pitch Moves Formants, against the exact scale
    constexpr double harmonyCents = 5.0;            // harmony voice against the wanted pitch
    constexpr double harmonyLevelDb = 2.0;          // harmony against the lead, against Harmony Level
    constexpr double doublerDetuneCents = 5.0;      // measured offset of each double against Doubler Detune
    constexpr double doublerTimingFraction = 0.25;  // measured lag against Doubler Timing, it wanders on purpose
    constexpr double doublerLevelDb = 2.0;          // double against Doubler Level
    constexpr double vocoderPitchCents = 5.0;       // the tone against the wanted pitch, at Mix 100
    constexpr double vocoderSubRiseDb = 20.0;       // Octave -1 adds a tone at half the voice's pitch, at least this much over off
    constexpr double vocoderLevelDb = 6.0;          // tone at Mix 100 against the voice's level
    constexpr double distortionCleanThd = 2.0;      // percent, Drive 0
    constexpr double distortionDirtyThd = 20.0;     // percent, Drive 100
    constexpr double distortionLevelDb = 6.0;       // level change on a -18 dBFS tone across Drive
    constexpr double distortionAliasDb = -40.0;     // non-harmonic energy against total at Drive 50, 5.2 kHz tone
    constexpr double filterCutoffDb = 1.0;          // level at the cutoff against -3 dB, at Resonance 0
    constexpr double filterStopDb = -18.0;          // two octaves past the cutoff
    constexpr double sweepMinSwingDb = 12.0;        // a tone's level swing from the sweep at Depth 2 octaves
    constexpr double sweepPeriodFraction = 0.05;    // measured sweep period against the note value
    constexpr double ringSidebandDb = 1.5;          // the two sidebands against each other
    constexpr double ringCarrierLeftDb = -30.0;     // the input tone left over at Ring Mix 100
    constexpr double delayTimeMs = 1.0;             // first echo against the note value, at 120 BPM and another tempo
    constexpr double delayRepeatDb = 0.5;           // one repeat against the feedback setting
    constexpr double reverbRt60Fraction = 0.20;     // RT60 against Reverb Decay
    constexpr double motionCents = 5.0;             // each step of a pattern against the wanted note
    constexpr double motionStepMs = 10.0;           // step time against the note value

    // Switching
    constexpr double switchStepRatio = 1.5;         // biggest sample step while a module switches, against the same step when it is steady on or off
    constexpr double switchStepSlack = 0.003;       // added to the above, so a module that barely changes a sine does not fail on rounding
    constexpr double nullDb = -120.0;               // an off module: never on against settings moved, and on then off, against the same run without it

    // Latency
    constexpr double latencyLagSamples = 1.0;       // measured delay of the direct voice against the reported latency, all on

    // Stereo
    constexpr double monoToStereoDb = 0.01;         // mono in, stereo out, with no module on: left against right

    // CPU, percent of one core, 48 kHz stereo, voiced material, all modules on (machine busy with other builds)
    constexpr double cpuAllOnPercent = 25.0;
}

// Added 2026-11-12, for the faults Tune Pro found on Luke's real takes (Rode NT1, 2026-11-10), in the tuner Glitter shares with it. The
// same values as Tune Pro's prolimits (and Tune's rodelimits), set before any of these tests was run on Glitter.
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
    constexpr double nonHarmonicDb = -35.0;           // non-harmonic energy against total in a corrected voice (Tune's limits::nonHarmonicDb)
}

// Entries into a note that hold their pitch from the start (Tune Pro's testEntries), judged by the real vocal check's own start lag rule
// for Glitter (time until the output is within 10 cents of the note for 15 ms) and its limits, at Retune 0, Glitter's default.
namespace entrylimits
{
    constexpr double instantMedianMs = 20.0, instantP90Ms = 40.0;
}

// Added 2026-11-12, before the tests that use them were run: limits of the real vocal check (realvox src/extra/tune-fx.cpp), brought into
// the suite where the real takes found a fault, so it can be tested and fixed here.
namespace voxlimits
{
    constexpr double distortionLevelDb = 3.0;         // Distortion on a voice: level against off, "within a few dB" (the make-up gain's claim)
}
