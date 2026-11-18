#pragma once
// Round 6 (1.6.3): a voice reaches its new note later on a rising note change than on a falling one. Included by PluginTest.cpp inside its
// namespace, after Round5Tests.h. Run alone: PluginTest round6. Probe: PluginTest r6probe.

namespace round6
{
using namespace phase2;

// The lead holds one scale note, slides to another over slideMs, and holds that. The plug-in at its defaults, two voices, each on its own
// side, lead off, consonants off. Per ms: each voice's pitch and note as the note logic asks them (the grains sing that pitch: on Luke's takes
// the grain spacing follows it within 0.3%).
struct MoveRun
{
    std::array<std::vector<double>, 2> pitch;
    std::array<std::vector<int>, 2> note;
    int arrive = -1;     // first ms the lead is within 50 cents of its new note
};

inline MoveRun moveRun (int fromNote, int toNote, double slideMs, unsigned seed)
{
    const double hold = 0.6;
    std::vector<double> f0 = voice::steady (noteHz (fromNote), hold, SR);
    const int slide = (int) (slideMs * 0.001 * SR);
    for (int i = 0; i < slide; ++i) f0.push_back (noteHz (fromNote + (toNote - fromNote) * (i + 1.0) / slide));
    const auto rest = voice::steady (noteHz (toNote), hold, SR);
    f0.insert (f0.end(), rest.begin(), rest.end());
    const auto x = join ({ zeros (0.3), voice::synth (f0, voice::vowelA, SR, -60.0, -18.0), zeros (0.2) });

    PluginProcessor::Options o;
    o.keyFilePath = "/nonexistent/smooth-harmony-test.json";
    o.hostLatencyUpdates = false;
    o.randomSeed = seed;
    auto p = std::make_unique<PluginProcessor> (o);
    setParam (*p, "leadLevel", -60.0f);
    setParam (*p, "consonants", 0.0f);
    setParam (*p, "breath", 0.0f);
    setParam (*p, vid (0, "Pan"), -100.0f);
    setParam (*p, vid (1, "Pan"), 100.0f);
    prepare (*p, 2, SR, 48);
    MoveRun r;
    const int block = 48;   // 1 ms
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (int pos = 0; pos + block <= (int) x.size(); pos += block)
    {
        for (int c = 0; c < 2; ++c) std::copy_n (x.data() + pos, block, buf.getWritePointer (c));
        p->processBlock (buf, midi);
        const auto rd = p->getReadings();
        for (int v = 0; v < 2; ++v) { r.pitch[(size_t) v].push_back (rd.voicePitch[(size_t) v]); r.note[(size_t) v].push_back (rd.voiceNote[(size_t) v]); }
    }
    // the lead's f0 in ms from the start of the render
    for (int ms = 0; ms < (int) (f0.size() / 48); ++ms)
        if (std::abs (12.0 * std::log2 (f0[(size_t) ms * 48] / noteHz (toNote))) <= 0.5) { r.arrive = 300 + ms; break; }
    return r;
}

// How long after the lead is within 50 cents of its new note the voice is within 50 cents of its own new note and stays there 20 ms
// (onsetOnNoteCents, onsetHoldMs); negative when it is there before the lead, never when it never is. Also the size of the voice's own move
// in semitones.
struct Landing { static constexpr int never = 99999; int ms = never; int step = 0; };

inline Landing landing (const MoveRun& r, int v)
{
    Landing l;
    const auto& pitch = r.pitch[(size_t) v];
    const auto& note = r.note[(size_t) v];
    const int n = (int) pitch.size();
    const int before = note[(size_t) juce::jmax (0, r.arrive - 150)], after = note[(size_t) juce::jmin (n - 1, r.arrive + 400)];
    l.step = after - before;
    int held = 0;
    for (int t = r.arrive - 100; t < n; ++t)
    {
        held = std::abs (pitch[(size_t) t] - after) <= 0.5 ? held + 1 : 0;
        if (held >= (int) limits::onsetHoldMs && t - held + 1 >= r.arrive - 100) { l.ms = t - held + 1 - r.arrive; break; }
    }
    return l;
}

// Lead moves between C major scale notes around the Alto/Tenor range, each up and down.
inline const std::array<std::pair<int, int>, 8> moves { { { 52, 53 }, { 59, 60 }, { 48, 50 }, { 50, 52 }, { 53, 55 }, { 55, 57 }, { 57, 60 }, { 48, 52 } } };

inline void traceMove()
{
    const int a = std::atoi (std::getenv ("R6FROM")), b = std::atoi (std::getenv ("R6TO"));
    const double slideMs = std::getenv ("R6SLIDE") ? std::atof (std::getenv ("R6SLIDE")) : 0.0;
    const auto r = moveRun (a, b, slideMs, (unsigned) (std::getenv ("R6SEED") ? std::atoi (std::getenv ("R6SEED")) : 0));
    for (int t = r.arrive - 60; t < r.arrive + 140; t += 2)
        std::printf ("%+4d ms  v1 note %3d pitch %7.3f   v2 note %3d pitch %7.3f\n", t - r.arrive, r.note[0][(size_t) t], r.pitch[0][(size_t) t], r.note[1][(size_t) t], r.pitch[1][(size_t) t]);
    for (int v = 0; v < 2; ++v) { const auto l = landing (r, v); std::printf ("voice %d step %+d landing %d ms\n", v + 1, l.step, l.ms); }
}

inline void probeMoves()
{
    if (std::getenv ("R6FROM") != nullptr) { traceMove(); return; }
    const char* sl = std::getenv ("R6SLIDE");
    const double slideMs = sl != nullptr ? std::atof (sl) : 60.0;
    // worst and median landing per voice step size and direction
    std::map<int, std::vector<int>> rise, fall;
    for (unsigned seed = 0; seed < 32; ++seed)
        for (const auto& m : moves)
            for (int dir = 0; dir < 2; ++dir)
            {
                const int a = dir == 0 ? m.first : m.second, b = dir == 0 ? m.second : m.first;
                const auto r = moveRun (a, b, slideMs, seed);
                for (int v = 0; v < 2; ++v)
                {
                    const auto l = landing (r, v);
                    if (l.step == 0) continue;
                    (l.step > 0 ? rise : fall)[std::abs (l.step)].push_back (l.ms);
                    if (std::getenv ("R6ALL") != nullptr)
                        std::printf ("seed %2u lead %d->%d voice %d step %+d landing %d ms\n", seed, a, b, v + 1, l.step, l.ms);
                }
            }
    for (int s = 1; s <= 5; ++s)
    {
        auto& u = rise[s];
        auto& d = fall[s];
        if (u.empty() && d.empty()) continue;
        std::sort (u.begin(), u.end());
        std::sort (d.begin(), d.end());
        const auto med = [] (const std::vector<int>& x) { return x.empty() ? -1 : x[x.size() / 2]; };
        const auto worst = [] (const std::vector<int>& x) { return x.empty() ? -1 : x.back(); };
        std::printf ("slide %.0f ms, voice step %d: rising n %3zu median %3d worst %4d   falling n %3zu median %3d worst %4d\n", slideMs, s, u.size(), med (u), worst (u),
                     d.size(), med (d), worst (d));
    }
}

// The limit: how fast 1.6.2 lands on a falling change of the same size, measured with this same code on 1.6.2 before any fix (32 runs of
// random numbers, the 8 moves above each way, both voices; median and worst in ms). A rising change must land no later.
struct FallLimit { int step; double slideMs; int median, worst; };
inline const FallLimit fallLimits[] { { 1, 0.0, 40, 63 }, { 1, 60.0, 30, 66 }, { 1, 100.0, 25, 70 },
                                      { 2, 0.0, 54, 92 }, { 2, 60.0, 65, 96 }, { 2, 100.0, 58, 103 } };

inline void testRisingLanding()
{
    // Rising and falling moves of a semitone and a whole tone in the voice (from steps and slides of the lead between scale notes), at the
    // defaults: Humanize 50, Glide 30 ms. On 1.6.2 a rising whole tone landed up to 104 ms after the lead (a falling one 92): Humanize's
    // scoop from below was added on top of the glide from below.
    for (const double slideMs : { 0.0, 60.0, 100.0 })
    {
        std::map<int, std::vector<int>> rise, fall;
        for (unsigned seed = 0; seed < 32; ++seed)
            for (const auto& m : moves)
                for (int dir = 0; dir < 2; ++dir)
                {
                    const auto r = moveRun (dir == 0 ? m.first : m.second, dir == 0 ? m.second : m.first, slideMs, seed);
                    for (int v = 0; v < 2; ++v)
                    {
                        const auto l = landing (r, v);
                        if (l.step != 0) (l.step > 0 ? rise : fall)[std::abs (l.step)].push_back (l.ms);
                    }
                }
        for (auto* table : { &rise, &fall }) for (auto& [k, x] : *table) std::sort (x.begin(), x.end());
        for (const auto& lim : fallLimits)
        {
            if (lim.slideMs != slideMs) continue;
            const auto& u = rise[lim.step];
            const std::string what = "round 6: a rising " + std::string (lim.step == 1 ? "semitone" : "whole tone") + ", lead slide " + std::to_string ((int) slideMs) + " ms, ";
            check (! u.empty() && u[u.size() / 2] <= lim.median, what + "median ms to within 50 cents of the new note (1.6.2 falling: " + std::to_string (lim.median) + ")", u.empty() ? -1 : u[u.size() / 2]);
            check (! u.empty() && u.back() <= lim.worst, what + "worst of 32 runs (1.6.2 falling: " + std::to_string (lim.worst) + ")", u.empty() ? -1 : u.back());
        }
        // A move of a third in the voice (a leap of the lead) mostly passes a note on the way, and the second change is taken without a
        // glide (a note held under 80 ms): it does not depend on the scoop. Measured, not judged.
        for (const int step : { 3, 4 })
            for (const bool up : { true, false })
            {
                const auto& x = (up ? rise : fall)[step];
                if (x.empty()) continue;
                const std::string what = "round 6: a " + std::string (up ? "rising" : "falling") + " move of " + std::to_string (step) + " semitones, lead slide " + std::to_string ((int) slideMs) + " ms, ";
                info (what + "median ms", x[x.size() / 2]);
                info (what + "worst ms", x.back());
            }
    }
}
} // namespace round6

void testRound6()
{
    round6::testRisingLanding();
}
