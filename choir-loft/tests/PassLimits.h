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
    constexpr double trackingChoosyPercent = 20.0;    // same limits as Smooth Tune, added when the test material was fixed: Tracking 0 calls at most this share pitched
    constexpr double trackingRelaxedPercent = 70.0;   // Tracking 100 at least this share
    constexpr double harmonyLevelDb = 0.2;            // Harmony Level on all voices together

    // 1.1.0, more human. Set after measuring 1.0.0 (tests/human-before-1.0.txt) and before measuring any of the new code.
    // Humanize 100 now also adds a per note landing and a per voice tendency on purpose, so humanizeBodyCents above went from 10 to the value below.
    constexpr double humanizeBodyCents11 = 20.0;      // Humanize 100: the note itself at 200 ms, 90th percentile
    constexpr double humanLateMinMs = 15.0;           // defaults: voices arrive after the lead by at least this on average (1.0.0: 13)
    constexpr double humanLateMaxMs = 60.0;           // and at most this
    constexpr double humanSpreadMinMs = 15.0;         // defaults: earliest to latest voice on a note change, median (1.0.0: 7)
    constexpr double humanOffMinCents = 3.0;          // defaults: distance from target in the body of a note, median, at least (1.0.0: 2.7)
    constexpr double humanOffMaxCents = 10.0;         // and at most: never out of tune
    constexpr double humanOffP95Cents = 20.0;         // 95th percentile at most (1.0.0: 7.0)
    constexpr double humanApartCents = 4.0;           // voices settle apart, highest average minus lowest, at least (1.0.0: 1.1)
    constexpr double humanLockstep = 0.40;            // correlation of pitch moves, most alike pair, at most (1.0.0: 0.68)
    constexpr double humanFormantGapPercent = 15.0;   // widest formant gap between two voices, at least (1.0.0: 11.8)
    constexpr double humanFormantGapMaxPercent = 28.0;// and at most: different people, not different species
    constexpr double humanBrightGapDb = 6.0;          // widest brightness gap between two voices, at least (1.0.0: 4.5)
    constexpr double humanSSpreadMs = 15.0;           // spread of when an S starts across voices, median, at least (1.0.0: 0)
    constexpr double humanSSame = 0.40;               // how alike two voices' S are at the best lag, median, at most (1.0.0: 0.86)
    constexpr double humanSLevelDb = -14.0;           // a voice's S against the lead above 3.5 kHz, at most (1.0.0: -9.7)
    constexpr double humanPile4Db = 0.35;             // say speech S and T with 4 voices against the lead alone, at most (1.0.0: 0.34)
    constexpr double humanSamenessDefaults = 0.95;    // period to period sameness above 3 kHz at +7, defaults, at most (1.0.0: 0.996, its lead 0.993)
    // Was: the grain pattern at -7 improves 3 dB. Dropped after measuring: the test lead's own energy between harmonics sits at -29.7 dB and 1.0.0 at -28.2,
    // so 3 dB is below what is in the lead. Now: with period blending no shift comes out above the lead's own level by more than this.
    constexpr double humanPatternOverLeadDb = 0.5;
    constexpr double driftLevelMinDb = 1.5;           // Drift 100: level wander, peak to peak in 100 ms frames, at least
    constexpr double driftLevelMaxDb = 4.0;           // and at most
    constexpr double breathFadeDb = -6.0;             // a long stretch without pitch, a breath, ends at least this much lower than it started, at defaults
    constexpr double breathFullLowDb = -18.0;         // Breath 100: noise against the voice above 3 kHz in a held vowel, at least
    constexpr double breathFullHighDb = -6.0;         // and at most
    constexpr double toneFullDb = 4.0;                // Voice Tone +/-100: brightness moves at least this

    // Tight preset keeps the 1.0.0 limits: voicePitchCents and driftOffMs above, measured through the preset itself.
    constexpr double tightSpreadMs = 8.0;             // earliest to latest voice on a note change, median, at most

    // Real material
    // Checked on the Tight preset. At the new defaults voices run on behind a lead that has stopped, so frames with the lead nearly silent read high on purpose: reported only.
    constexpr double speechFrameLevelDb = 6.0;        // 99th percentile change in 20 ms frame level, lead plus voices, against the level the lead alone would make with the same gain
    constexpr double speechJumpRatio = 2.5;           // biggest sample step in the voices, against the input
    constexpr double speechMedianCents = 20.0;        // fixed interval on real speech: median distance from lead plus interval

    // More singers and click to hear, set before any of it was measured. New voices use the same limits as the first four, above.
    constexpr double hearFadeMinMs = 10.0, hearFadeMaxMs = 20.0;   // a singer switching on or off, the brief asks for 10 to 20
    constexpr double hearOffDb = -120.0;                           // an off singer, once faded
    constexpr double muteNullDb = -90.0;                           // an off singer against the same singer at Level Off
    constexpr double comeBackCents = 15.0;                         // a voice coming back, 50 ms later, against where it should be
    constexpr double trimDb = 0.05;                                // the bus trim at 5 to 8 voices against its formula
    constexpr double eightAgainstFourDb = 1.5;                     // eight voices at one Level against four at that Level, RMS
    constexpr double newPresetPeak = 1.0;                          // new presets, input peaking at -6 dBFS: output peak under this
    constexpr double offVoiceCpuShare = 0.25;                      // CPU of 6 off out of 8, as a share of their cost when on

    // 1.4.0, phrase starts. Luke: "choir loft misses when vocals start pretty often". Set from what a listener hears, written down and
    // committed before measuring the 1.3.1 build on them. See tests/OnsetTests.h for exactly how each is measured.
    // A voice has sounded once it is within onsetOnNoteCents of its note and within onsetLevelDb of its own steady level against the lead,
    // and stays so for onsetHoldMs. Times run from when the lead is on its note (for a scoop, when it is within 50 cents of it).
    constexpr double onsetOnNoteCents = 50.0;
    constexpr double onsetLevelDb = -3.0;
    constexpr double onsetHoldMs = 20.0;
    constexpr int onsetMisses = 0;                    // first notes of 150 ms or longer where a voice never sounded, any kind of start
    constexpr double onsetSoundP95Ms = 40.0;          // time to sound past that voice's own lag, 95th percentile per kind of start
    constexpr double onsetSoundWorstMs = 80.0;        // and any single start: past this a listener hears the voice skip the note
    constexpr int onsetWrongNotes = 0;                // a voice singing another note first, audibly, before it reaches its own
    constexpr double onsetWrongCents = 80.0;          // counts as another note when this far from its note, from the lead and from the lead's scoop
    constexpr double onsetWrongMs = 20.0;             // for at least this long
    constexpr double onsetWrongAudibleDb = -12.0;     // and no quieter than this against its steady level
    constexpr double onsetCorpusSounded = 0.90;       // macOS say material: share of voiced stretches of 150 ms or longer where each voice sounds
    constexpr double onsetCorpusMedianMs = 40.0;      // and the median time to sound past the lag there
    // Things that are not singing: breaths, s and sh, noise, room tone. With Consonants 0 a voice is silent unless it thinks it hears a note.
    constexpr double noTriggerShare = 0.01;           // share of 10 ms frames where a voice sings (over -20 dB against the input), at most
    constexpr double noTriggerRunMs = 20.0;           // and never longer than this at once
    // 1.5.1. A word on a note of 130 Hz or more that comes straight out of a creak an octave down (tests/AfterCreak.h): once the note is
    // plain, the tracker may read it an octave low for no longer than a wrong note is allowed to last at any start (onsetWrongMs).
    constexpr double afterCreakLowMs = 20.0;
}
