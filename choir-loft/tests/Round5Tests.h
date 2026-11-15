#pragma once
// Round 5 (1.6.2): round 1's last low-note fault, take 10 at 41.242 s. Written to fail on 1.6.1 and pass after. Included by
// PluginTest.cpp inside its namespace, after Round3Tests.h. Run alone: PluginTest round5. Probe: PluginTest r5probe.

namespace round5
{
using namespace phase2;

// What the tracker reads on take 10 at 41.242 s: a rough low start read right (100 Hz) for most of the start's first 30 ms, then, in the
// last of those ms, an octave high (200 Hz) for 16 ms, the pitch lost for 6 ms, and the note found again where it was (104 Hz). Those first
// 30 ms take the newest reading back over the whole stretch, so the 16 ms high were written over the 30 ms read right before them, and a
// voice standing 22 ms behind sang the whole stretch an octave up.
// Here: a low vowel, the same vowel an octave up for a moment, a short gap, the low vowel again. Four voices with Drift, so they stand
// from a few ms to about 25 ms behind.
struct BlipRun
{
    std::vector<double> hz;                        // the lead as the tracker reads it, one value per ms
    std::array<std::vector<int>, 4> note;          // each voice's note
    std::array<std::vector<float>, 4> lag;
};

inline BlipRun blipRun (double lowSeconds, double highSeconds, double gapSeconds, double backHz, double drift)
{
    // One low vowel throughout. For a moment every other period of it is made the same: the vowel plus itself half a period later repeats
    // every half period, an octave up with no break in the sound (on a rough start the odd harmonics drop away). Then it comes back down
    // over gapSeconds while the vowel moves to backHz, and the tracker loses it for a few ms.
    const double tail = 0.5;
    auto f0 = voice::steady (100.0, lowSeconds + highSeconds, SR);
    for (int i = 0; i < (int) ((gapSeconds + tail) * SR); ++i)
        f0.push_back (100.0 + (backHz - 100.0) * std::min (1.0, i / (gapSeconds * SR + 1.0)));
    const auto base = voice::synth (f0, voice::vowelA, SR, -60.0, -18.0);
    const int half = (int) std::lround (SR / 200.0), lowEnd = (int) (lowSeconds * SR), end = (int) ((lowSeconds + highSeconds) * SR);
    std::vector<float> sung (base.size() - (size_t) half);
    for (int i = 0; i < (int) sung.size(); ++i)
    {
        const double a = i < end ? juce::jlimit (0.0, 1.0, (i - lowEnd) / (0.002 * SR)) : juce::jlimit (0.0, 1.0, 1.0 - (i - end) / (gapSeconds * SR + 1.0));
        sung[(size_t) i] = (float) ((base[(size_t) i + (size_t) half] + a * base[(size_t) i]) / (1.0 + a));
    }
    const auto x = join ({ zeros (0.3), sung, zeros (0.2) });
    auto p = clean (modeScale, 4);
    for (int v = 0; v < 4; ++v) setParam (*p, vid (v, "Drift"), (float) drift);
    setParam (*p, "consonants", 0.0f);
    prepare (*p, 1, SR, 48);
    BlipRun r;
    const int block = 48;   // 1 ms
    juce::AudioBuffer<float> buf (1, block);
    juce::MidiBuffer midi;
    for (int pos = 0; pos + block <= (int) x.size(); pos += block)
    {
        std::copy_n (x.data() + pos, block, buf.getWritePointer (0));
        p->processBlock (buf, midi);
        const auto rd = p->getReadings();
        r.hz.push_back (rd.detectedHz);
        for (int v = 0; v < 4; ++v) { r.note[(size_t) v].push_back (rd.voiceNote[(size_t) v]); r.lag[(size_t) v].push_back (rd.voiceLagMs[(size_t) v]); }
    }
    return r;
}

// The ms the tracker reads the lead at 150 Hz or more, and per voice the longest run of ms on a note 10 or more semitones above the note it
// sings for the low vowel (its note 100 ms into the last low vowel)
struct BlipCount { int trackerHigh = 0, trackerGap = 0; std::array<int, 4> voiceHigh {}; std::array<float, 4> lag {}; };

inline BlipCount countBlip (const BlipRun& r, double lowSeconds, double highSeconds, double gapSeconds)
{
    BlipCount c;
    const int from = 300, to = (int) r.hz.size();
    int lastHigh = -1;
    for (int t = from; t < to; ++t)
        if (r.hz[(size_t) t] >= 150.0f) { ++c.trackerHigh; lastHigh = t; }
    for (int t = lastHigh + 1; lastHigh >= 0 && t < to && r.hz[(size_t) t] <= 0.0f; ++t) ++c.trackerGap;
    const int lowNoteAt = juce::jmin (to - 1, (int) (1000.0 * (0.3 + lowSeconds + highSeconds + gapSeconds + 0.1)));
    for (int v = 0; v < 4; ++v)
    {
        const int low = r.note[(size_t) v][(size_t) lowNoteAt];
        int run = 0, best = 0;
        for (int t = from; t < to; ++t)
        {
            run = (low >= 0 && r.note[(size_t) v][(size_t) t] >= low + 10) ? run + 1 : 0;
            best = std::max (best, run);
        }
        c.voiceHigh[(size_t) v] = best;
        c.lag[(size_t) v] = r.lag[(size_t) v][(size_t) lowNoteAt];
    }
    return c;
}

inline void probeBlip()
{
    for (const double low : { 0.015, 0.02, 0.025, 0.03 })
        for (const double high : { 0.012, 0.016 })
          for (const double gap : { 0.001, 0.003, 0.008 })
        {
            if (const char* only = std::getenv ("R5ONLY")) if (std::abs (std::atof (only) - (low * 1.0e6 + high * 1.0e3 + gap)) > 1.0e-6) continue;
            const auto r = blipRun (low, high, gap, 104.0, 100.0);
            const auto c = countBlip (r, low, high, gap);
            std::printf ("low %2.0f ms high %2.0f ms gap %1.0f ms: tracker high %3d ms then none %2d ms, voices high %3d %3d %3d %3d ms (lags %4.1f %4.1f %4.1f %4.1f)\n", low * 1000.0, high * 1000.0, gap * 1000.0,
                         c.trackerHigh, c.trackerGap, c.voiceHigh[0], c.voiceHigh[1], c.voiceHigh[2], c.voiceHigh[3], c.lag[0], c.lag[1], c.lag[2], c.lag[3]);
            if (std::getenv ("R5TRACE") != nullptr)
                for (int t = 300; t < juce::jmin ((int) r.hz.size(), 420); ++t)
                    std::printf ("  %4d ms lead %6.1f notes %3d %3d %3d %3d\n", t - 300, r.hz[(size_t) t], r.note[0][(size_t) t], r.note[1][(size_t) t], r.note[2][(size_t) t], r.note[3][(size_t) t]);
        }
}

inline void testOctaveBlip()
{
    // The fault's shape: the tracker reads 100 Hz, then 200 Hz for under 20 ms inside the start's first 30 ms, then nothing for under 10 ms,
    // then 104 Hz. Four voices at Drift 100 stand 21 to 25 ms behind. Until the pitch comes back it cannot be known that the octave was the
    // tracker's, so a voice behind sings it from the jump to the return at most. On 1.6.1 it sang it for the whole stretch, the 20 ms read
    // right before the jump as well, and its own lag after the jump.
    const double low = 0.02, high = 0.016, gap = 0.008;
    const auto c = countBlip (blipRun (low, high, gap, 104.0, 100.0), low, high, gap);
    int worst = 0;
    for (int v = 0; v < 4; ++v) worst = std::max (worst, c.voiceHigh[(size_t) v]);
    check (c.trackerHigh > 0 && c.trackerHigh < 20 && c.trackerGap > 0 && c.trackerGap <= 10,
           "round 5 (guard): the tracker reads the octave under 20 ms (" + std::to_string (c.trackerHigh) + ") and then nothing for 10 ms or less (ms)", c.trackerGap);
    check (worst <= c.trackerHigh + c.trackerGap, "round 5: the longest a voice standing behind sings an octave above the low note (ms, no more than from the jump to the return, "
           + std::to_string (c.trackerHigh + c.trackerGap) + ")", worst);

    // A real leap: the same start, and the octave held. Every voice goes up with it and stays.
    const auto leap = countBlip (blipRun (low, 0.3, gap, 104.0, 100.0), low, 0.3, gap);
    int shortest = 1 << 20;
    for (int v = 0; v < 4; ++v) shortest = std::min (shortest, leap.voiceHigh[(size_t) v]);
    check (shortest >= 250, "round 5 (guard): an octave leap held 300 ms, the shortest any voice sings it (ms, 250 or more)", shortest);

    // The pitch comes back somewhere else, a fifth under where it began: not the tracker's octave, so a voice behind still sings the
    // stretch as read, as on 1.6.1.
    const auto other = countBlip (blipRun (low, high, gap, 66.7, 100.0), low, high, gap);
    int longest = 0;
    for (int v = 0; v < 4; ++v) longest = std::max (longest, other.voiceHigh[(size_t) v]);
    check (longest > other.trackerHigh + other.trackerGap, "round 5 (guard): back somewhere else after the gap, the longest a voice behind sings the octave (ms, more than from the jump to the return, "
           + std::to_string (other.trackerHigh + other.trackerGap) + ")", longest);
}
} // namespace round5

void testRound5()
{
    round5::testOctaveBlip();
}
