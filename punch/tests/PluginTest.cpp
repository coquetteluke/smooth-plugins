// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// All limits below were written before the first run.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include "editor/PunchEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <thread>
#include "GoldenV010.h"
#include "PunchEngine040.h"

using namespace testkit;

namespace
{
    struct Settings { float punch = 0, attack = 0, sustain = 0, mix = 100, gain = 0, impact = 0, ceiling = -1, ceilingOn = 0, steadyNotes = 0; };

    void setParam (PluginProcessor& p, const char* id, float v)
    {
        auto* prm = p.apvts.getParameter (id);
        prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    }

    void apply (PluginProcessor& p, const Settings& s)
    {
        setParam (p, "punch", s.punch);
        setParam (p, "attack", s.attack);
        setParam (p, "sustain", s.sustain);
        setParam (p, "mix", s.mix);
        setParam (p, "gain", s.gain);
        setParam (p, "impact", s.impact);
        setParam (p, "ceiling", s.ceiling);
        setParam (p, "ceilingOn", s.ceilingOn);
        setParam (p, "steadyNotes", s.steadyNotes);
    }

    // A fresh plug-in with these settings. Returns output channel 0, and the latency through `latency`.
    std::vector<float> process (const std::vector<float>& in, const Settings& s, double sr = 48000.0, int block = 512, int* latency = nullptr)
    {
        PluginProcessor p;
        apply (p, s);
        prepare (p, 2, sr, block);
        if (latency != nullptr) *latency = p.getLatencySamples();
        auto out = render (p, in, block);
        return std::vector<float> (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples());
    }

    double windowRmsDb (const std::vector<float>& x, int from, int len) { return toDb (rms (x.data() + from, len)); }

    enum class Tone { sine, noise };

    // Instant onset, exponential decay. Starts 100 ms in, and is one second long.
    std::vector<float> burst (double sr, double amp, double freq = 1000.0, double decaySeconds = 0.1, Tone tone = Tone::sine, double total = 1.0)
    {
        const int n = (int) (sr * total), onset = (int) (sr * 0.1);
        std::vector<float> x ((size_t) n, 0.0f);
        std::mt19937 g (11); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        for (int i = onset; i < n; ++i)
        {
            const double t = (i - onset) / sr;
            const double v = tone == Tone::sine ? std::sin (juce::MathConstants<double>::twoPi * freq * t) : (double) d (g);
            x[(size_t) i] = (float) (amp * std::exp (-t / decaySeconds) * v);
        }
        return x;
    }

    // Start: the first 3 ms of the sound. Tail: 60 to 300 ms after the onset.
    struct StartTail { double start, tail; };

    StartTail measure (const std::vector<float>& in, const Settings& s, double sr = 48000.0)
    {
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const int onset = (int) (sr * 0.1), startLen = (int) (sr * 0.003);
        const int tailFrom = onset + (int) (sr * 0.06), tailLen = (int) (sr * 0.24);
        return { windowRmsDb (out, onset + lat, startLen) - windowRmsDb (in, onset, startLen),
                 windowRmsDb (out, tailFrom + lat, tailLen) - windowRmsDb (in, tailFrom, tailLen) };
    }

    double nullDb (const std::vector<float>& a, const std::vector<float>& b, int aOffset, int from, int len, double reference)
    {
        double s = 0;
        for (int i = from; i < from + len; ++i)
        {
            const double e = (double) a[(size_t) (i + aOffset)] - (double) b[(size_t) i];
            s += e * e;
        }
        return toDb (std::sqrt (s / len)) - toDb (reference);
    }

    std::vector<float> burstTrain (double sr, double amp, double seconds, Tone tone, double period = 0.25, double decay = 0.06, double freq = 300.0)
    {
        const int n = (int) (sr * seconds), step = (int) (sr * period);
        std::vector<float> x ((size_t) n, 0.0f);
        std::mt19937 g (5); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        for (int b = 0; b * step < n; ++b)
            for (int i = b * step; i < std::min (n, (b + 1) * step); ++i)
            {
                const double t = (i - b * step) / sr;
                const double v = tone == Tone::sine ? std::sin (juce::MathConstants<double>::twoPi * freq * t) : (double) d (g);
                x[(size_t) i] = (float) (amp * std::exp (-t / decay) * v);
            }
        return x;
    }

    double crestDb (const float* x, int n) { return toDb (peak (x, n)) - toDb (rms (x, n)); }
}

// Attack: first 3 ms moves by at least 6 dB of a 9 dB setting. Tail moves under 1 dB.
static void testAttack()
{
    const auto in = burst (48000.0, 0.5);
    for (float a : { 9.0f, -9.0f, 3.0f, -3.0f, 15.0f, -15.0f })
    {
        Settings s; s.attack = a;
        const auto m = measure (in, s);
        const std::string tag = "Attack " + std::to_string ((int) a) + " dB, 1 kHz decaying burst";
        const double need = std::abs (a) * 2.0 / 3.0;
        check (m.start * juce::jlimit (-1.0f, 1.0f, a) >= need, tag + ": first 3 ms change", m.start, "dB");
        check (std::abs (m.tail) <= 1.0, tag + ": tail change", m.tail, "dB");
    }
}

// Sustain: tail moves by at least 4 dB of a 9 dB setting. First 3 ms moves under 1 dB.
static void testSustain()
{
    const auto in = burst (48000.0, 0.5);
    for (float v : { 9.0f, -9.0f, 15.0f, -15.0f })
    {
        Settings s; s.sustain = v;
        const auto m = measure (in, s);
        const std::string tag = "Sustain " + std::to_string ((int) v) + " dB, 1 kHz decaying burst";
        const double need = std::abs (v) * 4.0 / 9.0;
        check (m.tail * juce::jlimit (-1.0f, 1.0f, v) >= need, tag + ": tail change", m.tail, "dB");
        check (std::abs (m.start) <= 1.0, tag + ": first 3 ms change", m.start, "dB");
    }
    Settings s; s.sustain = 3.0f;
    info ("Sustain 3 dB: tail change", measure (in, s).tail, "dB");
}

// Level independence: spread across -6, -18, -30 and -42 dBFS within 1.5 dB (the task's limit).
static void testLevelIndependence()
{
    for (const auto& [name, s] : std::initializer_list<std::pair<const char*, Settings>> {
             { "Attack +9", { 0, 9, 0, 100 } }, { "Attack -9", { 0, -9, 0, 100 } },
             { "Sustain +9", { 0, 0, 9, 100 } }, { "Sustain -9", { 0, 0, -9, 100 } } })
    {
        double loS = 1e9, hiS = -1e9, loT = 1e9, hiT = -1e9;
        for (double db : { -6.0, -18.0, -30.0, -42.0 })
        {
            const auto m = measure (burst (48000.0, juce::Decibels::decibelsToGain (db)), s);
            info (std::string (name) + " at " + std::to_string ((int) db) + " dBFS: first 3 ms", m.start, "dB");
            info (std::string (name) + " at " + std::to_string ((int) db) + " dBFS: tail", m.tail, "dB");
            loS = std::min (loS, m.start); hiS = std::max (hiS, m.start);
            loT = std::min (loT, m.tail); hiT = std::max (hiT, m.tail);
        }
        check (hiS - loS <= 1.5, std::string (name) + ": first 3 ms, spread across 36 dB of input level", hiS - loS, "dB");
        check (hiT - loT <= 1.5, std::string (name) + ": tail, spread across 36 dB of input level", hiT - loT, "dB");
    }
}

// Other sounds: Attack 9 moves at least 5 dB and Sustain 9 at least 3 dB, on low tones, high tones and noise, with the other end under 1.5 dB.
static void testOtherSounds()
{
    struct Sound { const char* name; std::vector<float> x; };
    const std::vector<Sound> sounds { { "200 Hz", burst (48000.0, 0.5, 200.0) }, { "4 kHz", burst (48000.0, 0.5, 4000.0) },
                                      { "noise", burst (48000.0, 0.5, 0.0, 0.1, Tone::noise) } };
    for (const auto& snd : sounds)
    {
        Settings a; a.attack = 9.0f;
        Settings su; su.sustain = 9.0f;
        const auto ma = measure (snd.x, a);
        const auto ms = measure (snd.x, su);
        check (ma.start >= 5.0, std::string ("Attack +9 on ") + snd.name + ": first 3 ms", ma.start, "dB");
        check (std::abs (ma.tail) <= 1.5, std::string ("Attack +9 on ") + snd.name + ": tail", ma.tail, "dB");
        check (ms.tail >= 3.0, std::string ("Sustain +9 on ") + snd.name + ": tail", ms.tail, "dB");
        check (std::abs (ms.start) <= 1.5, std::string ("Sustain +9 on ") + snd.name + ": first 3 ms", ms.start, "dB");
    }
    // Decay rate: not a pass limit, just how Sustain depends on how fast the sound dies away.
    for (double decay : { 0.03, 0.1, 0.3, 1.0 })
    {
        Settings su; su.sustain = 9.0f;
        info ("Sustain +9, decay time " + std::to_string ((int) (decay * 1000)) + " ms: tail", measure (burst (48000.0, 0.5, 1000.0, decay), su).tail, "dB");
    }
}

// Sample rates: the same result within 1 dB at 44.1, 48 and 96 kHz.
static void testRates()
{
    double lo[4] = { 1e9, 1e9, 1e9, 1e9 }, hi[4] = { -1e9, -1e9, -1e9, -1e9 };
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const auto in = burst (sr, 0.5);
        Settings a; a.attack = 9.0f;
        Settings su; su.sustain = 9.0f;
        const auto ma = measure (in, a, sr);
        const auto ms = measure (in, su, sr);
        const double v[4] = { ma.start, ma.tail, ms.start, ms.tail };
        for (int i = 0; i < 4; ++i) { lo[i] = std::min (lo[i], v[i]); hi[i] = std::max (hi[i], v[i]); }
    }
    check (hi[0] - lo[0] <= 1.0, "Attack +9, first 3 ms, spread across sample rates", hi[0] - lo[0], "dB");
    check (hi[3] - lo[3] <= 1.0, "Sustain +9, tail, spread across sample rates", hi[3] - lo[3], "dB");
}

// A steady tone has no start and no tail: at the extremes it must null against the delayed input to -60 dB.
static void testSteadyTones()
{
    for (double f : { 60.0, 100.0, 1000.0, 9000.0 })
    {
        const int n = 96000;
        std::vector<float> in ((size_t) n);
        sine (in.data(), n, f, 48000.0, 0.25);
        Settings s; s.attack = 15.0f; s.sustain = 15.0f;
        int lat = 0;
        const auto out = process (in, s, 48000.0, 512, &lat);
        const double residual = nullDb (out, in, lat, 48000, 24000, rms (in.data() + 48000, 24000));
        check (residual <= -60.0, "steady " + std::to_string ((int) f) + " Hz, Attack and Sustain at maximum: difference from input", residual, "dB");
    }
}

// A held note is not a tail. A sung vowel sags and swells by a few dB while it is held (vibrato, breath), which no steady test tone does.
// On Luke's real takes (2026-11-11) Sustain moved held vowels by its full amount, in and out. Limits written before the first run:
// while a note is held with its level swinging 3 dB peak to peak, Sustain moves it by under 1 dB; when it dies away, the tail still
// moves by at least 4 dB of a 9 dB setting (100 to 300 ms after it starts to fall). That is what Steady Notes is for (0.4.0). Off, Sustain
// is what it was, which the first version's numbers further down hold it to.
static void testHeldNotes()
{
    const double sr = 48000.0, twoPi = juce::MathConstants<double>::twoPi;
    const int n = (int) (sr * 3.6);
    for (const bool hardStart : { false, true })
    {
        std::vector<float> in ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            if (t < 0.3) continue;
            double env = 1.0;
            if (! hardStart && t < 0.35) env = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (t - 0.3) / 0.05);
            if (t > 2.8) env = std::exp (-(t - 2.8) / 0.1);
            const double swing = std::pow (10.0, 1.5 * std::sin (twoPi * 5.0 * (t - 0.3)) / 20.0);
            double v = 0.0;
            for (int h = 1; h <= 3; ++h) v += std::sin (twoPi * 220.0 * h * t) / h;
            in[(size_t) i] = (float) (0.2 * env * swing * v);
        }
        for (float v : { 9.0f, -9.0f, 6.0f, -7.0f })
        {
            Settings s; s.sustain = v; s.steadyNotes = 1.0f;
            int lat = 0;
            const auto out = process (in, s, sr, 512, &lat);
            double worst = 0.0;
            for (int w = 60; w < 270; ++w) // 0.6 to 2.7 s, 10 ms at a time
                worst = std::max (worst, std::abs (windowRmsDb (out, w * 480 + lat, 480) - windowRmsDb (in, w * 480, 480)));
            const int tailFrom = (int) (sr * 2.9), tailLen = (int) (sr * 0.2);
            const double tail = windowRmsDb (out, tailFrom + lat, tailLen) - windowRmsDb (in, tailFrom, tailLen);
            const std::string tag = std::string ("Steady Notes, Sustain ") + std::to_string ((int) v) + " dB on a held note with a 3 dB swing" + (hardStart ? ", hard start" : ", soft start");
            check (worst <= 1.0, tag + ": largest change while it is held", worst, "dB");
            check (tail * juce::jlimit (-1.0f, 1.0f, v) >= std::abs (v) * 4.0 / 9.0, tag + ": change of its tail", tail, "dB");
        }
    }
}

// Steady Notes, the other two things it does. Limits written before the first run.
// A quieter note that starts sharply after a louder one is a new sound: its first 15 ms move by under 1 dB (off, it gets the tail's gain).
// A hiss after a note (an S) is spared: it moves by under 1 dB, while a dull sound of the same level after the same note still moves.
// And the parameter itself: off by default, off in every preset, saved and recalled, off for an older session.
static void testSteadyNotes()
{
    const double sr = 48000.0, twoPi = juce::MathConstants<double>::twoPi;
    {
        std::vector<float> in ((size_t) (sr * 1.5), 0.0f);
        for (int i = 0; i < (int) in.size(); ++i)
        {
            const double t = i / sr;
            if (t >= 0.1 && t < 0.5) in[(size_t) i] = (float) (0.5 * std::sin (twoPi * 220.0 * t) * (t > 0.45 ? std::exp (-(t - 0.45) / 0.01) : 1.0));
            if (t >= 0.8 && t < 1.3) in[(size_t) i] = (float) (0.158 * std::sin (twoPi * 220.0 * (t - 0.8)));
        }
        for (float on : { 1.0f, 0.0f })
        {
            Settings s; s.sustain = 9.0f; s.steadyNotes = on;
            int lat = 0;
            const auto out = process (in, s, sr, 512, &lat);
            const int at = (int) (sr * 0.8), len = (int) (sr * 0.015);
            const double change = windowRmsDb (out, at + lat, len) - windowRmsDb (in, at, len);
            if (on > 0.5f) check (std::abs (change) <= 1.0, "Steady Notes, Sustain 9: first 15 ms of a quieter note after a louder one", change, "dB");
            else info ("Steady Notes off, Sustain 9: first 15 ms of a quieter note after a louder one", change, "dB");
        }
    }
    for (const bool hiss : { true, false })
    {
        std::vector<float> in ((size_t) (sr * 1.0), 0.0f);
        std::mt19937 g (3); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        float last = 0.0f;
        for (int i = 0; i < (int) in.size(); ++i)
        {
            const double t = i / sr;
            if (t >= 0.1 && t < 0.4) in[(size_t) i] = (float) (0.5 * std::sin (twoPi * 220.0 * t));
            if (t >= 0.4 && t < 0.7)
            {
                const float n = d (g);
                in[(size_t) i] = hiss ? 0.06f * (n - last) : (float) (0.12 * std::sin (twoPi * 300.0 * t)); // a differenced noise is nearly all above 4 kHz
                last = n;
            }
        }
        Settings s; s.sustain = 6.0f; s.steadyNotes = 1.0f;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const int at = (int) (sr * 0.45), len = (int) (sr * 0.2);
        const double change = windowRmsDb (out, at + lat, len) - windowRmsDb (in, at, len);
        if (hiss) check (std::abs (change) <= 1.0, "Steady Notes, Sustain 6: a hiss after a note", change, "dB");
        else check (change >= 3.0, "Steady Notes, Sustain 6: a dull sound of the same kind after the same note still gets its tail", change, "dB");
    }
    {
        PluginProcessor p;
        check (p.apvts.getRawParameterValue ("steadyNotes")->load() < 0.5f, "Steady Notes is off by default");
        bool olderOff = true;
        // 0.5.0 appended six presets for a lead vocal that have it on. The 14 that were there before keep it off, as this test has always asked.
        // Every preset is looked at again (0.5.1): the 14 older ones must have it off, the ones after them must have it on, because with it off
        // four of the six lead presets fail 45 real vocal preset limits on the working set (they add 3.0 to 5.8 dB of swing to held vowels).
        bool newerOn = true;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            const bool on = p.apvts.getRawParameterValue ("steadyNotes")->load() > 0.5f;
            if (i < 14) olderOff = olderOff && ! on;
            else newerOn = newerOn && on;
        }
        check (olderOff && p.getNumPrograms() >= 14, "Steady Notes is off in all 14 presets that were there before 0.5.0", (double) p.getNumPrograms());
        check (newerOn, "Steady Notes is on in every preset added after them", (double) (p.getNumPrograms() - 14));
        p.setCurrentProgram (0);
        setParam (p, "steadyNotes", 1.0f);
        check (p.isCurrentProgramEdited(), "turning it on shows the preset as edited");
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        PluginProcessor q;
        q.setStateInformation (saved.getData(), (int) saved.getSize());
        check (q.apvts.getRawParameterValue ("steadyNotes")->load() > 0.5f, "Steady Notes comes back with the session");
        juce::XmlElement xml ("STATE");
        auto* e = xml.createNewChildElement ("PARAM");
        e->setAttribute ("id", "sustain");
        e->setAttribute ("value", 6.0);
        juce::MemoryBlock older;
        juce::AudioProcessor::copyXmlToBinary (xml, older);
        q.setStateInformation (older.getData(), (int) older.getSize());
        check (q.apvts.getRawParameterValue ("steadyNotes")->load() < 0.5f, "a session from before 0.4.0 loads with Steady Notes off");
    }
}

// Punch: crest factor reduction grows at every step by at least 0.5 dB, at least 3 dB at 50 and at least 5 dB at 100.
// RMS stays within 1.5 dB of the input.
static void testPunch()
{
    const double sr = 48000.0;
    for (auto tone : { Tone::sine, Tone::noise })
    {
        const char* name = tone == Tone::sine ? "300 Hz burst train" : "noise burst train";
        const auto in = burstTrain (sr, 0.5, 6.0, tone);
        const int from = (int) (sr * 3), len = (int) (sr * 2);
        const double inCrest = crestDb (in.data() + from, len), inRms = windowRmsDb (in, from, len);
        info (std::string (name) + ": input crest factor", inCrest, "dB");
        double last = 0.0;
        for (float amount : { 25.0f, 50.0f, 75.0f, 100.0f })
        {
            Settings s; s.punch = amount;
            int lat = 0;
            const auto out = process (in, s, sr, 512, &lat);
            const double reduction = inCrest - crestDb (out.data() + from + lat, len);
            const double levelChange = windowRmsDb (out, from + lat, len) - inRms;
            const std::string tag = std::string ("Punch ") + std::to_string ((int) amount) + ", " + name;
            check (reduction >= last + 0.5, tag + ": crest factor reduction", reduction, "dB");
            check (std::abs (levelChange) <= 1.5, tag + ": RMS level change", levelChange, "dB");
            if (amount == 50.0f) check (reduction >= 3.0, tag + ": at least 3 dB", reduction, "dB");
            if (amount == 100.0f) check (reduction >= 5.0, tag + ": at least 5 dB", reduction, "dB");
            last = reduction;
        }
    }
    // Steady tone: within 0.5 dB of the input once settled, and a clean null of everything but level (-50 dB).
    {
        std::vector<float> in ((size_t) (sr * 3));
        sine (in.data(), (int) in.size(), 100.0, sr, 0.25);
        Settings s; s.punch = 100.0f;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const int from = (int) (sr * 2), len = (int) (sr * 0.5);
        const double change = windowRmsDb (out, from + lat, len) - windowRmsDb (in, from, len);
        check (std::abs (change) <= 0.5, "Punch 100, steady 100 Hz: level change", change, "dB");
        std::vector<float> scaled (in);
        const float g = (float) juce::Decibels::decibelsToGain (change);
        for (auto& v : scaled) v *= g;
        const double residual = nullDb (out, scaled, lat, from, len, rms (in.data() + from, len));
        check (residual <= -50.0, "Punch 100, steady 100 Hz: everything other than level, relative to the tone", residual, "dB");
    }
    // Lowest tone the short hold fully covers: everything other than level stays under -40 dB at the heaviest setting.
    {
        std::vector<float> in ((size_t) (sr * 3));
        sine (in.data(), (int) in.size(), 65.0, sr, 0.25);
        Settings s; s.punch = 100.0f;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const int from = (int) (sr * 2), len = (int) (sr * 0.5);
        const double change = windowRmsDb (out, from + lat, len) - windowRmsDb (in, from, len);
        std::vector<float> scaled (in);
        const float g = (float) juce::Decibels::decibelsToGain (change);
        for (auto& v : scaled) v *= g;
        const double residual = nullDb (out, scaled, lat, from, len, rms (in.data() + from, len));
        check (residual <= -40.0, "Punch 100, steady 65 Hz: everything other than level, relative to the tone", residual, "dB");
    }
    // Level: the same crest reduction within 1.5 dB at -6, -24 and -42 dBFS. Task asks this of Attack and Sustain only, but Punch is built the same way.
    {
        double lo = 1e9, hi = -1e9;
        for (double db : { -6.0, -24.0, -42.0 })
        {
            const auto in = burstTrain (sr, juce::Decibels::decibelsToGain (db), 6.0, Tone::sine);
            const int from = (int) (sr * 3), len = (int) (sr * 2);
            Settings s; s.punch = 50.0f;
            int lat = 0;
            const auto out = process (in, s, sr, 512, &lat);
            const double r = crestDb (in.data() + from, len) - crestDb (out.data() + from + lat, len);
            lo = std::min (lo, r); hi = std::max (hi, r);
        }
        check (hi - lo <= 1.5, "Punch 50: crest reduction spread across 36 dB of input level", hi - lo, "dB");
    }
    // Not a pass limit: the first hit after silence, before the level match has anything to go on.
    {
        const auto in = burstTrain (sr, 0.5, 3.0, Tone::sine);
        Settings s; s.punch = 50.0f;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const int step = (int) (sr * 0.25);
        info ("Punch 50: first hit after silence, level against the input", windowRmsDb (out, lat, step) - windowRmsDb (in, 0, step), "dB");
        info ("Punch 50: second hit", windowRmsDb (out, step + lat, step) - windowRmsDb (in, step, step), "dB");
        info ("Punch 50: fifth hit", windowRmsDb (out, 4 * step + lat, step) - windowRmsDb (in, 4 * step, step), "dB");
    }
    // Quiet noise after silence: below the floor Punch must not lift it by more than the resting level match (under 6 dB at 50).
    {
        std::vector<float> in ((size_t) (sr * 2));
        noise (in.data(), (int) in.size(), 1.0e-4, 9);
        Settings s; s.punch = 50.0f;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const double change = windowRmsDb (out, (int) sr + lat, (int) (sr * 0.5)) - windowRmsDb (in, (int) sr, (int) (sr * 0.5));
        check (change <= 6.0, "Punch 50, noise at -80 dBFS: level lift", change, "dB");
    }
}

// Spectrum of out relative to in, in narrow bands. A comb would show as big dips.
static double spectrumSpreadDb (const std::vector<float>& out, const std::vector<float>& in, int offset, double sr)
{
    const int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> win ((size_t) size, juce::dsp::WindowingFunction<float>::hann);
    std::vector<float> a ((size_t) size * 2), b ((size_t) size * 2);
    std::vector<double> pa ((size_t) size / 2, 0.0), pb ((size_t) size / 2, 0.0);
    for (int pos = (int) sr; pos + size + offset < (int) in.size(); pos += size / 2)
    {
        std::fill (a.begin(), a.end(), 0.0f); std::fill (b.begin(), b.end(), 0.0f);
        std::copy_n (out.data() + pos + offset, size, a.data());
        std::copy_n (in.data() + pos, size, b.data());
        win.multiplyWithWindowingTable (a.data(), (size_t) size);
        win.multiplyWithWindowingTable (b.data(), (size_t) size);
        fft.performFrequencyOnlyForwardTransform (a.data());
        fft.performFrequencyOnlyForwardTransform (b.data());
        for (int k = 0; k < size / 2; ++k) { pa[(size_t) k] += a[(size_t) k] * a[(size_t) k]; pb[(size_t) k] += b[(size_t) k] * b[(size_t) k]; }
    }
    const int first = (int) (200.0 * size / sr), last = (int) (16000.0 * size / sr), group = 8;
    double lo = 1e9, hi = -1e9;
    for (int k = first; k + group <= last; k += group)
    {
        double sa = 0, sb = 0;
        for (int j = k; j < k + group; ++j) { sa += pa[(size_t) j]; sb += pb[(size_t) j]; }
        const double d = 10.0 * std::log10 (sa / sb);
        lo = std::min (lo, d); hi = std::max (hi, d);
    }
    return hi - lo;
}

// Mix: 0 nulls with the delayed input. 50 is the average of 0 and 100 to -80 dB, which only happens if the paths line up.
// Broadband response at 50 stays within 1.5 dB. A control run with a 20 sample misaligned dry shows the measurement can see a comb (over 6 dB).
static void testMix()
{
    const double sr = 48000.0;
    const auto in = burstTrain (sr, 0.5, 4.0, Tone::noise, 0.25, 0.1);
    Settings s; s.punch = 60.0f; s.attack = 6.0f; s.sustain = 6.0f;
    int lat = 0;
    s.mix = 100.0f; const auto wet = process (in, s, sr, 512, &lat);
    s.mix = 0.0f; const auto dry = process (in, s, sr, 512);
    s.mix = 50.0f; const auto half = process (in, s, sr, 512);

    const int from = (int) sr, len = (int) (sr * 2);
    const double ref = rms (in.data() + from, len);
    check (nullDb (dry, in, lat, from, len, ref) <= -120.0, "Mix 0 with everything else on: difference from delayed input", nullDb (dry, in, lat, from, len, ref), "dB");

    std::vector<float> avg (in.size());
    for (size_t i = 0; i < avg.size(); ++i) avg[i] = 0.5f * (wet[i] + dry[i]);
    const double blend = nullDb (half, avg, 0, from, len, ref);
    check (blend <= -80.0, "Mix 50 against the average of Mix 0 and Mix 100: difference", blend, "dB");

    const double spread = spectrumSpreadDb (half, in, lat, sr);
    check (spread <= 1.5, "Mix 50 with noise bursts: response spread, 200 Hz to 16 kHz", spread, "dB");
    info ("Mix 100 with noise bursts: response spread, 200 Hz to 16 kHz", spectrumSpreadDb (wet, in, lat, sr), "dB");

    std::vector<float> combed (in.size(), 0.0f);
    for (size_t i = 0; i + 20 < combed.size(); ++i) combed[i] = 0.5f * (wet[i + 20] + dry[i]);
    const double combSpread = spectrumSpreadDb (combed, in, lat, sr);
    check (combSpread > 6.0, "control: a 20 sample misaligned dry reads as a comb, spread", combSpread, "dB");

    // Mix sets how much of the change is used: halfway, in dB terms, is between dry and full.
    const auto burstIn = burst (sr, 0.5);
    Settings a; a.attack = 12.0f; a.mix = 0.0f;
    const double at0 = measure (burstIn, a).start;
    a.mix = 50.0f; const double at50 = measure (burstIn, a).start;
    a.mix = 100.0f; const double at100 = measure (burstIn, a).start;
    check (std::abs (at0) < 0.01 && at50 > at0 + 1.0 && at100 > at50 + 1.0, "Attack +12 at Mix 50 lands between Mix 0 and Mix 100", at50, "dB");
}

// Neutral settings: a perfect delay, nothing more (-120 dB, which is as far as float rounding is worth checking).
static void testNeutral()
{
    std::vector<float> in (96000);
    noise (in.data(), 96000, 0.3, 3);
    for (int i = 0; i < 96000; ++i) in[(size_t) i] += 0.3f * (float) std::sin (0.03 * i) * (i % 24000 < 2000 ? 1.0f : 0.1f);
    for (float mixValue : { 100.0f, 50.0f })
    {
        Settings s; s.mix = mixValue;
        int lat = 0;
        const auto out = process (in, s, 48000.0, 512, &lat);
        const int len = 96000 - lat;
        const double d = nullDb (out, in, lat, 0, len, rms (in.data(), len));
        check (d <= -120.0, "neutral at Mix " + std::to_string ((int) mixValue) + ": difference from delayed input", d, "dB");
    }
}

// Latency at each rate, bypass with the same delay, and a real-time safe bypass path.
static void testLatencyAndBypass()
{
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        std::vector<float> in ((size_t) sr / 2, 0.0f);
        in[100] = 0.5f;
        Settings s; s.punch = 50.0f; s.attack = 6.0f; s.sustain = 6.0f; s.mix = 0.0f;
        int lat = 0;
        const auto out = process (in, s, sr, 128, &lat);
        int found = -1;
        for (int i = 0; i < (int) out.size(); ++i) if (std::abs (out[(size_t) i]) > 0.25f) { found = i - 100; break; }
        check (found == lat && lat == (int) std::lround (sr * 0.002), "impulse delay equals reported latency at " + std::to_string ((int) sr) + " Hz", found, "samples");
    }
    const auto in = burst (48000.0, 0.5);
    {
        PluginProcessor p;
        apply (p, { 50, 9, 9, 100 });
        setParam (p, "bypass", 1.0f);
        prepare (p, 2, 48000.0, 512);
        const int lat = p.getLatencySamples();
        auto out = render (p, in, 512);
        std::vector<float> o (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples());
        const int len = (int) in.size() - lat;
        check (nullDb (o, in, lat, 0, len, rms (in.data(), len)) <= -120.0, "Bypass parameter: the input, delayed by the same latency");
    }
    {
        PluginProcessor p;
        apply (p, { 50, 9, 9, 100 });
        prepare (p, 2, 48000.0, 512);
        const int lat = p.getLatencySamples();
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        std::vector<float> o;
        for (size_t pos = 0; pos + 512 <= in.size(); pos += 512)
        {
            for (int ch = 0; ch < 2; ++ch) buf.copyFrom (ch, 0, in.data() + pos, 512);
            p.processBlockBypassed (buf, midi);
            o.insert (o.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 512);
        }
        // The fade from on to bypassed takes 20 ms, so measure after it, across the loud part of the burst.
        const double d = nullDb (o, in, lat, 4800 + 2400, 24000, rms (in.data() + 7200, 24000));
        check (d <= -120.0, "processBlockBypassed after its fade: difference from delayed input", d, "dB");
    }
}

// Same output at any block size, and prepareToPlay gives a clean restart.
static void testBlocksAndRestart()
{
    const auto in = burstTrain (48000.0, 0.5, 2.0, Tone::noise);
    Settings s { 50, 6, -6, 80 };
    const auto a = process (in, s, 48000.0, 512);
    for (int block : { 1, 32, 777, 4096 })
    {
        const auto b = process (in, s, 48000.0, block);
        const int len = (int) in.size();
        const double d = nullDb (a, b, 0, 0, len, rms (in.data(), len));
        check (d <= -100.0, "block size " + std::to_string (block) + " against 512: difference", d, "dB");
    }
    PluginProcessor p;
    apply (p, s);
    prepare (p, 2, 48000.0, 512);
    render (p, in, 512);
    prepare (p, 2, 96000.0, 64);
    render (p, in, 64);
    prepare (p, 2, 48000.0, 512);
    auto again = render (p, in, 512);
    std::vector<float> r (again.getReadPointer (0), again.getReadPointer (0) + again.getNumSamples());
    const double d = nullDb (a, r, 0, 0, (int) in.size(), rms (in.data(), (int) in.size()));
    check (d <= -100.0, "after preparing again at other rates: same output, difference", d, "dB");
}

// Gain never jumps: at the extremes, the gain moves under 1 dB per sample at 48 kHz. Stereo: one gain for both channels.
static void testGainMotionAndStereo()
{
    const auto in = burstTrain (48000.0, 0.5, 2.0, Tone::sine, 0.25, 0.1, 100.0);
    Settings s { 0, -15, 15, 100 };
    int lat = 0;
    const auto out = process (in, s, 48000.0, 512, &lat);
    double worst = 0, last = 0; bool have = false;
    for (int i = 0; i + lat < (int) in.size(); ++i)
    {
        const float dry = in[(size_t) i];
        if (std::abs (dry) < 0.002f) { have = false; continue; }
        const double g = toDb (std::abs (out[(size_t) (i + lat)] / dry));
        if (have) worst = std::max (worst, std::abs (g - last));
        last = g; have = true;
    }
    check (worst <= 1.0, "Attack -15 with Sustain +15 on 100 Hz bursts: largest gain move in one sample", worst, "dB");

    PluginProcessor p;
    apply (p, { 50, 6, 6, 100 });
    prepare (p, 2, 48000.0, 512);
    const auto left = burst (48000.0, 0.5);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    double worstImage = 0, rightLeak = 0;
    const int lat2 = p.getLatencySamples();
    std::vector<float> l;
    for (size_t pos = 0; pos + 512 <= left.size(); pos += 512)
    {
        for (int i = 0; i < 512; ++i) { buf.setSample (0, i, left[pos + (size_t) i]); buf.setSample (1, i, 0.5f * left[pos + (size_t) i]); }
        p.processBlock (buf, midi);
        for (int i = 0; i < 512; ++i)
            if (std::abs (buf.getSample (0, i)) > 1.0e-3f)
                worstImage = std::max (worstImage, std::abs (toDb (std::abs (buf.getSample (1, i) / buf.getSample (0, i))) + 6.0206));
    }
    check (worstImage <= 0.01, "stereo: left to right balance stays put, biggest move", worstImage, "dB");

    PluginProcessor q;
    apply (q, { 50, 6, 6, 100 });
    prepare (q, 2, 48000.0, 512);
    for (size_t pos = 0; pos + 512 <= left.size(); pos += 512)
    {
        buf.copyFrom (0, 0, left.data() + pos, 512);
        buf.clear (1, 0, 512);
        q.processBlock (buf, midi);
        rightLeak = std::max (rightLeak, peak (buf.getReadPointer (1), 512));
    }
    check (rightLeak == 0.0, "stereo: silence on one side stays silent", rightLeak);
    info ("latency at 48 kHz", lat2, "samples");
}

// Damaged saved state, beyond the generic checks: a dropped value returns to that control's default, an out of range value clamps,
// a cut off or foreign blob changes nothing. Bad input samples must not poison the detector either.
static void testDamagedStateAndInput()
{
    const auto valueOf = [] (PluginProcessor& p, const char* id) { return p.apvts.getParameterRange (id).convertFrom0to1 (p.apvts.getParameter (id)->getValue()); };
    const auto load = [] (PluginProcessor& p, const juce::String& attackValue)
    {
        juce::XmlElement xml ("STATE");
        for (const auto& [id, v] : std::initializer_list<std::pair<const char*, juce::String>> { { "attack", attackValue }, { "punch", "40" }, { "sustain", "-3" }, { "mix", "70" } })
        {
            auto* e = xml.createNewChildElement ("PARAM");
            e->setAttribute ("id", id);
            e->setAttribute ("value", v);
        }
        juce::MemoryBlock b;
        juce::AudioProcessor::copyXmlToBinary (xml, b);
        p.setStateInformation (b.getData(), (int) b.getSize());
    };
    {
        PluginProcessor p;
        setParam (p, "attack", 9.0f);
        load (p, "nan");
        check (std::abs (valueOf (p, "attack")) < 1.0e-3f && std::abs (valueOf (p, "punch") - 40.0f) < 0.01f, "state with NaN in Attack: Attack back at default, the rest kept", valueOf (p, "attack"), "dB");
    }
    {
        PluginProcessor p;
        load (p, "1e30");
        check (std::abs (valueOf (p, "attack") - 15.0f) < 1.0e-3f, "state with a huge Attack: clamped to the top of its range", valueOf (p, "attack"), "dB");
    }
    {
        PluginProcessor p;
        setParam (p, "sustain", 4.0f);
        juce::MemoryBlock good;
        p.getStateInformation (good);
        setParam (p, "sustain", 7.0f);
        p.setStateInformation (good.getData(), (int) good.getSize() / 2);
        const char junk[] = "not a saved state at all";
        p.setStateInformation (junk, (int) sizeof (junk));
        check (std::abs (valueOf (p, "sustain") - 7.0f) < 1.0e-3f, "cut off or foreign state: controls unchanged", valueOf (p, "sustain"), "dB");
    }
    {
        // NaN and infinity samples in the input never reach the output, and the gain carries on as normal.
        auto in = burst (48000.0, 0.5);
        in[6000] = std::numeric_limits<float>::quiet_NaN();
        in[6001] = std::numeric_limits<float>::infinity();
        Settings s { 50, 6, 6, 100 };
        int lat = 0;
        const auto out = process (in, s, 48000.0, 512, &lat);
        long bad = 0;
        for (float v : out) if (! std::isfinite (v)) ++bad;
        check (bad == 0, "NaN and infinity in the input: none in the output", (double) bad, "samples");
    }
}

// ---- Added in 0.2.0. Every limit below was written before the first run of the new code. ----

namespace
{
    Settings impactOnly (float amount) { Settings s; s.impact = amount; return s; }
    Settings withCeiling (float db) { Settings s; s.ceiling = db; s.ceilingOn = 1.0f; return s; }

    std::vector<float> tone (double sr, double seconds, double freq, double db)
    {
        std::vector<float> x ((size_t) (sr * seconds));
        sine (x.data(), (int) x.size(), freq, sr, juce::Decibels::decibelsToGain (db));
        return x;
    }

    // The largest absolute sample of the output, all channels.
    double outputPeak (const std::vector<float>& in, const Settings& s, double sr = 48000.0, int block = 512)
    {
        PluginProcessor p;
        apply (p, s);
        prepare (p, 2, sr, block);
        auto out = render (p, in, block);
        return juce::jmax (peak (out.getReadPointer (0), out.getNumSamples()), peak (out.getReadPointer (1), out.getNumSamples()));
    }

    // Level of everything other than a plain gain change, relative to the input, once settled.
    double beyondLevelDb (const std::vector<float>& in, const std::vector<float>& out, int lat, int from, int len)
    {
        const double change = windowRmsDb (out, from + lat, len) - windowRmsDb (in, from, len);
        std::vector<float> scaled (in);
        const float g = (float) juce::Decibels::decibelsToGain (change);
        for (auto& v : scaled) v *= g;
        return nullDb (out, scaled, lat, from, len, rms (in.data() + from, len) * g);
    }

    // Loud and quiet stretches of one tone, 1.5 s each, repeated.
    std::vector<float> twoLevels (double sr, double loudDb, double quietDb, double freq = 1000.0)
    {
        const int stretch = (int) (sr * 1.5), n = stretch * 4;
        std::vector<float> x ((size_t) n);
        const double loud = juce::Decibels::decibelsToGain (loudDb), quiet = juce::Decibels::decibelsToGain (quietDb);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) (((i / stretch) % 2 == 0 ? loud : quiet) * std::sin (juce::MathConstants<double>::twoPi * freq * i / sr));
        return x;
    }

    // How much the level gap between the loud and quiet stretches shrinks, in the settled last 0.5 s of the last two stretches.
    double gapShrink (double sr, const Settings& s, double loudDb = -6.0, double quietDb = -30.0)
    {
        const auto in = twoLevels (sr, loudDb, quietDb);
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const int stretch = (int) (sr * 1.5), len = (int) (sr * 0.5);
        const double loud = windowRmsDb (out, stretch * 2 + stretch - len + lat, len), quiet = windowRmsDb (out, stretch * 3 + stretch - len, len - lat);
        return (loudDb - quietDb) - (loud - quiet);
    }

    // Something shaped like speech, with no claim that it sounds like a voice: noise and a low tone, syllables at 5 per second, phrases at different levels with gaps.
    std::vector<float> speechShaped (double sr, double seconds)
    {
        const int n = (int) (sr * seconds);
        std::vector<float> x ((size_t) n);
        std::mt19937 g (21); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        const double phrases[] = { -8.0, -20.0, -13.0, -26.0, -10.0, -17.0 };
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const int phrase = (int) (t / 0.8);
            const double inPhrase = t - phrase * 0.8;
            const double phraseLevel = inPhrase < 0.65 ? juce::Decibels::decibelsToGain (phrases[phrase % 6]) : 0.0;
            const double syllable = std::pow (std::abs (std::sin (juce::MathConstants<double>::pi * 5.0 * t)), 2.0);
            x[(size_t) i] = (float) (phraseLevel * (0.2 + 0.8 * syllable) * (0.8 * std::sin (juce::MathConstants<double>::twoPi * 180.0 * t) + 0.2 * d (g)));
        }
        return x;
    }

    // Spread between loud and quiet moments: 90th minus 10th percentile of 100 ms RMS, across the moments with sound in them.
    double levelSpreadDb (const float* x, int n, double sr)
    {
        const int w = (int) (sr * 0.1);
        std::vector<double> levels;
        for (int i = 0; i + w <= n; i += w / 2)
        {
            const double db = toDb (rms (x + i, w));
            if (db > -60.0) levels.push_back (db);
        }
        std::sort (levels.begin(), levels.end());
        return levels[(size_t) (levels.size() * 9 / 10)] - levels[levels.size() / 10];
    }

    float readingFromText (juce::AudioProcessorParameter& prm) { return prm.getText (prm.getValue(), 32).getFloatValue(); }
}

// Gain: exactly what it says at neutral. Attack, Sustain and Punch don't care, since they ignore level. Impact does, on purpose.
static void testGain()
{
    const double sr = 48000.0;
    const auto in = tone (sr, 1.0, 1000.0, -24.0);
    for (float g : { -12.0f, 6.0f, 12.0f, 24.0f })
    {
        Settings s; s.gain = g;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const double change = windowRmsDb (out, 24000 + lat, 12000) - windowRmsDb (in, 24000, 12000);
        check (std::abs (change - g) <= 0.05, "Gain " + std::to_string ((int) g) + " dB at neutral: level change", change, "dB");
    }
    // The shaper is level independent: within 0.1 dB of the same setting without Gain.
    const auto b = burst (sr, 0.05);
    Settings a0; a0.attack = 9.0f;
    Settings a12 = a0; a12.gain = 12.0f;
    const auto m0 = measure (b, a0), m12 = measure (b, a12);
    check (std::abs ((m12.start - 12.0) - m0.start) <= 0.1, "Attack +9 with Gain +12: first 3 ms, against no Gain", (m12.start - 12.0) - m0.start, "dB");
    Settings s0; s0.sustain = 9.0f;
    Settings s12 = s0; s12.gain = 12.0f;
    const auto ms0 = measure (b, s0), ms12 = measure (b, s12);
    check (std::abs ((ms12.tail - 12.0) - ms0.tail) <= 0.1, "Sustain +9 with Gain +12: tail, against no Gain", (ms12.tail - 12.0) - ms0.tail, "dB");

    // Gain drives Impact. Impact 50 on a -30 dBFS tone, which alone is below its threshold: +18 dB of Gain gives at least 8 and at most 14 dB more out.
    const auto quiet = tone (sr, 3.0, 1000.0, -30.0);
    Settings i0 = impactOnly (50.0f);
    Settings i18 = i0; i18.gain = 18.0f;
    int lat = 0;
    const auto o0 = process (quiet, i0, sr, 512, &lat);
    const auto o18 = process (quiet, i18, sr, 512);
    const double rise = windowRmsDb (o18, 96000, 24000) - windowRmsDb (o0, 96000, 24000);
    check (rise >= 8.0 && rise <= 14.0, "Gain +18 into Impact 50 on a -30 dBFS tone: level rise", rise, "dB");

    // Mix blends the dynamics only, so Mix 0 still carries Gain. Bypass takes Gain out.
    const auto loudIn = burst (sr, 0.25);
    Settings mix0; mix0.gain = 6.0f; mix0.mix = 0.0f;
    const auto o = process (loudIn, mix0, sr, 512, &lat);
    std::vector<float> doubled (loudIn);
    const float g6 = (float) juce::Decibels::decibelsToGain (6.0);
    for (auto& v : doubled) v *= g6;
    const int len = (int) loudIn.size() - lat;
    const double mixNull = nullDb (o, doubled, lat, 0, len, rms (doubled.data(), len));
    check (mixNull <= -100.0, "Mix 0 with Gain +6: difference from the delayed input raised 6 dB", mixNull, "dB");
    PluginProcessor p;
    Settings by; by.gain = 12.0f; by.impact = 70.0f; by.ceiling = -12.0f; by.ceilingOn = 1.0f;
    apply (p, by);
    setParam (p, "bypass", 1.0f);
    prepare (p, 2, sr, 512);
    auto bo = render (p, loudIn, 512);
    std::vector<float> bypassed (bo.getReadPointer (0), bo.getReadPointer (0) + bo.getNumSamples());
    const double byNull = nullDb (bypassed, loudIn, lat, 0, len, rms (loudIn.data(), len));
    check (byNull <= -120.0, "Bypass with Gain, Impact and Ceiling on: difference from the delayed input", byNull, "dB");
}

// Impact: evens levels out and raises them. Limits follow from the curve, less some room for knee and release.
static void testImpact()
{
    const double sr = 48000.0;
    // A 24 dB gap between loud and quiet stretches shrinks by at least 3, 8 and 14 dB at 25, 50 and 100, and at least 2 dB more at each step.
    double last = 0.0;
    for (const auto& [amount, need] : std::initializer_list<std::pair<float, double>> { { 25.0f, 3.0 }, { 50.0f, 8.0 }, { 75.0f, 10.0 }, { 100.0f, 14.0 } })
    {
        const double shrink = gapShrink (sr, impactOnly (amount));
        const std::string tag = "Impact " + std::to_string ((int) amount) + ": 24 dB level gap shrinks by";
        check (shrink >= need, tag, shrink, "dB");
        check (shrink >= last + 2.0, tag + " (at least 2 dB more than the step before)", shrink - last, "dB");
        last = shrink;
    }
    // Raises the level: on a steady -24 dBFS tone at least 3, 6 and 8 dB at 25, 50 and 100. On -40 dBFS, under every threshold, at least 15 at 100.
    for (const auto& [amount, need] : std::initializer_list<std::pair<float, double>> { { 25.0f, 3.0 }, { 50.0f, 6.0 }, { 100.0f, 8.0 } })
    {
        const auto in = tone (sr, 3.0, 1000.0, -24.0);
        int lat = 0;
        const auto out = process (in, impactOnly (amount), sr, 512, &lat);
        const double change = windowRmsDb (out, 96000 + lat, 24000) - windowRmsDb (in, 96000, 24000);
        check (change >= need, "Impact " + std::to_string ((int) amount) + ", -24 dBFS tone: level rise", change, "dB");
    }
    {
        const auto in = tone (sr, 3.0, 1000.0, -40.0);
        int lat = 0;
        const auto out = process (in, impactOnly (100.0f), sr, 512, &lat);
        const double change = windowRmsDb (out, 96000 + lat, 24000) - windowRmsDb (in, 96000, 24000);
        check (change >= 15.0, "Impact 100, -40 dBFS tone: level rise", change, "dB");
    }
    // A loud level stays in hand: under 6 dB of rise at the top of the control on a -6 dBFS tone.
    {
        const auto in = tone (sr, 3.0, 1000.0, -6.0);
        int lat = 0;
        const auto out = process (in, impactOnly (100.0f), sr, 512, &lat);
        const double change = windowRmsDb (out, 96000 + lat, 24000) - windowRmsDb (in, 96000, 24000);
        check (change <= 6.0, "Impact 100, -6 dBFS tone: level rise", change, "dB");
    }
    // Speech shaped signal: loud to quiet spread shrinks by at least 4 dB at 50 and 6 dB at 100.
    {
        const auto in = speechShaped (sr, 10.0);
        const int skip = (int) (sr * 2);
        const int len = (int) in.size() - skip - 200;
        const double spreadIn = levelSpreadDb (in.data() + skip, len, sr);
        info ("speech shaped signal: input level spread", spreadIn, "dB");
        for (const auto& [amount, need] : std::initializer_list<std::pair<float, double>> { { 50.0f, 4.0 }, { 100.0f, 6.0 } })
        {
            int lat = 0;
            const auto out = process (in, impactOnly (amount), sr, 512, &lat);
            const double shrink = spreadIn - levelSpreadDb (out.data() + skip + lat, len, sr);
            check (shrink >= need, "Impact " + std::to_string ((int) amount) + ", speech shaped signal: level spread shrinks by", shrink, "dB");
        }
    }
    // Steady low tones stay clean at the top of the control: everything other than level under -50 dB.
    for (double f : { 65.0, 100.0, 1000.0 })
    {
        const auto in = tone (sr, 3.0, f, -12.0);
        int lat = 0;
        const auto out = process (in, impactOnly (100.0f), sr, 512, &lat);
        const double beyond = beyondLevelDb (in, out, lat, 96000, 24000);
        check (beyond <= -50.0, "Impact 100, steady " + std::to_string ((int) f) + " Hz: everything other than level", beyond, "dB");
    }
    // Speed. A jump from -30 to -6 dBFS: the gain is within 3 dB of settled by 25 ms.
    const auto gainDbAt = [] (const std::vector<float>& in, const std::vector<float>& out, int lat, int at, int len)
    {
        return windowRmsDb (out, at + lat, len) - windowRmsDb (in, at, len);
    };
    {
        const int onset = (int) (sr * 2);
        std::vector<float> in ((size_t) (sr * 4));
        for (int i = 0; i < (int) in.size(); ++i)
            in[(size_t) i] = (float) (juce::Decibels::decibelsToGain (i < onset ? -30.0 : -6.0) * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / sr));
        int lat = 0;
        const auto out = process (in, impactOnly (100.0f), sr, 512, &lat);
        const double settled = gainDbAt (in, out, lat, onset + (int) sr, 24000);
        const double at25 = gainDbAt (in, out, lat, onset + (int) (sr * 0.025), 48);
        check (std::abs (at25 - settled) <= 3.0, "Impact 100, jump from -30 to -6 dBFS: gain 25 ms later, against settled", at25 - settled, "dB");
    }
    // Program dependent release. Back within 3 dB of the quiet level's gain: at most 300 ms after a 50 ms peak, and at least twice as long after a 1.5 s hold.
    {
        const auto recovery = [&] (double holdSeconds)
        {
            const int settle = (int) (sr * 3), peakLen = (int) (sr * holdSeconds), n = settle + peakLen + (int) (sr * 4);
            std::vector<float> in ((size_t) n);
            for (int i = 0; i < n; ++i)
                in[(size_t) i] = (float) (juce::Decibels::decibelsToGain (i >= settle && i < settle + peakLen ? -6.0 : -30.0) * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / sr));
            int lat = 0;
            const auto out = process (in, impactOnly (100.0f), sr, 512, &lat);
            const double target = windowRmsDb (out, n - (int) (sr * 0.5) - lat, 24000) - windowRmsDb (in, n - (int) (sr * 0.5) - lat, 24000);
            const int end = settle + peakLen, w = 48;
            for (int i = end; i + w + lat < n; i += w)
                if (windowRmsDb (out, i + lat, w) - windowRmsDb (in, i, w) >= target - 3.0)
                    return (i - end) / sr * 1000.0;
            return 1.0e6;
        };
        const double afterShort = recovery (0.05), afterLong = recovery (1.5);
        check (afterShort <= 300.0, "Impact 100: recovery after a 50 ms peak", afterShort, "ms");
        check (afterLong >= 2.0 * afterShort, "Impact 100: recovery after a 1.5 s hold", afterLong, "ms");
    }
    // Near silence isn't lifted: noise at -80 dBFS rises at most 3 dB at the top of the control.
    {
        std::vector<float> in ((size_t) (sr * 3));
        noise (in.data(), (int) in.size(), 1.0e-4, 9);
        int lat = 0;
        const auto out = process (in, impactOnly (100.0f), sr, 512, &lat);
        const double change = windowRmsDb (out, 96000 + lat, 24000) - windowRmsDb (in, 96000, 24000);
        check (change <= 3.0, "Impact 100, noise at -80 dBFS: level lift", change, "dB");
    }
    // Mix: 0 with Impact on nulls, and 50 is the average of 0 and 100, so there is still no comb.
    {
        const auto in = speechShaped (sr, 4.0);
        Settings s = impactOnly (80.0f);
        int lat = 0;
        s.mix = 100.0f; const auto wet = process (in, s, sr, 512, &lat);
        s.mix = 0.0f; const auto dry = process (in, s, sr, 512);
        s.mix = 50.0f; const auto half = process (in, s, sr, 512);
        const int from = (int) sr, len = (int) (sr * 2);
        const double ref = rms (in.data() + from, len);
        const double d0 = nullDb (dry, in, lat, from, len, ref);
        check (d0 <= -120.0, "Mix 0 with Impact 80: difference from the delayed input", d0, "dB");
        std::vector<float> avg (in.size());
        for (size_t i = 0; i < avg.size(); ++i) avg[i] = 0.5f * (wet[i] + dry[i]);
        const double blend = nullDb (half, avg, 0, from, len, ref);
        check (blend <= -80.0, "Mix 50 with Impact 80, against the average of Mix 0 and Mix 100", blend, "dB");
    }
    // Sample rates: the same within 0.5 dB.
    {
        double lo = 1e9, hi = -1e9;
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            const double v = gapShrink (rate, impactOnly (50.0f));
            lo = std::min (lo, v); hi = std::max (hi, v);
        }
        check (hi - lo <= 0.5, "Impact 50: gap shrink, spread across sample rates", hi - lo, "dB");
    }
}

// Ceiling: nothing past it, nothing changed under it.
static void testCeiling()
{
    const double sr = 48000.0;
    // Hot material with every boost on: peak at most 0.01 dB over the ceiling. Includes sudden onsets the look ahead can't fully see.
    {
        const auto hot = burstTrain (sr, 0.7, 3.0, Tone::noise);
        const auto sineHits = burstTrain (sr, 0.7, 3.0, Tone::sine);
        for (float db : { -1.0f, -6.0f, -12.0f, -24.0f })
            for (const auto* in : { &hot, &sineHits })
            {
                Settings s { 20.0f, 12.0f, 3.0f, 100.0f, 12.0f, 70.0f, db, 1.0f };
                const double over = toDb (outputPeak (*in, s)) - db;
                check (over <= 0.01, std::string ("Ceiling ") + std::to_string ((int) db) + " dBFS, " + (in == &hot ? "noise" : "300 Hz") + " hits, all boosts on: peak over the ceiling", over, "dB");
            }
        for (double rate : { 44100.0, 96000.0 })
        {
            Settings s { 20.0f, 12.0f, 3.0f, 100.0f, 12.0f, 70.0f, -6.0f, 1.0f };
            const auto in = burstTrain (rate, 0.7, 3.0, Tone::noise);
            const double over = toDb (outputPeak (in, s, rate)) + 6.0;
            check (over <= 0.01, "Ceiling -6 dBFS at " + std::to_string ((int) rate) + " Hz: peak over the ceiling", over, "dB");
        }
    }
    // A lone spike at an onset. The look ahead reads it at the gain before Attack's boost arrives, so this is the case only the exact last step can hold.
    // At most 0.01 dB over. Without that last step this fails, which is the point of the test.
    {
        std::vector<float> in ((size_t) sr, 0.0f);
        in[4800] = 0.2f;
        for (int i = 4801; i < 4801 + 240; ++i) in[(size_t) i] = 0.02f;
        Settings s = withCeiling (-12.0f); s.attack = 15.0f;
        const double over = toDb (outputPeak (in, s)) + 12.0;
        check (over <= 0.01, "Ceiling -12 dBFS, lone spike at an onset with Attack +15: peak over the ceiling", over, "dB");
    }
    // Under the ceiling nothing happens at all.
    {
        const auto in = tone (sr, 1.0, 1000.0, -12.0);
        int lat = 0;
        const auto out = process (in, withCeiling (-6.0f), sr, 512, &lat);
        const int len = (int) in.size() - lat;
        const double d = nullDb (out, in, lat, 0, len, rms (in.data(), len));
        check (d <= -100.0, "Ceiling -6 dBFS on a -12 dBFS tone: difference from the delayed input", d, "dB");
    }
    // A steady tone 6 dB over: lands on the ceiling within 0.2 dB, and stays clean. Under -50 dB other than level, -40 dB at 60 Hz, near the end of what the hold covers.
    for (const auto& [f, limit] : std::initializer_list<std::pair<double, double>> { { 60.0, -40.0 }, { 100.0, -50.0 }, { 1000.0, -50.0 } })
    {
        const auto in = tone (sr, 2.0, f, -6.0);
        int lat = 0;
        const auto out = process (in, withCeiling (-12.0f), sr, 512, &lat);
        const double pk = toDb (peak (out.data() + 48000, 24000));
        check (std::abs (pk + 12.0) <= 0.2, "Ceiling -12 dBFS, steady " + std::to_string ((int) f) + " Hz at -6 dBFS: peak against the ceiling", pk + 12.0, "dB");
        const double beyond = beyondLevelDb (in, out, lat, 48000, 24000);
        check (beyond <= limit, "Ceiling -12 dBFS, steady " + std::to_string ((int) f) + " Hz at -6 dBFS: everything other than level", beyond, "dB");
    }
    // Gain never jumps much at a hard onset: at most 1 dB per sample alone, at most 3 dB per sample with Attack +12, where the look ahead can't see the boost in time.
    for (const auto& [attackDb, limit] : std::initializer_list<std::pair<float, double>> { { 0.0f, 1.0 }, { 12.0f, 3.0 } })
    {
        const auto in = burst (sr, 1.0);
        Settings s = withCeiling (-12.0f); s.attack = attackDb;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        double worst = 0, prev = 0; bool have = false;
        for (int i = 0; i + lat < (int) in.size(); ++i)
        {
            if (std::abs (in[(size_t) i]) < 0.01f) { have = false; continue; }
            const double g = toDb (std::abs (out[(size_t) (i + lat)] / in[(size_t) i]));
            if (have) worst = std::max (worst, std::abs (g - prev));
            prev = g; have = true;
        }
        check (worst <= limit, "Ceiling -12 dBFS, hard onset, Attack " + std::to_string ((int) attackDb) + " dB: largest gain move in one sample", worst, "dB");
    }
    // Lets go: after a hit 12 dB over, gain is back within 1 dB by 250 ms.
    {
        const int hitEnd = (int) (sr * 0.5), n = (int) (sr * 1.5);
        std::vector<float> in ((size_t) n);
        for (int i = 0; i < n; ++i)
            in[(size_t) i] = (float) (juce::Decibels::decibelsToGain (i < hitEnd ? 0.0 : -24.0) * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / sr));
        int lat = 0;
        const auto out = process (in, withCeiling (-12.0f), sr, 512, &lat);
        double back = 1.0e6;
        for (int i = hitEnd; i + 48 + lat < n; i += 48)
            if (windowRmsDb (out, i + lat, 48) - windowRmsDb (in, i, 48) >= -1.0) { back = (i - hitEnd) / sr * 1000.0; break; }
        check (back <= 250.0, "Ceiling -12 dBFS: gain back within 1 dB after a hit", back, "ms");
    }
    // Ceiling On off: the ceiling value does nothing.
    {
        const auto in = burst (sr, 1.0);
        Settings s; s.ceiling = -24.0f; s.ceilingOn = 0.0f;
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const int len = (int) in.size() - lat;
        const double d = nullDb (out, in, lat, 0, len, rms (in.data(), len));
        check (d <= -120.0, "Ceiling On off, Ceiling -24 dBFS: difference from the delayed input", d, "dB");
    }
    // Latency stays what it was with everything on, and an impulse comes out where it says.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        std::vector<float> in ((size_t) rate / 2, 0.0f);
        in[100] = 0.25f;
        Settings s { 0.0f, 0.0f, 0.0f, 0.0f, 12.0f, 80.0f, -1.0f, 1.0f };
        int lat = 0;
        const auto out = process (in, s, rate, 128, &lat);
        int found = -1;
        for (int i = 0; i < (int) out.size(); ++i) if (std::abs (out[(size_t) i]) > 0.25f) { found = i - 100; break; }
        check (found == lat && lat == (int) std::lround (rate * 0.002), "Gain, Impact and Ceiling on: impulse delay equals the same latency at " + std::to_string ((int) rate) + " Hz", found, "samples");
    }
}

// Stereo stays linked and every block size agrees with all the new parts on.
static void testNewPartsStereoAndBlocks()
{
    const double sr = 48000.0;
    const auto left = speechShaped (sr, 3.0);
    Settings s { 20.0f, 3.0f, -2.0f, 100.0f, 6.0f, 60.0f, -6.0f, 1.0f };
    PluginProcessor p;
    apply (p, s);
    prepare (p, 2, sr, 512);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    double worst = 0, leak = 0;
    for (size_t pos = 0; pos + 512 <= left.size(); pos += 512)
    {
        for (int i = 0; i < 512; ++i) { buf.setSample (0, i, left[pos + (size_t) i]); buf.setSample (1, i, 0.5f * left[pos + (size_t) i]); }
        p.processBlock (buf, midi);
        for (int i = 0; i < 512; ++i)
            if (std::abs (buf.getSample (0, i)) > 1.0e-3f)
                worst = std::max (worst, std::abs (toDb (std::abs (buf.getSample (1, i) / buf.getSample (0, i))) + 6.0206));
    }
    check (worst <= 0.01, "stereo with Gain, Impact and Ceiling on: left to right balance, biggest move", worst, "dB");
    PluginProcessor q;
    apply (q, s);
    prepare (q, 2, sr, 512);
    for (size_t pos = 0; pos + 512 <= left.size(); pos += 512)
    {
        buf.copyFrom (0, 0, left.data() + pos, 512);
        buf.clear (1, 0, 512);
        q.processBlock (buf, midi);
        leak = std::max (leak, peak (buf.getReadPointer (1), 512));
    }
    check (leak == 0.0, "stereo with Gain, Impact and Ceiling on: silence on one side stays silent", leak);

    const auto a = process (left, s, sr, 512);
    for (int block : { 1, 32, 777, 4096 })
    {
        const auto b = process (left, s, sr, block);
        const double d = nullDb (a, b, 0, 0, (int) left.size(), rms (left.data(), (int) left.size()));
        check (d <= -100.0, "Gain, Impact and Ceiling on, block size " + std::to_string (block) + " against 512: difference", d, "dB");
    }
    // Mono path.
    PluginProcessor m;
    apply (m, s);
    prepare (m, 1, sr, 512);
    auto mo = render (m, left, 512);
    std::vector<float> mono (mo.getReadPointer (0), mo.getReadPointer (0) + mo.getNumSamples());
    const double md = nullDb (a, mono, 0, 0, (int) left.size(), rms (left.data(), (int) left.size()));
    check (md <= -100.0, "mono against stereo with identical channels: difference", md, "dB");
}

// The readings, read the way a host reads them: through the parameter's value string. They never write automation and never land in saved state.
static void testMeters()
{
    const double sr = 48000.0;
    struct Counter final : juce::AudioProcessorListener
    {
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        int changes = 0;
    };
    const auto run = [&] (const Settings& s, double db, PluginProcessor& p)
    {
        apply (p, s);
        prepare (p, 2, sr, 512);
        render (p, tone (sr, 3.0, 1000.0, db), 512);
    };
    {
        PluginProcessor p;
        Settings s; s.gain = 6.0f;
        Counter counter;
        apply (p, s);
        prepare (p, 2, sr, 512);
        p.addListener (&counter);
        render (p, tone (sr, 3.0, 1000.0, -12.0), 512);
        p.removeListener (&counter);
        const float in = readingFromText (*p.inputLevel), out = readingFromText (*p.outputLevel);
        check (std::abs (in + 12.0f) <= 0.2f, "Input Level with a -12 dBFS tone and Gain +6: reads before Gain", in, "dBFS");
        check (std::abs (out + 6.0f) <= 0.2f, "Output Level with a -12 dBFS tone and Gain +6: reads after everything", out, "dBFS");
        check (counter.changes == 0, "readings write no automation during audio", counter.changes, "changes");
    }
    {
        // Curve for -6 dBFS at Impact 50: threshold -21, ratio 3.5, so 10.7 dB. Reading within 1.5 dB.
        PluginProcessor p;
        run (impactOnly (50.0f), -6.0, p);
        const float r = readingFromText (*p.impactReduction);
        check (std::abs (r + 10.7f) <= 1.5f, "Impact Reduction, Impact 50 with a -6 dBFS tone", r, "dB");
        check (readingFromText (*p.ceilingReduction) == 0.0f, "Ceiling Reduction with Ceiling On off", readingFromText (*p.ceilingReduction), "dB");
    }
    {
        PluginProcessor p;
        run (withCeiling (-12.0f), -6.0, p);
        const float r = readingFromText (*p.ceilingReduction);
        check (std::abs (r + 6.0f) <= 0.5f, "Ceiling Reduction, Ceiling -12 dBFS with a -6 dBFS tone", r, "dB");
        const float o = readingFromText (*p.outputLevel);
        check (std::abs (o + 12.0f) <= 0.2f, "Output Level sits on the ceiling", o, "dBFS");
    }
    {
        // Falls back when the sound stops: under -50 dBFS after 3 s of silence.
        PluginProcessor p;
        run (Settings {}, -6.0, p);
        render (p, std::vector<float> ((size_t) (sr * 3), 0.0f), 512);
        check (readingFromText (*p.inputLevel) <= -50.0f, "Input Level after 3 s of silence", readingFromText (*p.inputLevel), "dBFS");
        // Hosts can't set a reading, and it stays out of saved state.
        p.inputLevel->setValueNotifyingHost (1.0f);
        check (readingFromText (*p.inputLevel) <= -50.0f, "Input Level ignores a write from a host", readingFromText (*p.inputLevel), "dBFS");
        juce::MemoryBlock st;
        p.getStateInformation (st);
        bool found = false;
        if (auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize()))
            found = xml->toString().contains ("inputLevel") || xml->toString().contains ("impactReduction");
        check (! found, "readings are not saved in state");
    }
}

// Nothing already in Luke's sessions may move. Compares against numbers recorded from the 0.1.0 code before any of this was written.
static void testSameAsFirstVersion()
{
    const double sr = 48000.0;
    auto in = burstTrain (sr, 0.5, 2.0, Tone::noise, 0.25, 0.08);
    const auto low = burstTrain (sr, 0.25, 2.0, Tone::sine, 0.33, 0.12, 180.0);
    for (size_t i = 0; i < in.size(); ++i) in[i] += low[i];
    for (int preset = 0; preset < 7; ++preset)
    {
        PluginProcessor p;
        p.setCurrentProgram (preset);
        const juce::String name = p.getProgramName (preset);
        prepare (p, 2, sr, 512);
        auto out = render (p, in, 512);
        double worst = 0.0;
        for (int w = 0; w < 200; ++w)
            worst = std::max (worst, std::abs (toDb (rms (out.getReadPointer (0) + w * 480, 480)) - goldenV010[preset][w]));
        check (worst <= 0.001, "preset " + name.toStdString() + " against version 0.1.0: largest 10 ms level difference", worst, "dB");
    }
    // A session saved by 0.1.0 has none of the new values. It loads as it sounded, even over a copy that has them all turned on.
    {
        juce::XmlElement xml ("STATE");
        for (const auto& [id, v] : std::initializer_list<std::pair<const char*, float>> { { "punch", 30.0f }, { "attack", 2.5f }, { "sustain", 0.0f }, { "mix", 100.0f } })
        {
            auto* e = xml.createNewChildElement ("PARAM");
            e->setAttribute ("id", id);
            e->setAttribute ("value", (double) v);
        }
        juce::MemoryBlock b;
        juce::AudioProcessor::copyXmlToBinary (xml, b);
        PluginProcessor p;
        setParam (p, "gain", 12.0f); setParam (p, "impact", 80.0f); setParam (p, "ceilingOn", 1.0f);
        p.setStateInformation (b.getData(), (int) b.getSize());
        prepare (p, 2, sr, 512);
        auto out = render (p, in, 512);
        double worst = 0.0;
        for (int w = 0; w < 200; ++w)
            worst = std::max (worst, std::abs (toDb (rms (out.getReadPointer (0) + w * 480, 480)) - goldenV010[1][w]));
        check (worst <= 0.001, "state saved by 0.1.0 with the Vocal Forward values: largest 10 ms level difference", worst, "dB");
    }
}

// New presets: all hold their ceiling on hot material, and all leave a quiet voice level at least as high as it came in.
static void testNewPresets()
{
    const double sr = 48000.0;
    const auto hot = speechShaped (sr, 5.0);
    for (int preset = 7; preset < PluginProcessor().getNumPrograms(); ++preset) // every preset from 0.2.0 on, the six lead presets of 0.5.0 too
    {
        PluginProcessor p;
        p.setCurrentProgram (preset);
        const juce::String name = p.getProgramName (preset);
        const float ceilingDb = p.apvts.getRawParameterValue ("ceiling")->load();
        check (p.apvts.getRawParameterValue ("ceilingOn")->load() > 0.5f, "preset " + name.toStdString() + ": Ceiling On");
        prepare (p, 2, sr, 512);
        auto out = render (p, hot, 512);
        const double over = toDb (peak (out.getReadPointer (0), out.getNumSamples())) - ceilingDb;
        check (over <= 0.01, "preset " + name.toStdString() + ": peak over its ceiling on hot material", over, "dB");

        PluginProcessor q;
        q.setCurrentProgram (preset);
        prepare (q, 2, sr, 512);
        const auto quiet = tone (sr, 3.0, 1000.0, -30.0);
        auto qo = render (q, quiet, 512);
        const double rise = rmsDb (qo, 0, 96000, 24000) - windowRmsDb (quiet, 96000, 24000);
        check (rise >= -0.1, "preset " + name.toStdString() + ": level change on a -30 dBFS tone", rise, "dB");
    }
}

// ---- the window ----------------------------------------------------------------------------------------------------
// All limits below were written before the first run of these tests.

namespace
{
    struct ParamWatch final : juce::AudioProcessorParameter::Listener
    {
        explicit ParamWatch (juce::AudioProcessor& proc) : p (proc)
        {
            for (auto* prm : p.getParameters())
                prm->addListener (this);
        }
        ~ParamWatch() override
        {
            for (auto* prm : p.getParameters())
                prm->removeListener (this);
        }
        void parameterValueChanged (int, float) override { ++changes; }
        void parameterGestureChanged (int, bool starting) override { if (starting) ++begins; else ++ends; }
        void reset() { changes = begins = ends = 0; }

        juce::AudioProcessor& p;
        std::atomic<int> changes { 0 }, begins { 0 }, ends { 0 };
    };

    juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> at, int clicks = 1, juce::ModifierKeys mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), juce::Point<float> downAt = { -1.0f, -1.0f })
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now,
                                 downAt.x < 0.0f ? at : downAt, now, clicks, downAt.x >= 0.0f);
    }

    float realValue (PluginProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

    void paintAt (juce::Component& c, float scale)
    {
        auto snapshot = c.createComponentSnapshot (c.getLocalBounds(), true, scale);
        juce::ignoreUnused (snapshot);
    }

    std::vector<float> loadSpeech (const char* name)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (SMOOTH_PUNCH_CORPUS).getChildFile (name)));
        std::vector<float> x;
        if (reader == nullptr)
            return x;
        juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
        reader->read (&b, 0, b.getNumSamples(), 0, true, false);
        x.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        return x;
    }

    // Runs audio through a plug-in and everything the tap collected through a tracker. The last hit it caught comes back in the tracker.
    void captureHits (PluginProcessor& p, const std::vector<float>& x, punchview::HitTracker& tracker, int& hits)
    {
        std::uint32_t last = p.envelopeTap.getWritten();
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        hits = 0;
        for (int pos = 0; pos < (int) x.size(); pos += 512)
        {
            const int n = juce::jmin (512, (int) x.size() - pos);
            buffer.setSize (2, n, false, false, true);
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, x.data() + pos, n);
            p.processBlock (buffer, midi);
            for (const auto w = p.envelopeTap.getWritten(); last != w; ++last)
                if (tracker.feed (p.envelopeTap.before (last), p.envelopeTap.after (last)))
                    ++hits;
        }
        tracker.latest().smooth();
    }

    std::vector<float> burstTrain (double sr, double amp, int count, double spacing = 1.0)
    {
        std::vector<float> x;
        for (int i = 0; i < count; ++i)
        {
            auto one = burst (sr, amp, 300.0, 0.1, Tone::sine, spacing);
            x.insert (x.end(), one.begin(), one.end());
        }
        return x;
    }

    double shapeDifferenceDb (const punchview::HitShape& hit, double fromSeconds, double toSeconds)
    {
        const int from = (int) (fromSeconds / punchview::binSeconds), to = juce::jmin (hit.length, (int) (toSeconds / punchview::binSeconds));
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += (double) punchview::toDb (hit.after[(size_t) i]) - (double) punchview::toDb (hit.before[(size_t) i]);
        return sum / juce::jmax (1, to - from);
    }
}


// ---------------------------------------------------------------------------------------------------------------------------------
// 0.5.0. Steady Notes follows the note. Every limit below was written before the first run, from the limits of the real vocal checks
// (~/smooth-plugins/_shared/realvox/src/extra/punch.cpp), on a made-up sung line that has exactly the parts a real one has.
namespace
{
    // Times in seconds. A room (hiss at -70 dBFS and a 120 Hz hum at -64 dBFS) runs under everything.
    //   0.30 to 1.50   note A: 196 Hz with vibrato, its level swinging 3 dB at 5 Hz, soft start. The voice stops in about 15 ms.
    //   1.50 to 1.66   what is left of A: a dull breathy sound 20 dB under the note, dying away
    //   1.75 to 2.15   a breath, 26 dB under the note, 250 ms after it
    //   2.35           a mouth sound: a 2 ms tick, 30 dB under the note
    //   2.60 to 3.40   note B, 10 dB quieter than A, hard start; at 3.00 it drops 5 dB and holds (two syllables on one breath)
    //   3.46 to 3.86   a breath straight after B
    //   3.90 to 4.40   the room alone
    std::vector<float> sungLine (double sr)
    {
        const double twoPi = juce::MathConstants<double>::twoPi, pi = juce::MathConstants<double>::pi;
        const int n = (int) (sr * 4.4);
        std::vector<float> x ((size_t) n, 0.0f);
        std::mt19937 g (21); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = 0.0003f * d (g) + (float) (0.0006 * std::sin (twoPi * 120.0 * i / sr));

        const auto voiced = [&] (double from, double to, double f0, double swingDb, bool soft, const std::function<double (double)>& amp)
        {
            double phase = 0.0;
            for (int i = (int) (from * sr); i < (int) ((to + 0.06) * sr) && i < n; ++i)
            {
                const double t = i / sr - from;
                phase += twoPi * f0 * std::pow (2.0, 0.5 / 12.0 * std::sin (twoPi * 5.5 * t)) / sr;
                double env = 1.0;
                if (soft && t < 0.03) env = 0.5 - 0.5 * std::cos (pi * t / 0.03);
                if (i / sr > to) env = std::exp (-(i / sr - to) / 0.004);
                const double swing = std::pow (10.0, 0.5 * swingDb * std::sin (twoPi * 5.0 * t) / 20.0);
                double v = 0.0;
                for (int h = 1; h <= 6; ++h) v += std::sin (h * phase) / h;
                x[(size_t) i] += (float) (amp (t) * env * swing * v);
            }
        };
        // Dull noise through a one pole low pass. slow: rises and falls like a breath. Otherwise it starts at once and dies away.
        const auto airy = [&] (double from, double to, double amp, double cutoff, bool slow)
        {
            const double k = 1.0 - std::exp (-twoPi * cutoff / sr);
            double lp = 0.0;
            for (int i = (int) (from * sr); i < (int) (to * sr) && i < n; ++i)
            {
                const double t = i / sr - from, len = to - from;
                lp += k * ((double) d (g) - lp);
                const double env = slow ? 0.5 - 0.5 * std::cos (twoPi * t / len) : std::exp (-t / (len / 2.0));
                x[(size_t) i] += (float) (amp * 3.0 * env * lp);
            }
        };
        voiced (0.3, 1.5, 196.0, 3.0, true, [] (double) { return 0.2; });
        airy (1.5, 1.66, 0.02, 1500.0, false);
        airy (1.75, 2.15, 0.01, 2500.0, true);
        for (int i = (int) (2.35 * sr); i < (int) (2.352 * sr); ++i)
            x[(size_t) i] += (float) (0.02 * std::exp (-(i / sr - 2.35) / 0.0005) * d (g));
        voiced (2.6, 3.4, 220.0, 0.0, false, [] (double t) { return 0.063 * (t < 0.4 ? 1.0 : (t < 0.42 ? 1.0 - 0.44 * (t - 0.4) / 0.02 : 0.56)); });
        airy (3.46, 3.86, 0.004, 2500.0, true);
        return x;
    }

    struct LineNumbers { double held, tail, breath, tick, startB, legato, breathAfter, room; };

    LineNumbers measureLine (const std::vector<float>& in, const Settings& s, double sr = 48000.0)
    {
        int lat = 0;
        const auto out = process (in, s, sr, 512, &lat);
        const auto change = [&] (double from, double to)
        {
            const int a = (int) (from * sr), len = (int) ((to - from) * sr);
            return windowRmsDb (out, a + lat, len) - windowRmsDb (in, a, len);
        };
        const auto worst = [&] (double from, double to)
        {
            double w = 0.0;
            for (double t = from; t + 0.01 <= to + 1.0e-9; t += 0.01) w = std::max (w, std::abs (change (t, t + 0.01)));
            return w;
        };
        return { worst (0.5, 1.4), change (1.52, 1.65), change (1.75, 2.15), change (2.348, 2.355), change (2.6, 2.615), worst (3.02, 3.38), change (3.46, 3.86), change (3.95, 4.35) };
    }
}

static void testSteadyVoice()
{
    const double sr = 48000.0;
    const auto line = sungLine (sr);
    for (float v : { 9.0f, -9.0f })
    {
        Settings s; s.sustain = v; s.steadyNotes = 1.0f;
        const auto m = measureLine (line, s);
        const std::string tag = "Steady Notes, Sustain " + std::to_string ((int) v) + " on a sung line: ";
        check (m.held <= 1.0, tag + "largest change of the held note", m.held, "dB");
        check (v > 0 ? m.tail >= 4.5 : m.tail <= -4.5, tag + "the 20 to 150 ms after the voice stops", m.tail, "dB");
        check (std::abs (m.startB) <= 1.0, tag + "first 15 ms of the next, quieter note", m.startB, "dB");
        check (std::abs (m.breath) <= 1.0, tag + "a breath a quarter of a second after the note", m.breath, "dB");
        check (std::abs (m.room) <= 0.5, tag + "the room and its hum on their own", m.room, "dB");
        Settings off = s; off.steadyNotes = 0.0f;
        const auto o = measureLine (line, off);
        info ("  without Steady Notes, the same: held note", o.held, "dB");
        info ("  without Steady Notes, the same: after the voice stops", o.tail, "dB");
        info ("  without Steady Notes, the same: the breath", o.breath, "dB");
    }
    for (float v : { 6.0f, -6.0f })
    {
        Settings s; s.sustain = v; s.steadyNotes = 1.0f;
        const auto m = measureLine (line, s);
        const std::string tag = "Steady Notes, Sustain " + std::to_string ((int) v) + " on a sung line: ";
        check (m.legato <= 1.0, tag + "largest change of a syllable that is 5 dB quieter than the one before, on the same breath", m.legato, "dB");
        if (v > 0)
        {
            check (m.breath <= 1.0, tag + "a breath a quarter of a second after the note", m.breath, "dB");
            check (m.breathAfter <= 3.0, tag + "a breath straight after the note, all of it", m.breathAfter, "dB");
            check (std::abs (m.tick) <= 1.0, tag + "a mouth sound between phrases", m.tick, "dB");
        }
    }
    // Other rates: the tail is still found and the held note still left alone.
    for (double rate : { 44100.0, 96000.0 })
    {
        Settings s; s.sustain = 9.0f; s.steadyNotes = 1.0f;
        const auto m = measureLine (sungLine (rate), s, rate);
        const std::string tag = "Steady Notes, Sustain 9 at " + std::to_string ((int) rate) + " Hz: ";
        check (m.held <= 1.0, tag + "largest change of the held note", m.held, "dB");
        check (m.tail >= 4.5, tag + "the 20 to 150 ms after the voice stops", m.tail, "dB");
        check (std::abs (m.breath) <= 1.0, tag + "a breath a quarter of a second after the note", m.breath, "dB");
    }
    // Steady Notes with nothing else set changes nothing at all.
    {
        Settings s; s.steadyNotes = 1.0f;
        int lat = 0;
        const auto out = process (line, s, sr, 512, &lat);
        const double residual = nullDb (out, line, lat, 0, (int) line.size() - lat, rms (line.data(), (int) line.size()));
        check (residual <= -120.0, "Steady Notes on with every other control at its default: difference from the input", residual, "dB");
    }
}

// The pitch sense on its own: a voice is found within 80 ms and lost within 40 ms of its end; noise and silence have no pitch.
static void testPitchSense()
{
    const double sr = 48000.0, twoPi = juce::MathConstants<double>::twoPi;
    for (double f0 : { 82.0, 110.0, 440.0, 880.0 })
    {
        PitchSense ps;
        ps.prepare (sr);
        int foundAt = -1, lostAt = -1;
        const int on = (int) (0.2 * sr), off = (int) (0.8 * sr), n = (int) (1.0 * sr);
        for (int i = 0; i < n; ++i)
        {
            double v = 0.0;
            if (i >= on && i < off)
                for (int h = 1; h <= 5; ++h) v += 0.1 * std::sin (twoPi * f0 * h * (i - on) / sr) / h;
            ps.push ((float) v);
            if (foundAt < 0 && ps.voiced) foundAt = i;
            if (foundAt >= 0 && lostAt < 0 && i >= off && ! ps.voiced) lostAt = i;
        }
        const std::string tag = "pitch sense, " + std::to_string ((int) f0) + " Hz: ";
        check (foundAt >= on && foundAt <= on + (int) (0.08 * sr), tag + "found after", foundAt >= 0 ? 1000.0 * (foundAt - on) / sr : -1.0, "ms");
        check (lostAt >= off && lostAt <= off + (int) (0.04 * sr), tag + "lost after the end", lostAt >= 0 ? 1000.0 * (lostAt - off) / sr : -1.0, "ms");
    }
    {
        PitchSense ps;
        ps.prepare (sr);
        std::mt19937 g (9); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        long voiced = 0; const long n = (long) (4.0 * sr);
        for (long i = 0; i < n; ++i) { ps.push (0.1f * d (g)); voiced += ps.voiced ? 1 : 0; }
        check (100.0 * voiced / n <= 2.0, "pitch sense, white noise: share of the time it finds a pitch", 100.0 * voiced / n, "%");
        PitchSense quiet;
        quiet.prepare (sr);
        long any = 0;
        for (long i = 0; i < (long) sr; ++i) { quiet.push (0.0f); any += quiet.voiced ? 1 : 0; }
        check (any == 0, "pitch sense, silence: no pitch", (double) any);
    }
}

// Punch with Steady Notes: a held note whose peaks move while its level does not must keep its level, from the first word of a phrase on.
// The note: three harmonics whose phases turn against each other, so the crest factor moves and the level over 30 ms does not. Two
// phrases with a pause between; the second is measured, from 150 ms after its start (the first is where the level match is learned).
static void testSteadyPunch()
{
    const double sr = 48000.0, twoPi = juce::MathConstants<double>::twoPi, pi = juce::MathConstants<double>::pi;
    const int n = (int) (sr * 4.9);
    std::vector<float> in ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const bool first = t >= 0.3 && t < 1.8, second = t >= 2.6 && t < 4.6;
        if (! first && ! second) continue;
        const double u = t - (first ? 0.3 : 2.6), turn = 0.5 * pi * (1.0 - std::cos (twoPi * 1.5 * u));
        const double p = twoPi * 150.0 * u;
        in[(size_t) i] = (float) (0.15 * (std::sin (p) + 0.8 * std::sin (2.0 * p + turn) + 0.6 * std::sin (3.0 * p + 2.0 * turn)));
    }
    for (float amount : { 50.0f, 100.0f })
        for (float on : { 1.0f, 0.0f })
        {
            Settings s; s.punch = amount; s.steadyNotes = on;
            int lat = 0;
            const auto out = process (in, s, sr, 512, &lat);
            double inLo = 1e9, inHi = -1e9, outLo = 1e9, outHi = -1e9;
            for (int w = 275; w + 3 <= 455; ++w) // 2.75 to 4.55 s, 30 ms windows every 10 ms
            {
                const double a = windowRmsDb (in, w * 480, 1440), b = windowRmsDb (out, w * 480 + lat, 1440);
                inLo = std::min (inLo, a); inHi = std::max (inHi, a); outLo = std::min (outLo, b); outHi = std::max (outHi, b);
            }
            const double added = (outHi - outLo) - (inHi - inLo);
            const double level = windowRmsDb (out, (int) (2.75 * sr) + lat, (int) (1.8 * sr)) - windowRmsDb (in, (int) (2.75 * sr), (int) (1.8 * sr));
            const std::string tag = "Punch " + std::to_string ((int) amount) + (on > 0.5f ? " with Steady Notes" : " without Steady Notes") + ", a held note whose peaks move: ";
            if (on > 0.5f)
            {
                check (added <= 1.5, tag + "level swing added", added, "dB");
                check (std::abs (level) <= 1.0, tag + "level against the input", level, "dB");
            }
            else
            {
                info (tag + "level swing added", added, "dB");
                info (tag + "level against the input", level, "dB");
            }
        }
}

// Attack with Steady Notes: a jump of the peaks inside a held note is not a start. The note: six harmonics of 150 Hz whose phases change at
// one moment, so the peaks rise by about 3 dB while the level stays where it is (on Luke's takes such jumps were 1.6 to 4.8 dB). Limits
// written before the first run: with Steady Notes the 10 ms around the jump move by under 1 dB at Attack +9 and -9, and the start of the
// note itself still moves by at least 6 dB of the 9, as in the Attack test.
static void testSteadyAttack()
{
    const double sr = 48000.0, twoPi = juce::MathConstants<double>::twoPi;
    const int n = (int) (sr * 2.0), on = (int) (sr * 0.3), jump = (int) (sr * 1.0);
    std::vector<float> in ((size_t) n, 0.0f);
    for (int i = on; i < (int) (sr * 1.7); ++i)
    {
        const double p = twoPi * 150.0 * (i - on) / sr, k = i < jump ? 0.5236 : 0.2;
        double v = 0.0;
        for (int h = 1; h <= 6; ++h) v += std::cos (h * p + k * h * h);
        in[(size_t) i] = (float) (0.03 * v);
    }
    info ("a held note whose peaks jump: the jump of the peak", toDb (peak (in.data() + jump, 4800)) - toDb (peak (in.data() + jump - 4800, 4800)), "dB");
    info ("a held note whose peaks jump: the jump of the level", windowRmsDb (in, jump, 4800) - windowRmsDb (in, jump - 4800, 4800), "dB");
    for (float v : { 9.0f, -9.0f })
        for (float steady : { 1.0f, 0.0f })
        {
            Settings s; s.attack = v; s.steadyNotes = steady;
            int lat = 0;
            const auto out = process (in, s, sr, 512, &lat);
            double worst = 0.0;
            for (int w = -5; w < 30; ++w) // 50 ms before the jump to 300 ms after, 10 ms at a time
                worst = std::max (worst, std::abs (windowRmsDb (out, jump + w * 480 + lat, 480) - windowRmsDb (in, jump + w * 480, 480)));
            const double start = windowRmsDb (out, on + lat, 144) - windowRmsDb (in, on, 144);
            const std::string tag = std::string ("Attack ") + std::to_string ((int) v) + (steady > 0.5f ? " with Steady Notes" : " without Steady Notes") + ", a held note whose peaks jump: ";
            if (steady > 0.5f)
            {
                check (worst <= 1.0, tag + "largest change around the jump", worst, "dB");
                check (start * (v > 0 ? 1.0 : -1.0) >= 6.0, tag + "first 3 ms of the note", start, "dB");
            }
            else
            {
                info (tag + "largest change around the jump", worst, "dB");
                info (tag + "first 3 ms of the note", start, "dB");
            }
        }
}

// Off stays off. With Steady Notes off the detector is the one 0.4.0 shipped, sample for sample: both run side by side on speech and on
// bursts at the settings of each of the 14 presets that were there before 0.5.0, and at everything turned up. Not one gain may differ.
static void testOffStaysOff()
{
    const double sr = 48000.0;
    auto sound = speechShaped (sr, 6.0);
    const auto bursts = burstTrain (sr, 0.5, 2.0, Tone::noise, 0.25, 0.08);
    sound.insert (sound.end(), bursts.begin(), bursts.end());
    struct Case { std::string name; float punch, attack, sustain, impact, gain; };
    std::vector<Case> cases;
    for (int i = 0; i < 14; ++i)
    {
        PluginProcessor p;
        p.setCurrentProgram (i);
        const auto v = [&p] (const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        cases.push_back ({ p.getProgramName (i).toStdString(), v ("punch") * 0.01f, v ("attack"), v ("sustain"), v ("impact") * 0.01f, v ("gain") });
    }
    cases.push_back ({ "everything turned up", 1.0f, 15.0f, 15.0f, 1.0f, 12.0f });
    cases.push_back ({ "everything turned down", 0.37f, -15.0f, -15.0f, 0.5f, -12.0f });
    long differing = 0, compared = 0;
    for (const auto& c : cases)
    {
        PunchEngine now;
        v040::PunchEngine then;
        now.prepare (sr); then.prepare (sr);
        now.reset (c.punch); then.reset (c.punch);
        const float inGain = PunchEngine::dbToGain (c.gain);
        long here = 0;
        for (float x : sound)
        {
            const float a = now.process (std::abs (x) * inGain, c.attack, c.sustain, c.punch, c.impact, false, x);
            const float b = then.process (std::abs (x) * inGain, c.attack, c.sustain, c.punch, c.impact, false, x);
            if (a != b || now.impactReductionNowDb() != then.impactReductionNowDb()) ++here;
            ++compared;
        }
        differing += here;
        if (here != 0) check (false, "Steady Notes off, " + c.name + ": gains that differ from 0.4.0's detector", (double) here);
    }
    check (differing == 0 && compared > 5000000, "Steady Notes off: the detector's gain is 0.4.0's, sample for sample, at all 14 older presets and both extremes; samples that differ", (double) differing);
    // And it comes back to that after Steady Notes has been on for a while and is turned off again, once the old envelopes have settled.
    {
        PunchEngine now;
        v040::PunchEngine then;
        now.prepare (sr); then.prepare (sr);
        now.reset (0.3f); then.reset (0.3f);
        long late = 0;
        for (size_t i = 0; i < sound.size(); ++i)
        {
            const bool steady = i > sound.size() / 6 && i < sound.size() / 3;
            const float a = now.process (std::abs (sound[i]), 2.5f, 6.0f, 0.3f, 0.0f, steady, sound[i]);
            const float b = then.process (std::abs (sound[i]), 2.5f, 6.0f, 0.3f, 0.0f, false, sound[i]);
            if (i > sound.size() * 2 / 3 && std::abs (a - b) > 0.01f) ++late;
        }
        check (late == 0, "Steady Notes switched on and off again: 2.5 s later the gain is within 0.01 dB of a detector that never had it on; samples further off", (double) late);
    }
}

// The six presets 0.5.0 added for a lead vocal: at the end of the list, Steady Notes on, the values written here, names that are new.
// 0.5.1. A held note that starts loud and settles 10 dB lower. Found on Luke's takes (blind take 15 missed the held vowel limit, Sustain 9:
// mean gain 0.25 dB, limit 0.2; the same thing on working take 04 at 149.86 s, 7.6 dB for 30 ms, and take 02 at 223.13 s). Inside a note,
// Sustain judged a sag against its slow envelope, which was still coming down from the loud start, so a small dip of the settled note read
// as a note dying away. Limits are the real vocal check's held vowel limits, written before the first run: on the settled part, every
// 10 ms no more than 1.0 dB from the input and the whole within 0.2 dB.
static void testHeldAfterLoudStart()
{
    for (double sr : { 48000.0, 44100.0, 96000.0 })
    {
        const double twoPi = juce::MathConstants<double>::twoPi;
        const int n = (int) (sr * 1.8);
        std::vector<float> x ((size_t) n, 0.0f);
        std::mt19937 g (5); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            x[(size_t) i] = 0.0003f * d (g);
            if (t < 0.3 || t > 1.5) continue;
            const double u = t - 0.3;
            // 0.3 to 0.4 s loud, then 10 dB down over 0.1 s, then held, its level swinging 3 dB at 5 Hz. The voice stops at 1.5 s.
            double ampDb = u < 0.1 ? 0.0 : (u < 0.2 ? -10.0 * (u - 0.1) / 0.1 : -10.0);
            if (u >= 0.2) ampDb += 1.5 * std::sin (twoPi * 5.0 * (u - 0.2));
            double env = std::pow (10.0, ampDb / 20.0) * (u < 0.02 ? u / 0.02 : 1.0) * (t > 1.496 ? (1.5 - t) / 0.004 : 1.0);
            phase += twoPi * 196.0 * std::pow (2.0, 0.4 / 12.0 * std::sin (twoPi * 5.5 * u)) / sr;
            double v = 0.0;
            for (int h = 1; h <= 6; ++h) v += std::sin (h * phase) / h;
            x[(size_t) i] += (float) (0.3 * env * v);
        }
        for (float sustain : { 9.0f, -9.0f, 6.0f })
        {
            if (sr != 48000.0 && sustain != 9.0f) continue;
            Settings s; s.sustain = sustain; s.steadyNotes = 1.0f;
            int lat = 0;
            const auto out = process (x, s, sr, 512, &lat);
            const int w = (int) (sr * 0.01);
            double worst = 0.0, ein = 0.0, eout = 0.0;
            for (int k = (int) (0.55 * sr); k + w <= (int) (1.45 * sr); k += w)
            {
                worst = std::max (worst, std::abs (windowRmsDb (out, k + lat, w) - windowRmsDb (x, k, w)));
                for (int j = 0; j < w; ++j) { ein += (double) x[(size_t) (k + j)] * x[(size_t) (k + j)]; eout += (double) out[(size_t) (k + j + lat)] * out[(size_t) (k + j + lat)]; }
            }
            const std::string tag = "Steady Notes, Sustain " + std::to_string ((int) sustain) + " at " + std::to_string ((int) sr) + " Hz, a note that starts loud and settles 10 dB lower: ";
            check (worst <= 1.0, tag + "largest change of the settled note, 10 ms at a time", worst, "dB");
            check (std::abs (10.0 * std::log10 (eout / ein)) <= 0.2, tag + "level of the settled note", 10.0 * std::log10 (eout / ein), "dB");
        }
    }
}

static void testLeadPresets()
{
    struct Want { const char* name; float punch, attack, sustain, mix, gain, impact, ceiling; bool ceilingOn; };
    const Want wants[] = {
        { "Lead Gentle",          12.0f,  1.5f,  0.0f, 100.0f, 0.0f,  0.0f, -1.0f, true },
        { "Lead Upfront",         20.0f,  2.5f,  0.0f, 100.0f, 0.0f, 35.0f, -1.0f, true },
        { "Lead Rap",             15.0f,  2.5f, -2.0f, 100.0f, 0.0f, 50.0f, -1.0f, true },
        { "Lead Soft Consonants", 10.0f, -5.0f,  1.5f, 100.0f, 0.0f,  0.0f, -1.0f, true },
        { "Lead Long Tails",      10.0f,  0.0f,  6.0f, 100.0f, 0.0f,  0.0f, -1.0f, true },
        { "Lead Dry And Close",   10.0f,  1.5f, -6.0f, 100.0f, 0.0f,  0.0f, -1.0f, true },
    };
    PluginProcessor p;
    check (p.getNumPrograms() == 20, "there are 20 presets: the 14 from before and six for a lead vocal", (double) p.getNumPrograms());
    const char* older[] = { "Neutral", "Vocal Forward", "Soft Consonants", "Dry And Close", "Longer Tails", "Drum Snap", "Parallel Weight", "Even Lead", "Rap Upfront",
                            "Quiet Take Lift", "Spoken Word", "Full Level Hook", "Parallel Impact", "Ceiling Only" };
    bool sameOrder = true;
    for (int i = 0; i < 14; ++i) sameOrder = sameOrder && p.getProgramName (i) == older[i];
    check (sameOrder, "the 14 older presets keep their numbers and names");
    const auto line = sungLine (48000.0);
    for (int i = 0; i < 6; ++i)
    {
        const auto& w = wants[i];
        p.setCurrentProgram (14 + i);
        const auto v = [&p] (const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        const bool values = std::abs (v ("punch") - w.punch) < 0.01f && std::abs (v ("attack") - w.attack) < 0.01f && std::abs (v ("sustain") - w.sustain) < 0.01f
                         && std::abs (v ("mix") - w.mix) < 0.01f && std::abs (v ("gain") - w.gain) < 0.01f && std::abs (v ("impact") - w.impact) < 0.01f
                         && std::abs (v ("ceiling") - w.ceiling) < 0.01f && (v ("ceilingOn") > 0.5f) == w.ceilingOn;
        check (p.getProgramName (14 + i) == w.name && values && v ("steadyNotes") > 0.5f && ! p.isCurrentProgramEdited(),
               std::string ("preset ") + std::to_string (14 + i) + " is " + w.name + ", with its values and Steady Notes on");
        // On the made-up sung line: no more than 1.5 dB of level swing added to the held note, nothing over 0 dBFS, no bad samples.
        Settings s; s.punch = w.punch; s.attack = w.attack; s.sustain = w.sustain; s.mix = w.mix; s.gain = w.gain; s.impact = w.impact; s.ceiling = w.ceiling;
        s.ceilingOn = w.ceilingOn ? 1.0f : 0.0f; s.steadyNotes = 1.0f;
        int lat = 0;
        const auto out = process (line, s, 48000.0, 512, &lat);
        // The level swing of the held note that the input does not have, as the real vocal check reads it: the swing of the output's
        // level less the swing of the input's. (A first version of this test read the swing of the gain, which a level rider such as
        // Impact has by design: it moves against the note's own swing and makes it smaller. Changed after its first run; the limit is the same.)
        double inLo = 1e9, inHi = -1e9, outLo = 1e9, outHi = -1e9;
        for (int k = 80; k + 3 <= 140; ++k) // 0.8 to 1.4 s of note A, 30 ms windows
        {
            const double a = windowRmsDb (line, k * 480, 1440), b = windowRmsDb (out, k * 480 + lat, 1440);
            inLo = std::min (inLo, a); inHi = std::max (inHi, a); outLo = std::min (outLo, b); outHi = std::max (outHi, b);
        }
        const double added = (outHi - outLo) - (inHi - inLo);
        bool finite = true;
        for (float y : out) finite = finite && std::isfinite (y);
        check (added <= 1.5, std::string ("preset ") + w.name + ": level swing added to the held note of the sung line", added, "dB");
        check (finite && peak (out.data(), (int) out.size()) <= 1.0, std::string ("preset ") + w.name + ": peak of the sung line", toDb (peak (out.data(), (int) out.size())), "dBFS");
    }
}

void testEnvelopeTap()
{
    constexpr double sr = 48000.0;
    const auto x = burstTrain (sr, 0.5, 2);

    // Neutral: before and after are the same sample, so they must match in every bin.
    {
        PluginProcessor p;
        prepare (p, 2, sr, 512);
        render (p, x, 512);
        double worst = 0.0;
        const auto w = p.envelopeTap.getWritten();
        for (std::uint32_t i = w - 2000u; i != w; ++i)
            worst = juce::jmax (worst, (double) std::abs (p.envelopeTap.before (i) - p.envelopeTap.after (i)));
        check (worst == 0.0, "the display's before and after match exactly at neutral, so they line up in time", worst);
    }

    // Gain moves after, not before.
    {
        PluginProcessor p;
        setParam (p, "gain", 6.0f);
        prepare (p, 2, sr, 512);
        render (p, x, 512);
        const auto w = p.envelopeTap.getWritten();
        double worst = 0.0;
        for (std::uint32_t i = w - 1000u; i != w; ++i)
            if (p.envelopeTap.before (i) > 1.0e-4f)
                worst = juce::jmax (worst, std::abs ((double) punchview::toDb (p.envelopeTap.after (i)) - (double) punchview::toDb (p.envelopeTap.before (i)) - 6.0));
        check (worst < 0.01, "Gain +6: after sits 6 dB over before in every bin, worst error", worst, "dB");
    }

    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        PluginProcessor p;
        prepare (p, 2, rate, 512);
        const auto start = p.envelopeTap.getWritten();
        render (p, std::vector<float> ((size_t) rate, 0.1f), 512);
        const double bins = (double) (p.envelopeTap.getWritten() - start);
        check (std::abs (bins - 2000.0) < 8.0, "about 2000 bins a second at " + std::to_string ((int) rate) + " Hz", bins);
    }

    // Mono has no second channel to read.
    {
        PluginProcessor p;
        prepare (p, 1, sr, 512);
        render (p, x, 512);
        const auto w = p.envelopeTap.getWritten();
        double high = 0.0;
        for (std::uint32_t i = w - 3000u; i != w; ++i)
            high = juce::jmax (high, (double) p.envelopeTap.after (i));
        check (p.envelopeTap.getWritten() > 3000u && high > 0.1 && high < 0.6, "the tap works in mono", high);
    }

    // A long run goes round the ring with no trouble.
    {
        PluginProcessor p;
        prepare (p, 2, sr, 512);
        std::vector<float> longer ((size_t) (sr * 12.0), 0.25f);
        render (p, longer, 512);
        const auto w = p.envelopeTap.getWritten();
        check (w > (std::uint32_t) EnvelopeTap::numBins && std::abs (p.envelopeTap.after (w - 1u) - 0.25f) < 1.0e-5f, "the tap goes round its ring and the newest bin is right", (double) w);
    }
}

void testHitDisplayNumbers()
{
    using namespace punchview;
    constexpr double sr = 48000.0;

    const auto shapeWith = [&] (float attack, float sustain, HitTracker& tracker)
    {
        PluginProcessor p;
        setParam (p, "attack", attack);
        setParam (p, "sustain", sustain);
        prepare (p, 2, sr, 512);
        int hits = 0;
        captureHits (p, burstTrain (sr, 0.5, 3), tracker, hits);
        return hits;
    };

    // What is drawn really moves when the controls move.
    {
        auto tracker = std::make_unique<HitTracker>();
        check (shapeWith (9.0f, 0.0f, *tracker) == 3, "3 bursts make 3 hits");
        const double start = shapeDifferenceDb (tracker->latest(), 0.002, 0.004), tail = shapeDifferenceDb (tracker->latest(), 0.1, 0.3);
        check (start > 6.0 && start < 10.0, "Attack +9: the drawn start rises", start, "dB");
        check (std::abs (tail) < 1.0, "Attack +9: the drawn tail stays where it was", tail, "dB");
    }
    {
        auto tracker = std::make_unique<HitTracker>();
        shapeWith (-9.0f, 0.0f, *tracker);
        const double start = shapeDifferenceDb (tracker->latest(), 0.002, 0.004);
        check (start < -6.0 && start > -10.0, "Attack -9: the drawn start falls", start, "dB");
    }
    {
        auto tracker = std::make_unique<HitTracker>();
        shapeWith (0.0f, -9.0f, *tracker);
        const double start = shapeDifferenceDb (tracker->latest(), 0.002, 0.004), tail = shapeDifferenceDb (tracker->latest(), 0.1, 0.3);
        check (tail < -6.0 && tail > -11.0, "Sustain -9: the drawn tail falls", tail, "dB");
        check (std::abs (start) < 1.5, "Sustain -9: the drawn start stays where it was", start, "dB");
    }
    {
        auto tracker = std::make_unique<HitTracker>();
        shapeWith (0.0f, 9.0f, *tracker);
        const double tail = shapeDifferenceDb (tracker->latest(), 0.1, 0.3);
        check (tail > 6.0 && tail < 11.0, "Sustain +9: the drawn tail rises", tail, "dB");
    }

    // What counts as a hit.
    const auto countHits = [&] (const std::vector<float>& x)
    {
        PluginProcessor p;
        prepare (p, 2, sr, 512);
        auto tracker = std::make_unique<HitTracker>();
        int hits = 0;
        captureHits (p, x, *tracker, hits);
        return hits;
    };
    check (countHits (burstTrain (sr, 0.03, 5)) == 5, "5 bursts at -30 dBFS make 5 hits, so level does not matter");
    check (countHits (tone (sr, 3.0, 1000.0, -12.0)) == 1, "a steady tone is one hit, when it starts");
    check (countHits (std::vector<float> ((size_t) sr, 0.0f)) == 0, "silence makes no hits");
    check (countHits (tone (sr, 2.0, 1000.0, -60.0)) == 0, "a tone at -60 dBFS makes no hits");
    {
        const auto speech = loadSpeech ("speech_samantha.wav");
        const int hits = countHits (speech);
        const double perSecond = hits / (speech.size() / sr);
        check (perSecond > 0.5 && perSecond <= 2.86, "real speech makes a readable number of hits a second, no more than one per 350 ms", perSecond);
    }

    // The preview on a stopped session is a close drawing of what really happens.
    const auto previewError = [&] (const punchview::Settings& s)
    {
        PluginProcessor p;
        apply (p, { s.punch, s.attack, s.sustain, s.mix, s.gain, s.impact, s.ceiling, s.ceilingOn ? 1.0f : 0.0f });
        prepare (p, 2, sr, 512);
        auto tracker = std::make_unique<HitTracker>();
        int hits = 0;
        captureHits (p, burstTrain (sr, 0.5, 4), *tracker, hits);
        auto& hit = tracker->latest();
        Preview preview;
        std::vector<float> drawn ((size_t) hit.length);
        preview.run (hit.before.data(), hit.length, s, drawn.data());
        double sum = 0.0;
        for (int i = 2; i < hit.length; ++i)
            sum += std::abs ((double) toDb (drawn[(size_t) i]) - (double) toDb (hit.after[(size_t) i]));
        return sum / (hit.length - 2);
    };
    const auto settings = [] (float punch, float attack, float sustain, float mix, float gain, float impact, float ceiling, bool on)
    {
        punchview::Settings s;
        s.punch = punch; s.attack = attack; s.sustain = sustain; s.mix = mix; s.gain = gain; s.impact = impact; s.ceiling = ceiling; s.ceilingOn = on;
        return s;
    };
    {
        const double a = previewError (settings (0, 9, 0, 100, 0, 0, -1, false));
        check (a < 2.0, "the preview against the real result, Attack +9, average error", a, "dB");
        const double b = previewError (settings (0, 0, -9, 100, 0, 0, -1, false));
        check (b < 2.0, "the preview against the real result, Sustain -9, average error", b, "dB");
        const double c = previewError (settings (30, 4, -3, 80, 3, 50, -6, true));
        check (c < 3.0, "the preview against the real result, everything on, average error", c, "dB");
    }
    {
        Preview preview;
        std::array<float, hitBins> example {}, after {};
        fillExampleHit (example.data(), hitBins);
        punchview::Settings s;
        preview.run (example.data(), hitBins, s, after.data());
        double worst = 0.0;
        for (int i = 0; i < hitBins; ++i)
            worst = juce::jmax (worst, std::abs ((double) toDb (after[(size_t) i]) - (double) toDb (example[(size_t) i])));
        check (worst < 0.01, "at neutral the example hit is left alone, worst", worst, "dB");
        s.bypass = true;
        s.attack = 12.0f;
        preview.run (example.data(), hitBins, s, after.data());
        worst = 0.0;
        for (int i = 0; i < hitBins; ++i)
            worst = juce::jmax (worst, std::abs ((double) toDb (after[(size_t) i]) - (double) toDb (example[(size_t) i])));
        check (worst < 0.01, "with Bypass on the preview changes nothing, worst", worst, "dB");
    }

    check (std::abs (xForSeconds (0.0)) < 1.0e-6f && std::abs (xForSeconds (0.4) - 1.0f) < 1.0e-5f, "the time axis runs from 0 to 400 ms");
    check (std::abs (secondsForX (xForSeconds (0.03)) - 0.03) < 1.0e-5, "and it can be read both ways");
    check (xForSeconds (0.03) > 0.4f && xForSeconds (0.03) < 0.5f, "the first 30 ms take close to half the width", (double) xForSeconds (0.03));
}

void testWindow()
{
    using ui::BarSlider;
    constexpr double SR = 48000.0;
    auto holder = std::make_unique<PluginProcessor>();
    auto& p = *holder;
    prepare (p, 2, SR, 256);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    auto* w = dynamic_cast<PunchEditor*> (ed.get());
    check (w != nullptr, "the window is the custom one");
    if (w == nullptr)
        return;
    w->setAdvancedOpen (true);

    // Nothing lost: every parameter a person can change has a control in the window, under its own ID, and keeps its name and range.
    {
        struct Expected { const char* id; const char* name; float low, high, rest; };
        const Expected expected[] = {
            { "punch", "Punch", 0, 100, 0 }, { "attack", "Attack", -15, 15, 0 }, { "sustain", "Sustain", -15, 15, 0 }, { "mix", "Mix", 0, 100, 100 },
            { "bypass", "Bypass", 0, 1, 0 }, { "gain", "Gain", -12, 24, 0 }, { "impact", "Impact", 0, 100, 0 }, { "ceiling", "Ceiling", -24, 0, -1 },
            { "ceilingOn", "Ceiling On", 0, 1, 0 },
            { "steadyNotes", "Steady Notes", 0, 1, 0 },
        };
        int missing = 0, wrong = 0;
        for (const auto& e : expected)
        {
            auto* prm = p.apvts.getParameter (e.id);
            if (prm == nullptr || prm->getName (64) != e.name || prm->getNormalisableRange().start != e.low || prm->getNormalisableRange().end != e.high
                || std::abs (prm->convertFrom0to1 (prm->getDefaultValue()) - e.rest) > 0.001f)
            {
                ++wrong;
                std::printf ("  changed: %s\n", e.id);
            }
            if (ed->findChildWithID (e.id) == nullptr)
            {
                ++missing;
                std::printf ("  no control for %s\n", e.id);
            }
        }
        check (wrong == 0, "all 10 controls keep their ID, name, range and default");
        check (missing == 0, "all 10 controls have a place in the window");

        int settable = 0;
        for (auto* prm : p.getParameters())
            if (dynamic_cast<ReadingParameter*> (prm) == nullptr)
                ++settable;
        check (settable == 10, "and there are no others that can be set", (double) settable);
    }

    // The hint line is one line, at the smallest window.
    {
        const auto hintFont = ui::font (ui::Weight::regular, 13.0f);
        const float room = (float) (PluginProcessor::minWindowWidth - 44) * 0.6f;
        float widest = 0.0f;
        const std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            if (auto* h = dynamic_cast<ui::Hinted*> (&c))
                widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (hintFont, h->hint));
            for (auto* child : c.getChildren())
                walk (*child);
        };
        walk (*ed);
        check (widest > 100.0f && widest <= room, "every hint fits on the hint line at the smallest window, widest in points", (double) widest);
    }

    auto* attack = dynamic_cast<BarSlider*> (ed->findChildWithID ("attack"));
    auto* ceiling = dynamic_cast<BarSlider*> (ed->findChildWithID ("ceiling"));
    auto* punch = dynamic_cast<BarSlider*> (ed->findChildWithID ("punch"));
    check (attack != nullptr && ceiling != nullptr && punch != nullptr, "the bars are found");
    if (attack == nullptr || ceiling == nullptr || punch == nullptr)
        return;

    // A bar moves its parameter, with a gesture, and the parameter moves the bar.
    {
        ParamWatch watch (p);
        watch.reset();
        const float y = (float) attack->getHeight() - 20.0f;
        const juce::Point<float> from (50.0f, y), to (50.0f + (float) attack->getWidth() * 0.25f, y);
        attack->mouseDown (mouseAt (*attack, from));
        attack->mouseDrag (mouseAt (*attack, to, 1, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), from));
        attack->mouseUp (mouseAt (*attack, to, 1, juce::ModifierKeys(), from));
        check (realValue (p, "attack") > 4.0f && realValue (p, "attack") < 11.0f, "dragging Attack a quarter of its width moves it from where it was", (double) realValue (p, "attack"));
        check (watch.begins == 1 && watch.ends == 1 && watch.changes > 0, "a drag is one gesture for automation, begins", (double) watch.begins.load());

        setParam (p, "attack", 7.5f);
        check (std::abs (attack->getValue() - 7.5) < 0.05, "automation moves the bar", attack->getValue());
        check (attack->valueText() == "+7.5 dB", "the bar shows the value: " + attack->valueText().toStdString());
        setParam (p, "attack", -3.0f);
        check (attack->valueText() == juce::String::fromUTF8 ("\xe2\x88\x92") + "3.0 dB", "and a real minus sign below zero");

        watch.reset();
        attack->mouseDoubleClick (mouseAt (*attack, from, 2));
        check (std::abs (realValue (p, "attack")) < 0.05f && watch.begins == 1 && watch.ends == 1, "double-click resets to the default, as one gesture", (double) realValue (p, "attack"));
        check (attack->valueText() == "0.0 dB", "and it reads 0.0 dB, with no sign");
    }

    // Typing a value.
    {
        ParamWatch watch (p);
        punch->applyTyped ("42 %");
        check (std::abs (realValue (p, "punch") - 42.0f) < 0.06f && watch.begins == 1 && watch.ends == 1, "typing 42 % sets 42, as one gesture", (double) realValue (p, "punch"));
        punch->applyTyped ("nonsense");
        check (std::abs (realValue (p, "punch") - 42.0f) < 0.06f, "typing a word that means nothing leaves it alone", (double) realValue (p, "punch"));
        punch->applyTyped ("9999");
        check (std::abs (realValue (p, "punch") - 100.0f) < 0.06f, "typing too much stops at the top of the range", (double) realValue (p, "punch"));
        attack->applyTyped (juce::String::fromUTF8 ("\xe2\x88\x92") + "5 dB");
        check (std::abs (realValue (p, "attack") + 5.0f) < 0.06f, "the minus sign the window shows can be typed back", (double) realValue (p, "attack"));
        attack->applyTyped ("+6");
        check (std::abs (realValue (p, "attack") - 6.0f) < 0.06f, "so can a plus sign", (double) realValue (p, "attack"));
        setParam (p, "attack", 0.0f);
        setParam (p, "punch", 0.0f);
    }

    // Buttons.
    {
        auto* on = dynamic_cast<ui::Pill*> (ed->findChildWithID ("ceilingOn"));
        auto* byp = dynamic_cast<ui::Pill*> (ed->findChildWithID ("bypass"));
        check (on != nullptr && byp != nullptr, "the buttons are found");
        if (on == nullptr || byp == nullptr)
            return;
        ParamWatch watch (p);
        on->mouseDown (mouseAt (*on, { 5.0f, 5.0f }));
        check (realValue (p, "ceilingOn") >= 0.5f && on->isOn() && watch.begins == 1 && watch.ends == 1, "the button turns Ceiling On on, as one gesture");
        on->mouseDoubleClick (mouseAt (*on, { 5.0f, 5.0f }, 2));
        check (realValue (p, "ceilingOn") < 0.5f, "double-click puts it back to its default, off");
        byp->mouseDown (mouseAt (*byp, { 5.0f, 5.0f }));
        check (realValue (p, "bypass") >= 0.5f && byp->isOn(), "the button turns Bypass on");
        setParam (p, "bypass", 0.0f);
        check (! byp->isOn(), "automation moves the button");
    }

    // The ceiling line on the display.
    {
        auto& view = w->getHitView();
        setParam (p, "ceilingOn", 0.0f);
        w->tick (1.0 / 30.0);
        const auto plot = view.getPlot().toFloat();
        check (! view.isNearCeilingLine (view.ceilingLineY()), "with the Ceiling off, its line can't be grabbed");

        setParam (p, "ceiling", -6.0f);
        setParam (p, "ceilingOn", 1.0f);
        w->tick (1.0 / 30.0);
        const juce::Point<float> from (plot.getCentreX(), view.ceilingLineY());
        check (std::abs (punchview::topDb - (from.y - plot.getY()) / plot.getHeight() * (punchview::topDb - punchview::bottomDb) + 6.0f) < 0.01f, "the line is drawn at the Ceiling's level on the plot's scale");
        ParamWatch watch (p);
        const auto to = from.translated (0.0f, plot.getHeight() * 6.0f / (punchview::topDb - punchview::bottomDb));
        view.mouseDown (mouseAt (view, from));
        view.mouseDrag (mouseAt (view, to, 1, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), from));
        view.mouseUp (mouseAt (view, to, 1, juce::ModifierKeys(), from));
        check (std::abs (realValue (p, "ceiling") + 12.0f) < 0.2f, "dragging the line 6 dB down moves the Ceiling to -12", (double) realValue (p, "ceiling"));
        check (watch.begins == 1 && watch.ends == 1, "as one gesture", (double) watch.begins.load());
        w->tick (1.0 / 30.0);
        const auto near = juce::Point<float> (plot.getCentreX(), view.ceilingLineY());
        view.mouseDoubleClick (mouseAt (view, near, 2));
        check (std::abs (realValue (p, "ceiling") + 1.0f) < 0.05f, "double-click on the line puts it back to -1", (double) realValue (p, "ceiling"));
        setParam (p, "ceilingOn", 0.0f);
    }

    // Presets.
    {
        auto* box = dynamic_cast<ui::PresetBox*> (ed->findChildWithID ("presets"));
        check (box != nullptr, "the preset box is found");
        if (box != nullptr)
        {
            // Every name fits in the box, with ", edited" after it, in the box's own font: nothing is cut short.
            const auto nameFont = ui::font (ui::Weight::medium, 14.0f);
            const float room = (float) box->getWidth() - 2.0f * 30.0f;
            float widest = 0.0f;
            for (int i = 0; i < p.getNumPrograms(); ++i)
                widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (nameFont, p.getProgramName (i) + ", edited"));
            check (p.getNumPrograms() == 20 && widest > 100.0f && widest <= room, "all 20 preset names fit in the preset box, edited or not; widest in points", (double) widest);
            info ("  room for a name in the preset box", (double) room, "points");
            box->choose (8);
            check (std::abs (realValue (p, "impact") - 55.0f) < 0.06f && box->getShownName() == "Rap Upfront", "choosing a preset sets it and shows its name: " + box->getShownName().toStdString());
            setParam (p, "attack", 9.0f);
            box->refresh();
            check (box->getShownName() == "Rap Upfront, edited", "moving a control says so: " + box->getShownName().toStdString());
            box->choose (0);
            check (box->getShownName() == "Neutral" && ! p.isCurrentProgramEdited(), "and Neutral is the defaults");
        }
    }

    // What the numbers say.
    {
        const auto x = tone (SR, 1.0, 1000.0, -12.0);
        setParam (p, "gain", 6.0f);
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        for (int pos = 0; pos + 256 <= (int) x.size(); pos += 256)
        {
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, x.data() + pos, 256);
            p.processBlock (buffer, midi);
            w->tick (1.0 / 30.0);
        }
        auto& r = w->getReadouts();
        const auto minus = juce::String::fromUTF8 ("\xe2\x88\x92");
        check (r.textOf (0) == minus + "12.0" && r.textOf (1) == minus + "6.0", "a tone at -12 with Gain +6 reads In -12.0 and Out -6.0: " + r.textOf (0).toStdString() + " " + r.textOf (1).toStdString());
        check (r.textOf (2) == "0.0" && r.textOf (3) == "Off", "Impact reads 0.0 and the Ceiling reads Off");
        setParam (p, "impact", 50.0f);
        setParam (p, "ceiling", -12.0f);
        setParam (p, "ceilingOn", 1.0f);
        for (int pos = 0; pos + 256 <= (int) x.size(); pos += 256)
        {
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, x.data() + pos, 256);
            p.processBlock (buffer, midi);
            w->tick (1.0 / 30.0);
        }
        check (std::abs (r.textOf (2).replace (minus, "-").getFloatValue() - p.impactReduction->getDb()) < 0.3f && p.impactReduction->getDb() < -1.0f, "Impact's number is the Impact Reduction reading: " + r.textOf (2).toStdString());
        check (std::abs (r.textOf (3).replace (minus, "-").getFloatValue() - p.ceilingReduction->getDb()) < 0.3f && p.ceilingReduction->getDb() < -1.0f, "the Ceiling's number is the Ceiling Reduction reading: " + r.textOf (3).toStdString());
        p.setCurrentProgram (0);
        for (int i = 0; i < 40; ++i)
            w->tick (1.0 / 30.0);
        check (r.textOf (2) == "0.0", "with the host stopped, the numbers fall back on their own");
    }

    // Saved state: only the size and Advanced are added, they stay in range, and they stay out of the parameters.
    {
        w->setSize (1100, 800);
        w->setAdvancedOpen (true);
        juce::MemoryBlock st;
        p.getStateInformation (st);
        auto other = std::make_unique<PluginProcessor>();
        other->setStateInformation (st.getData(), (int) st.getSize());
        check (other->windowWidth.load() == 1100 && other->windowHeight.load() == 800 && other->advancedOpen.load(), "the size and Advanced come back with the session", (double) other->windowWidth.load());
        check (! other->apvts.state.hasProperty ("windowWidth") && ! other->apvts.state.hasProperty ("windowHeight") && ! other->apvts.state.hasProperty ("advancedOpen"), "they stay out of the parameter tree");

        auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        xml->setAttribute ("windowWidth", 99999);
        xml->setAttribute ("windowHeight", "nonsense");
        juce::MemoryBlock bad;
        juce::AudioProcessor::copyXmlToBinary (*xml, bad);
        other->setStateInformation (bad.getData(), (int) bad.getSize());
        check (other->windowWidth.load() == PluginProcessor::maxWindowWidth && other->windowHeight.load() == PluginProcessor::minWindowHeight, "a damaged size goes back in range", (double) other->windowWidth.load());

        xml->removeAttribute ("windowWidth");
        xml->removeAttribute ("windowHeight");
        xml->removeAttribute ("advancedOpen");
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        auto fresh = std::make_unique<PluginProcessor>();
        fresh->setStateInformation (old.getData(), (int) old.getSize());
        check (fresh->windowWidth.load() == PluginProcessor::defaultWindowWidth && ! fresh->advancedOpen.load(), "a session from before the window opens at the default size");

        std::unique_ptr<juce::AudioProcessorEditor> reopened (other->createEditor());
        check (reopened->getWidth() == PluginProcessor::maxWindowWidth && reopened->getHeight() >= PluginProcessor::minWindowHeight, "a window opens within limits from a damaged size", (double) reopened->getHeight());
        w->setAdvancedOpen (false);
    }

    // The window only reads: frames and paints, with speech going through, move no parameter and start no gesture.
    {
        const auto x = loadSpeech ("speech_daniel.wav");
        check (! x.empty(), "the speech is there");
        ParamWatch watch (p);
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        bool moved = false;
        for (int pos = 0; pos + 256 <= (int) x.size() / 4; pos += 256)
        {
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, x.data() + pos, 256);
            p.processBlock (buffer, midi);
            if (pos % 1600 < 256)
            {
                moved = w->tick (1.0 / 30.0) || moved;
                paintAt (*ed, pos % 3200 < 1600 ? 1.0f : 2.0f);
            }
        }
        check (moved, "with speech going through, the display reports it is moving");
        check (w->getHitView().getShowing() == punchview::HitView::Showing::real, "and it shows the real hit, not the example");
        check (watch.changes == 0 && watch.begins == 0 && watch.ends == 0, "the window reading and painting writes no parameter and no automation", (double) watch.changes.load());

        // With no more audio, it settles and says so, so the timer can slow down and nothing repaints.
        bool still = true;
        for (int i = 0; i < 200; ++i)
            still = w->tick (1.0 / 30.0);
        check (! still, "with the host stopped, the display stops moving");

        const int paints = w->getHitView().getPaintCount();
        for (int i = 0; i < 30; ++i)
            w->tick (1.0 / 30.0);
        paintAt (*ed, 1.0f);
        check (w->getHitView().getPaintCount() == paints + 1, "and no longer asks to be drawn again", (double) (w->getHitView().getPaintCount() - paints));

        // Moved while stopped: the hit in view is redrawn with the new settings, with no audio.
        setParam (p, "attack", 12.0f);
        w->tick (1.0 / 30.0);
        check (w->getHitView().getShowing() == punchview::HitView::Showing::preview, "moving Attack while stopped redraws the hit in view");
        check (w->tick (1.0 / 30.0), "and keeps the window lively for a moment while the control moves");
        const auto& view = w->getHitView();
        double gained = 0.0;
        for (int i = 4; i < 12; ++i)
            gained += ((double) punchview::toDb (view.shownAfter()[i]) - (double) punchview::toDb (view.shownBefore()[i])) / 8.0;
        check (gained > 3.0, "and its start rises", gained, "dB");
        setParam (p, "attack", 0.0f);
        w->tick (1.0 / 30.0);
        check (view.getShowing() == punchview::HitView::Showing::real && view.shownAfter() == view.getTracker().latest().after.data(), "putting it back shows what really happened again");
    }

    // A host playing silence: nothing on the timeline, so nothing moves, and nothing asks to be drawn.
    {
        auto q = std::make_unique<PluginProcessor>();
        prepare (*q, 2, SR, 256);
        std::unique_ptr<juce::AudioProcessorEditor> e (q->createEditor());
        auto* te = dynamic_cast<PunchEditor*> (e.get());
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        for (int i = 0; i < 30; ++i)
            te->tick (1.0 / 30.0);
        int movedFrames = 0;
        for (int frame = 0; frame < 300; ++frame)
        {
            for (int b = 0; b < 6; ++b)
            {
                buffer.clear();
                q->processBlock (buffer, midi);
            }
            movedFrames += te->tick (1.0 / 30.0) ? 1 : 0;
        }
        check (movedFrames == 0, "10 seconds of silence from the host moves nothing, frames", (double) movedFrames);
    }

    // Before any sound: the example hit, and the controls move it.
    {
        auto q = std::make_unique<PluginProcessor>();
        prepare (*q, 2, SR, 256);
        std::unique_ptr<juce::AudioProcessorEditor> e (q->createEditor());
        auto* te = dynamic_cast<PunchEditor*> (e.get());
        auto& view = te->getHitView();
        check (view.getShowing() == punchview::HitView::Showing::example && view.captionText().contains ("Example"), "with nothing played, it says it is an example");
        const auto at = [&view] (int bin) { return (double) punchview::toDb (view.shownAfter()[bin]) - (double) punchview::toDb (view.shownBefore()[bin]); };
        check (std::abs (at (6)) < 0.01, "and at neutral nothing is changed", at (6), "dB");
        setParam (*q, "attack", 12.0f);
        te->tick (1.0 / 30.0);
        check (at (6) > 6.0, "Attack moves its start", at (6), "dB");
        setParam (*q, "attack", 0.0f);
        setParam (*q, "sustain", -12.0f);
        te->tick (1.0 / 30.0);
        check (at (400) < -6.0, "Sustain moves its tail", at (400), "dB");
        paintAt (*e, 2.0f);
    }

    // Opening and closing, over and over, with audio running on another thread and with none.
    {
        auto q = std::make_unique<PluginProcessor>();
        prepare (*q, 2, SR, 256);
        std::atomic<bool> running { true };
        const auto x = loadSpeech ("speech_samantha.wav");
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            int pos = 0;
            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    buffer.copyFrom (ch, 0, x.data() + pos, 256);
                pos = (pos + 256) % ((int) x.size() - 256);
                q->processBlock (buffer, midi);
            }
        });
        int opened = 0;
        for (int i = 0; i < 30; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> e (q->createEditor());
            auto* te = dynamic_cast<PunchEditor*> (e.get());
            te->tick (1.0 / 30.0);
            paintAt (*e, i % 2 == 0 ? 2.0f : 1.0f);
            te->setAdvancedOpen (i % 3 == 0);
            te->tick (1.0 / 30.0);
            paintAt (*e, 1.0f);
            ++opened;
        }
        running = false;
        audio.join();
        for (int i = 0; i < 30; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> e (q->createEditor());
            paintAt (*e, 2.0f);
            ++opened;
        }
        check (opened == 60, "60 windows opened, painted and closed, 30 of them with audio running", (double) opened);

        auto unprepared = std::make_unique<PluginProcessor>();
        std::unique_ptr<juce::AudioProcessorEditor> before (unprepared->createEditor());
        paintAt (*before, 2.0f);
        check (before->getWidth() > 0, "a window opens and paints before the host has prepared the plug-in");
    }

    // The null test: real speech with the window open, ticking and painting is identical to the sample with it closed.
    {
        const auto x = loadSpeech ("speech_samantha.wav");
        const auto renderWith = [&] (bool withWindow)
        {
            auto q = std::make_unique<PluginProcessor>();
            apply (*q, { 25.0f, 6.0f, -4.0f, 90.0f, 3.0f, 45.0f, -3.0f, 1.0f });
            prepare (*q, 2, SR, 256);
            std::unique_ptr<juce::AudioProcessorEditor> e (withWindow ? q->createEditor() : nullptr);
            auto* te = dynamic_cast<PunchEditor*> (e.get());
            int block = 0;
            return render (*q, x, 256, nullptr, [&] (int)
            {
                if (te != nullptr)
                {
                    te->tick (1.0 / 30.0);
                    if (++block % 16 == 0)
                        paintAt (*e, 2.0f);
                }
            });
        };
        const auto closed = renderWith (false);
        const auto open = renderWith (true);
        double worst = 0.0, loudest = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < closed.getNumSamples(); ++i)
            {
                worst = juce::jmax (worst, (double) std::abs (closed.getSample (ch, i) - open.getSample (ch, i)));
                loudest = juce::jmax (loudest, (double) std::abs (closed.getSample (ch, i)));
            }
        check (loudest > 0.1, "the null test has real audio in it, peak", loudest);
        check (worst == 0.0, "audio with the window open and painting nulls to exact zero against the window closed", worst);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    GenericOptions opt;
    opt.minPresets = 4;
    opt.readOnly = { "inputLevel", "outputLevel", "impactReduction", "ceilingReduction" };
    runGenericChecks ([] { return std::make_unique<PluginProcessor>(); }, opt);
    testAttack();
    testSustain();
    testLevelIndependence();
    testOtherSounds();
    testRates();
    testSteadyTones();
    testHeldNotes();
    testSteadyNotes();
    testPunch();
    testMix();
    testNeutral();
    testLatencyAndBypass();
    testBlocksAndRestart();
    testGainMotionAndStereo();
    testDamagedStateAndInput();
    testGain();
    testImpact();
    testCeiling();
    testNewPartsStereoAndBlocks();
    testMeters();
    testSameAsFirstVersion();
    testNewPresets();
    testPitchSense();
    testSteadyVoice();
    testSteadyPunch();
    testSteadyAttack();
    testOffStaysOff();
    testHeldAfterLoudStart();
    testLeadPresets();
    testEnvelopeTap();
    testHitDisplayNumbers();
    testWindow();
    return finish();
}
