// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// All limits below were written before the first run.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>

using namespace testkit;

namespace
{
    struct Settings { float punch = 0, attack = 0, sustain = 0, mix = 100; };

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
        const auto ma = measure (in, a);
        const auto ms = measure (in, su);
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

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    GenericOptions opt;
    opt.minPresets = 4;
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
    return finish();
}
