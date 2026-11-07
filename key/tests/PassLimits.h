#pragma once
// Pass limits for Smooth Key. Written and committed before any accuracy result existed. Do not loosen one just to get a pass.
// The accuracy corpus has two seeds: DEV_SEED was used while building the detector, HELD_OUT_SEED only for the reported numbers.
namespace limits
{
    constexpr unsigned devSeed = 1000u;
    // Earlier held out seeds (20261106, then 20261107) were run once each and showed misses. The detector was then changed using
    // the dev seed only. 1.0.0 and 1.1.0 were reported on a third seed, 20261108. For 1.2.0 the detector was changed using dev
    // seeds 1000 to 1002 only, and the reported numbers come from this fourth seed, run once at the end. No limit was changed.
    constexpr unsigned heldOutSeed = 20261109u;

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

    // key.json
    constexpr double maxWritesPerSecond = 2.2;       // brief says at most about twice a second
}
