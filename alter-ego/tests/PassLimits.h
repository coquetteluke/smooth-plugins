#pragma once
// Pass limits for Smooth Morph. Written and committed before any DSP existed, so no limit was chosen by looking at a result.
// If one of these ever changes, the reason goes next to it and into the report.
namespace limits
{
    // Pitch control, synthetic voices, +/-12 semitones
    constexpr double pitchCents = 5.0;                // 95th percentile of frames, each note and shift, the task's limit
    constexpr double pitchFormantFraction = 0.05;     // formant shape scale against the input, the task's "about 5%"
    constexpr double pitchLevelDb = 2.0;              // level against the input

    // Formant Shift control
    constexpr double formantRatioFraction = 0.05;     // measured scale against 2^(st/12)
    constexpr double formantPitchCents = 2.0;         // 95th percentile, the task's limit

    // Characters. Pitch and formant moves are the values stated in Characters.h
    constexpr double characterPitchCents = 5.0;
    constexpr double characterFormantFraction = 0.05;
    constexpr double robotPitchCents = 5.0;           // output stays this close to the set note, 95th percentile, on a voice gliding and with vibrato
    constexpr double robotPeakGainDb = 1.5;           // formant peaks at least this much taller against the valleys than Natural
    constexpr double monsterSubharmonicRiseDb = 15.0; // energy at half the output pitch, rise against Natural at the same pitch
    constexpr double monsterJitterPercent = 2.0;      // cycle to cycle change in period, at least
    constexpr double monsterShimmerDb = 1.0;          // cycle to cycle change in level, at least
    constexpr double monsterPitchCents = 15.0;        // median pitch, Monster shakes on purpose so it is the median that counts

    // Vibrato
    constexpr double vibratoFraction = 0.10;          // rate and depth, the task's limit

    // Whisper at 100
    constexpr double whisperCombDb = 1.5;             // energy on the harmonics against between them, voice at +20 dB or more going in
    constexpr double whisperPeriodicity = 0.10;       // normalised autocorrelation at the pitch lag of the whitened output
    constexpr double whisperFormantFraction = 0.05;   // formant shape scale against the input
    constexpr double whisperShapeDb = 4.0;            // RMS difference of 300 Hz smoothed spectrum shape, 200 Hz to 5 kHz
    constexpr double whisperLevelDb = 3.0;            // level against the input

    // Mix
    constexpr double mixNullDb = -100.0;              // Mix 0 against the input delayed by the reported latency
    constexpr double neutralNullDb = -80.0;           // default settings at Mix 100, and Mix 50, against the same
    constexpr double mixBlendDb = 0.5;                // Mix 50 of a shifted voice: dry part is 6 dB down, within this

    // Clicks. A change of Character or Pitch during a sustained voice
    constexpr double clickStepRatio = 2.0;            // biggest sample to sample step against the worst of the steady states either side
    constexpr double clickHighBandRiseDb = 6.0;       // 5 ms frame level above 8 kHz against the worst steady frame

    // Consonants
    constexpr double fricativeDifferenceDb = -60.0;   // middle of an S between shifted vowels, against the delayed input, formants unchanged
    constexpr double fricativeFlatnessRatio = 0.85;   // spectral flatness in the S, out over in, at the worst setting tried: tones would drop it
    constexpr double speechHissLevelDb = 3.0;         // real speech: hiss band level in unvoiced frames, 95th percentile change, formants unchanged
    constexpr double speechFormantFraction = 0.05;    // real speech: long term spectrum shape against the input, Pitch +/-7
    constexpr double speechPitchCents = 15.0;         // real speech: median pitch of the output against the input's plus the shift

    // Latency
    constexpr double latencyMaxMs = 30.0;             // at every rate
    constexpr int latencyErrorSamples = 0;            // correlation peak against the reported figure

    // Range Match
    constexpr double rangeMatchSemitones = 1.0;       // median output pitch against the set centre, input anywhere over two octaves

    // CPU
    constexpr double cpuPercent = 15.0;               // of one core, 48 kHz stereo, machine busy with other builds
    constexpr double cpuPercent96 = 30.0;             // the same at 96 kHz
}
