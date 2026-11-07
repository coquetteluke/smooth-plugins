#pragma once
// Pass limits for Smooth Key. Written and committed before any accuracy result existed. Do not loosen one just to get a pass.
// The accuracy corpus has two seeds: DEV_SEED was used while building the detector, HELD_OUT_SEED only for the reported numbers.
namespace limits
{
    constexpr unsigned devSeed = 1000u;
    constexpr unsigned heldOutSeed = 20261106u;

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

    // key.json
    constexpr double maxWritesPerSecond = 2.2;       // brief says at most about twice a second
}
