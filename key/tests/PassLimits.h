#pragma once
// Pass limits for Smooth Key. Written and committed before any accuracy result existed. Do not loosen one just to get a pass.
// The accuracy corpus has two seeds: DEV_SEED was used while building the detector, HELD_OUT_SEED only for the reported numbers.
namespace limits
{
    constexpr unsigned devSeed = 1000u;
    // Earlier held out seeds (20261106, then 20261107) were run once each and showed misses. The detector was then changed using
    // the dev seed only. 1.0.0 and 1.1.0 were reported on a third seed, 20261108. For 1.2.0 the detector was changed using dev
    // seeds 1000 to 1002 only, and reported on a fourth seed, 20261109. 1.3.0 was tuned on dev seeds 1000 to 1005 only, and the
    // reported numbers came from a fifth seed, 20261110. 1.4.0 was tuned on dev seeds 1000 to 1005 only, checked on unseen dev
    // seeds 1012 to 1017, and reported on a sixth seed, 20261111. 1.5.0 was tuned on dev seeds 1018 to 1023 only, checked on unseen
    // dev seeds 1024 to 1029, and reported on this seventh seed, run once at the end. Earlier seeds still run with PluginTest --seed N.
    // No limit was changed. 1.6.0 (lone voice) was worked out on Luke's working set takes and dev seeds 2000 and 1030, and its new
    // tests report on this same seed plus 21, 22 and 23, which nothing had been run on.
    constexpr unsigned heldOutSeed = 20261112u;

    // Chord progressions in all 24 keys, every timbre, Response Stable, Source Mix, read at the end of the clip.
    constexpr double chordExactPercent = 90.0;
    constexpr double chordRelatedPercent = 98.0;     // exact, relative, or a fifth away in the same scale
    constexpr double chordMedianSettleStable = 8.0;  // seconds until correct and staying correct
    constexpr double chordMedianSettleFast = 5.0;

    // Melody only. Reported as it comes. These floors only catch a broken detector.
    constexpr double melodyExactFloor = 50.0;
    constexpr double melodyRelatedFloor = 80.0;

    // Silence
    constexpr double confidenceAfterTenSecondsSilence = 0.10;

    // Flicker on chord clips, Response Stable, after the first reading settles
    constexpr int stableBounces = 0;                 // A to B and back to A
    constexpr double stableClipsWithAnyChange = 1.0; // percent of clips whose reading moves after 8 s

    // Key change half way through a file: percent followed within 30 s, and median time to follow
    constexpr double keyChangeFollowedPercent = 90.0;
    constexpr double keyChangeMedianStable = 15.0;
    constexpr double keyChangeMedianFast = 8.0;

    // White noise alone must not look like a confident key
    constexpr double noiseConfidence = 0.20;

    // Added with the rule change, before any of these features existed.
    constexpr double referencePitchToleranceHz = 1.0;  // music tuned off A440 reads back within this
    constexpr double tempoToleranceBpm = 1.0;          // half or double of the true tempo also counts as found
    constexpr double tempoFoundPercent = 90.0;
    constexpr double tempoScaleErrorBpm = 0.01;        // Half and Double read exactly half and double
    constexpr double swapPercent = 100.0;              // Relative Swap gives the relative key of the detected one
    constexpr double manualPercent = 100.0;            // Manual mode: all 24 keys reach the readings and the file
    constexpr double manualFileSeconds = 1.5;          // a Manual Key change reaches key.json within this
    constexpr double voiceDroneExact = 80.0;           // Single Voice, melody over a clashing rumble below A2

    // A lone voice. Added on 2026-11-10, before the 1.6.0 reading was run on any of these clips. The singer is made up in
    // KeyCorpus.h (sungClip): every note off by its own amount, scoops, glides, vibrato, drift, no tuning of his own.
    constexpr double sungRightNotesPercent = 90.0;   // clips that end on the right 7 notes (the key or its relative), in each Source
    constexpr double sungMedianSettle = 20.0;        // seconds until the right 7 notes are shown and kept to the end
    constexpr double sungMovingPercent = 10.0;       // clips whose 7 notes change more than once after the first reading
    constexpr double sungReferenceHz = 2.0;          // an off key singer has no tuning of his own: Reference Pitch stays this near 440
    constexpr double sungReferencePercent = 90.0;    // of clips, read at the end
    constexpr double rapSteadyPercent = 90.0;        // rap clips (rapClip) whose reading changes at most once in a minute, or never shows
    constexpr double voiceStepUpPercent = 80.0;      // a tune that moves up a semitone: the new 7 notes within 30 s, Single Voice
    // Added on 2026-11-11 for 1.7.0, before the new key change rule was run on any of them. Both directions: a real change must be
    // followed, and a singer who only strays must not be taken for one.
    // Up two semitones. The limit first written here on 2026-11-11 was "the new 7 notes within 30 s on 80% of clips"
    // (voiceStepUp2Percent). No reader of a lone voice can meet that without also moving when this singer only strays.
    // Proof, rerun with tests/proofs/ideal_reader.py step: a reader with no noise that is told the moment of the change and
    // hears the true notes needs to move at a lead of +0.18 to follow 80% of 192 made up clips, and Luke's take 06, where
    // nothing changes, shows a lead of +0.23 for the same move (a semitone needs +0.49 and his highest is +0.18, so that
    // test stays as written). Replaced by the strictest thing such a reader can do, same number: within 30 s the new 7 notes
    // are shown, or from 30 s after the change the reading is never at confidence 0.5 or more on any other notes, so it says
    // not sure and nothing follows a wrong key. The first wording's number is still printed, as INFO.
    constexpr double voiceStepUp2HonestPercent = 80.0;
    constexpr double sungLeftPercent = 5.0;          // off key singer who stays in key: clips that show the right 7 notes and then leave them
    constexpr double rapConfidentPercent = 5.0;      // rap: share of the time with a reading at confidence 0.5 or more, which followers act on

    // key.json
    constexpr double maxWritesPerSecond = 2.2;       // brief says at most about twice a second
}
