#pragma once
// Pass limits for Smooth Harmony. Committed before any DSP existed, so no limit was chosen by looking at a result.
// If one of these ever changes, the reason goes next to it and into the report.
namespace limits
{
    // Voice pitch against its target, 95th percentile of frames in the body of a note. All three modes, all 24 keys.
    constexpr double voicePitchCents = 5.0;

    // Formants at +/-7 semitones, moved by this fraction at most. The task's number, and the known weak spot.
    constexpr double formantShiftFraction = 0.05;
    constexpr double voiceLevelAgainstLeadDb = 1.5;   // a voice at Level 0 dB against the lead, at +/-7 semitones, long term

    // Level and Pan
    constexpr double levelDb = 0.2;                   // Level -12 and -6 and +6 against Level 0
    constexpr double leadLevelDb = 0.1;               // Lead Level on the lead
    constexpr double panCentreDb = 0.1;               // left against right at Pan 0
    constexpr double panHardDb = -60.0;               // the far channel at Pan +/-100, against the near one
    constexpr double panLawDb = 0.3;                  // Pan +50, each channel against the constant power law

    // Drift
    constexpr double driftOffCents = 5.0;             // Drift 0: the same limit as voice pitch
    constexpr double driftFullMinCents = 10.0;        // Drift 100: biggest pitch move over 30 s is at least this
    constexpr double driftFullMaxCents = 25.0;        // and at most this
    constexpr double driftFullMinMs = 15.0;           // Drift 100: spread of timing over 30 s is at least this
    constexpr double driftFullMaxMs = 45.0;           // and at most this
    constexpr double driftOffMs = 2.0;                // Drift 0: a unison voice sits on the lead within this
    constexpr double driftScaleTolerance = 0.35;      // Drift 50 moves half of what Drift 100 does, within this fraction

    // Separate singers. Two voices on the same interval, the worst case, at default Drift and Humanize.
    constexpr double copyBestLagCorrelation = 0.75;   // median over 1 s windows, best lag within +/-50 ms
    constexpr double copyZeroLagCorrelation = 0.50;   // median over 1 s windows, no lag allowed
    constexpr double copySumRiseDb = 4.5;             // level rise when the second voice is added: 6 is a copy, 3 is unrelated

    // Clicks. Biggest sample step and biggest second difference, against the same voices on a steady note.
    constexpr double clickJumpRatio = 2.0;
    constexpr double clickCurveRatio = 2.5;

    // Consonants through the voices
    constexpr double consonantShapeDb = 3.0;          // voice spectrum against the lead in thirds of an octave, 2 to 12 kHz, RMS difference
    constexpr double consonantPeriodicityRise = 0.10; // best correlation at lags of 2 to 15 ms, voice against lead: buzz would raise it
    constexpr double consonantLevelDb = 1.5;          // Consonants 35 sets the level within this, against -9.1 dB
    constexpr double consonantVoicesOffDb = -80.0;    // Consonants 0: what is left of the noise in the voices

    // Silent voices
    constexpr double nullDb = -100.0;                 // MIDI mode with no notes, and Mix 0, against the delayed input

    // Timing against the lead
    constexpr double voiceOnsetMs = 10.0;             // onset of a unison voice against the onset of the lead, Drift 0
    constexpr double midiChangeMs = 15.0;             // a chord change reaches the output this close to when it was played, plus latency

    // Latency
    constexpr double defaultLatencyMs = 15.0;         // default Input Type at 48 kHz

    // CPU, percent of one core, 48 kHz stereo, 4 voices, all moving
    constexpr double cpuPercent = 10.0;

    // Added controls
    constexpr double vibratoDepthFraction = 0.25;     // measured depth against the set depth
    constexpr double vibratoRateFraction = 0.05;      // measured rate against the set rate, per voice spread included
    constexpr double humanizeScoopMinCents = 15.0;    // Humanize 100: average pitch dip at the start of a note is at least this
    constexpr double humanizeOffCents = 5.0;          // Humanize 0: the same place on the note stays on target
    constexpr double humanizeBodyCents = 10.0;        // Humanize 100: the body of the note stays this close to the target
    constexpr double glideFraction = 0.25;            // measured 63% time against Glide
    constexpr double glideMs = 8.0;                   // the bigger of the two applies: measuring pitch needs a few periods
    constexpr double throatMinMove = 0.10;            // Throat 130 moves formants down, Throat 75 up, at least this
    constexpr double throatRead = 0.05;               // and within this of the exact move
    constexpr double followLeadCents = 5.0;           // Follow Lead 0 stays on the note, 100 copies a sharp lead
    constexpr double harmonyLevelDb = 0.2;            // Harmony Level on all voices together

    // Real material
    constexpr double speechFrameLevelDb = 6.0;        // 99th percentile change in 20 ms frame level, lead plus voices, against the level the lead alone would make with the same gain
    constexpr double speechJumpRatio = 2.5;           // biggest sample step in the voices, against the input
    constexpr double speechMedianCents = 20.0;        // fixed interval on real speech: median distance from lead plus interval
}
