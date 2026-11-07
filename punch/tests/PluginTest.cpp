// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// All limits below were written before the first run.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include "GoldenV010.h"

using namespace testkit;

namespace
{
    struct Settings { float punch = 0, attack = 0, sustain = 0, mix = 100, gain = 0, impact = 0, ceiling = -1, ceilingOn = 0; };

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
    for (int preset = 7; preset < PluginProcessor().getNumPrograms(); ++preset)
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
    return finish();
}
