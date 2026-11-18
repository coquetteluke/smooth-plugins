#pragma once
// Phase 2 round 3 (1.6.1): checks written to fail on 1.6.0 and pass after. Included by PluginTest.cpp inside its namespace, after
// Phase2Tests.h. Run alone: PluginTest round3. The ids are the hunt's, in ~/Bug Fixing/smooth-plugins/fix/harmony.json.

namespace round3
{
using namespace phase2;

// ------------------------------------------------------------------------------------------------- a long-lag voice at a phrase start (choir-dsp-r3-5)

// A phrase on A3, a short breath, a phrase on E4. Voice 4 at Humanize and Drift 100 stands 55 to 85 ms behind the lead, longer than the
// breath, so when the tracker calls the second phrase it is still singing the first phrase's last note.
struct LongLagRun
{
    std::vector<double> pitch, hz;   // voice 4's pitch (semitones) and the lead's, one value per ms
    std::vector<int> note;           // voice 4's note
    std::vector<float> lag;
    int phraseStart = -1;            // the first ms of the second phrase the tracker reads
};

inline LongLagRun longLagRun (double breathSeconds)
{
    const auto x = join ({ zeros (0.2), vowelAt (220.0, 1.0, -16.0), zeros (breathSeconds), vowelAt (329.63, 0.6, -16.0), zeros (0.2) });
    auto p = clean (modeScale, 4);
    setParam (*p, "humanize", 100.0f);
    setParam (*p, vid (3, "Drift"), 100.0f);
    setParam (*p, "consonants", 0.0f);
    prepare (*p, 1, SR, 48);
    LongLagRun r;
    const int block = 48;   // 1 ms
    juce::AudioBuffer<float> buf (1, block);
    juce::MidiBuffer midi;
    for (int pos = 0; pos + block <= (int) x.size(); pos += block)
    {
        std::copy_n (x.data() + pos, block, buf.getWritePointer (0));
        p->processBlock (buf, midi);
        const auto rd = p->getReadings();
        r.pitch.push_back (rd.voicePitch[3]);
        r.note.push_back (rd.voiceNote[3]);
        r.lag.push_back (rd.voiceLagMs[3]);
        r.hz.push_back (rd.detectedHz);
        const int ms = (int) r.hz.size() - 1;
        if (r.phraseStart < 0 && ms > (int) ((1.2 + breathSeconds * 0.5) * 1000.0) && rd.detectedHz > 0.0f)
            r.phraseStart = ms;
    }
    return r;
}

inline void probeLongLag()
{
    const char* only = std::getenv ("R35BREATH");
    for (const double breath : { 0.035, 0.045, 0.05, 0.055, 0.06, 0.3 })
    {
        if (only != nullptr && std::abs (std::atof (only) - breath) > 1.0e-6) continue;
        const auto r = longLagRun (breath);
        std::printf ("breath %.0f ms, phrase start at %d ms\n", breath * 1000.0, r.phraseStart);
        for (int ms = juce::jmax (0, r.phraseStart - 10); ms < juce::jmin ((int) r.pitch.size(), r.phraseStart + 200); ++ms)
            std::printf ("%5d ms  lead %6.1f  voice 4 note %3d  pitch %8.3f  lag %5.1f\n", ms - r.phraseStart, r.hz[(size_t) ms], r.note[(size_t) ms], r.pitch[(size_t) ms], r.lag[(size_t) ms]);
    }
}

inline void testLongLagPhraseStart()
{
    // 45 ms of breath: the tracker goes without pitch for over 40 ms, so the second phrase is a new phrase, and voice 4 stands 64 ms behind.
    const auto r = longLagRun (0.045);
    if (r.phraseStart < 0) { check (false, "choir-dsp-r3-5: the second phrase is read"); return; }
    // (In the ms before the call the note reads -1: the lead has had no pitch for 40 ms. The voice is still singing the last phrase's note.)
    int oldNote = -1;
    for (int t = r.phraseStart - 1; t > 0 && oldNote < 0; --t) oldNote = r.note[(size_t) t];
    // 1. While voice 4 still sings the first phrase's last note, its pitch only moves the way Drift and the lead's own pitch move it: a few
    //    cents a ms at most. A scoop is 28 cents or more at Humanize 100 (0.4 to 1 of 70). On 1.6.0 the second phrase's scoop, detune and
    //    settle were drawn when the tracker called the phrase and bent that old note.
    double step = 0.0;
    int ms = r.phraseStart;
    for (; ms < (int) r.pitch.size() && r.note[(size_t) ms] == oldNote; ++ms)
        step = std::max (step, 100.0 * std::abs (r.pitch[(size_t) ms] - r.pitch[(size_t) ms - 1]));
    const int reached = ms;
    check (reached > r.phraseStart, "choir-dsp-r3-5 (guard): voice 4 is still on the last phrase's note when the tracker calls the new phrase (ms)", reached - r.phraseStart);
    check (step <= 5.0, "choir-dsp-r3-5: voice 4 on the last phrase's tail after the tracker calls a new phrase: its largest move in a ms (cents)", step);

    // 2. The new note gets its scoop when voice 4 comes to it: a drop of at least the smallest scoop, 28 cents in a ms, within its first
    //    100 ms on the new note. On 1.6.0 the scoop had been drawn on the old note, and the new note had none of its own.
    double drop = 0.0;
    const int newNote = r.note[(size_t) juce::jmin ((int) r.note.size() - 1, reached + 300)];
    for (int t = reached + 1; t < juce::jmin ((int) r.pitch.size(), reached + 100); ++t)
        if (r.note[(size_t) t] == newNote && r.note[(size_t) t - 1] == newNote)
            drop = std::min (drop, 100.0 * (r.pitch[(size_t) t] - r.pitch[(size_t) t - 1]));
    check (newNote != oldNote, "choir-dsp-r3-5 (guard): voice 4 moves to the new phrase's note");
    check (drop <= -28.0, "choir-dsp-r3-5: voice 4's scoop into the new phrase's note, its largest drop in a ms within 100 ms of reaching it (cents)", drop);
}
} // namespace round3

void testRound3()
{
    round3::testLongLagPhraseStart();
}
