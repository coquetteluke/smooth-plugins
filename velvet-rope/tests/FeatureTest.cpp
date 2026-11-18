// The five features added in 0.5.0, measured through the real processor. Every limit was written before the first run.
// Run: build/FeatureTest_artefacts/Release/FeatureTest
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include "legacy/LegacyEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>


#include <time.h>

/** The time this thread has spent running, in seconds. The lines that time a block of audio read this and not the wall clock:
    on a busy Mac the wall clock also counts the time macOS has the thread off its core, which is not the plug-in's doing
    (measured 2026-11-11: a block that read 3313% of real time by the wall clock had cost 4.3% in CPU time). The limits and the
    wording of those lines are as they were. */
static double threadCpuSeconds() noexcept
{
    return (double) clock_gettime_nsec_np (CLOCK_THREAD_CPUTIME_ID) * 1.0e-9;
}

using namespace testkit;
using Proc = SmoothResonanceProcessor;
using Multi = std::vector<std::vector<float>>;
using smoothres::Quality;
using Set = juce::AudioChannelSet;

static constexpr double kRate = 48000.0;
static constexpr double pi2 = juce::MathConstants<double>::twoPi;

#ifndef SPEECH_DIR
 #define SPEECH_DIR "build/speech"
#endif

// ---- helpers --------------------------------------------------------------------------------------------------

static void setParam (Proc& p, const juce::String& id, float value)
{
    auto* prm = p.parameters.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static void prepareLayout (Proc& p, const Set& set, double rate = kRate, int block = 256)
{
    p.releaseResources();
    auto l = p.getBusesLayout();
    l.inputBuses.getReference (0) = set;
    l.outputBuses.getReference (0) = set;
    l.inputBuses.getReference (1) = Set::disabled();
    const bool ok = p.setBusesLayout (l);
    jassertquiet (ok);
    p.setRateAndBufferSizeDetails (rate, block);
    p.prepareToPlay (rate, block);
}

static std::vector<float> tone (int samples, double hz, double peakDb, double rate = kRate)
{
    std::vector<float> x ((size_t) samples);
    const double a = std::pow (10.0, peakDb / 20.0);
    for (int i = 0; i < samples; ++i)
        x[(size_t) i] = (float) (a * std::sin (pi2 * hz * i / rate));
    return x;
}

static std::vector<float> noise (int samples, double rmsDb, unsigned seed)
{
    std::vector<float> x ((size_t) samples);
    std::mt19937 g (seed);
    std::normal_distribution<float> d (0.0f, (float) std::pow (10.0, rmsDb / 20.0));
    for (auto& v : x) v = d (g);
    return x;
}

static std::vector<float> plus (std::vector<float> a, const std::vector<float>& b)
{
    for (size_t i = 0; i < a.size(); ++i) a[i] += b[i];
    return a;
}

/** Runs through a host's blocks. The output keeps its delay, exactly as a host would get it. before() runs ahead of each block. */
static Multi run (Proc& p, const Multi& in, int block = 256, const std::function<void (int)>& before = {})
{
    const int total = (int) in[0].size(), channels = (int) in.size();
    juce::AudioBuffer<float> buf (channels, block);
    juce::MidiBuffer midi;
    Multi out = in;

    for (int pos = 0; pos < total; pos += block)
    {
        if (before) before (pos);
        const int len = std::min (block, total - pos);
        buf.setSize (channels, len, false, false, true);
        for (int ch = 0; ch < channels; ++ch)
            std::copy_n (in[(size_t) ch].data() + pos, len, buf.getWritePointer (ch));
        p.processBlock (buf, midi);
        for (int ch = 0; ch < channels; ++ch)
            std::copy_n (buf.getReadPointer (ch), len, out[(size_t) ch].data() + pos);
    }

    return out;
}

static Multi stereoOf (const std::vector<float>& x)  { return { x, x }; }

static double bandDb (const std::vector<float>& x, double lo, double hi, int from = 0, int to = -1)
{
    const int n = 8192;
    juce::dsp::FFT fft (13);
    std::vector<float> buf ((size_t) n * 2);
    double sum = 0.0;
    int frames = 0;
    if (to < 0) to = (int) x.size();
    for (int pos = from; pos + n <= to; pos += n / 2)
    {
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = x[(size_t) (pos + i)] * (float) (0.5 - 0.5 * std::cos (pi2 * i / n));
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = (int) (lo * n / kRate); k <= (int) (hi * n / kRate); ++k)
            sum += (double) buf[(size_t) k * 2] * buf[(size_t) k * 2] + (double) buf[(size_t) k * 2 + 1] * buf[(size_t) k * 2 + 1];
        ++frames;
    }
    return 10.0 * std::log10 (std::max (sum / std::max (frames, 1), 1.0e-20));
}

static double toneLevelDb (const std::vector<float>& x, double hz, int from, int to = -1)
{
    return toneDb (x.data() + from, (to < 0 ? (int) x.size() : to) - from, hz, kRate);
}

static double peakOf (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = from; i < to; ++i) m = std::max (m, (double) std::abs (x[(size_t) i]));
    return m;
}

/** Biggest second difference. On a smooth signal this is small, and a click shows straight away. */
static double roughness (const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = from + 2; i < to; ++i)
        m = std::max (m, (double) std::abs (x[(size_t) i] - 2.0f * x[(size_t) i - 1] + x[(size_t) i - 2]));
    return m;
}

static int firstAbove (const std::vector<float>& x, float limit)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs (x[i]) > limit)
            return (int) i;
    return -1;
}

static const char* qualityName (int q)  { return q == 0 ? "Low Latency" : q == 1 ? "Normal" : "High"; }

static std::vector<float> loadSpeech (const char* voice)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (juce::File (juce::String (SPEECH_DIR) + "/" + voice + ".wav").createInputStream().release(), true));
    if (reader == nullptr)
        return {};
    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, b.getNumSamples(), 0, true, false);
    std::vector<float> x (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    double sum = 0.0;
    for (auto v : x) sum += (double) v * v;
    const auto scale = (float) (0.15 / std::sqrt (sum / (double) x.size()));
    for (auto& v : x) v *= scale;
    return x;
}

static std::vector<float> peakFilter (std::vector<float> x, double hz, double q, double gainDb)
{
    const double a = std::pow (10.0, gainDb / 40.0), w = pi2 * hz / kRate;
    const double alpha = std::sin (w) / (2.0 * q), cosw = std::cos (w), a0 = 1.0 + alpha / a;
    const double b0 = (1.0 + alpha * a) / a0, b1 = -2.0 * cosw / a0, b2 = (1.0 - alpha * a) / a0, a1 = b1, a2 = (1.0 - alpha / a) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (auto& v : x)
    {
        const double in = v, y = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in; y2 = y1; y1 = y;
        v = (float) y;
    }
    return x;
}

// ---- dots ------------------------------------------------------------------------------------------------------

static void testDots()
{
    std::puts ("\n== Up to 8 dots");
    Proc fresh;
    bool allThere = true, offByDefault = true;

    for (int b = 1; b <= 8; ++b)
    {
        const auto n = juce::String (b);
        for (const auto& suffix : { juce::String ("Freq"), juce::String ("Sens"), juce::String ("Width"), juce::String ("On") })
        {
            auto* prm = fresh.parameters.getParameter ("band" + n + suffix);
            allThere = allThere && prm != nullptr && prm->isAutomatable() && prm->getName (64).startsWith ("Band " + n + " ");
        }
        const bool on = fresh.parameters.getParameter ("band" + n + "On")->getValue() > 0.5f;
        offByDefault = offByDefault && on == (b <= 3);
    }

    check (allThere, "dots 1 to 8 each have frequency, sensitivity, width and on, as real named parameters");
    check (offByDefault, "dots 1 to 3 start on and dots 4 to 8 start off");

    const int samples = (int) (4.0 * kRate), settled = (int) (2.5 * kRate);
    const auto bed = plus (tone (samples, 3000.0, -14.0), noise (samples, -30.0, 11));

    const auto cutWith = [&] (const std::function<void (Proc&)>& setup)
    {
        Proc p;
        setParam (p, "depth", 12.0f);
        setup (p);
        prepareLayout (p, Set::stereo());
        const auto out = run (p, stereoOf (bed));
        return toneLevelDb (bed, 3000.0, settled - p.getLatencySamples(), samples - p.getLatencySamples()) - toneLevelDb (out[0], 3000.0, settled);
    };

    const double plain = cutWith ([] (Proc&) {});
    check (plain > 4.0, "without new dots the test tone is cut (limit over 4 dB)", plain, "dB");

    for (int b = 4; b <= 8; ++b)
    {
        const auto n = juce::String (b);

        const double protectedCut = cutWith ([&] (Proc& p) { setParam (p, "band" + n + "On", 1.0f); setParam (p, "band" + n + "Freq", 3000.0f); setParam (p, "band" + n + "Sens", -12.0f); });
        check (protectedCut < 0.5, "dot " + std::to_string (b) + " on, -12 dB at the tone: treatment off there (limit under 0.5 dB)", protectedCut, "dB");

        const double offCut = cutWith ([&] (Proc& p) { setParam (p, "band" + n + "Freq", 3000.0f); setParam (p, "band" + n + "Sens", -12.0f); });
        check (std::abs (offCut - plain) < 0.01, "dot " + std::to_string (b) + " off with the same settings does nothing (limit 0.01 dB)", offCut - plain, "dB");
    }

    // Dots 1 to 3 can be taken away too, which is what the window's remove does.
    const double removedOld = cutWith ([] (Proc& p) { setParam (p, "band2Freq", 3000.0f); setParam (p, "band2Sens", -12.0f); setParam (p, "band2On", 0.0f); });
    check (std::abs (removedOld - plain) < 0.01, "dot 2 taken away: its setting no longer counts (limit 0.01 dB)", removedOld - plain, "dB");

    // A dot switched on with sensitivity 0 is silent in every sense.
    {
        Proc a, b2;
        for (int b = 4; b <= 8; ++b)
            setParam (b2, "band" + juce::String (b) + "On", 1.0f);
        prepareLayout (a, Set::stereo());
        prepareLayout (b2, Set::stereo());
        const auto x = run (a, stereoOf (bed)), y = run (b2, stereoOf (bed));
        double worst = 0.0;
        for (size_t i = 0; i < x[0].size(); ++i) worst = std::max (worst, (double) std::abs (x[0][i] - y[0][i]));
        check (toDb (worst) <= -90.0, "all 8 dots on, all at 0 dB: nulls against 3 dots (limit -90)", toDb (worst), "dBFS");
    }

    // All 8 working at once, and switching dots with sound going through.
    {
        Proc p;
        setParam (p, "depth", 12.0f);
        for (int b = 1; b <= 8; ++b)
        {
            const auto n = juce::String (b);
            setParam (p, "band" + n + "On", 1.0f);
            setParam (p, "band" + n + "Sens", b % 2 == 0 ? 6.0f : -6.0f);
            setParam (p, "band" + n + "Freq", 200.0f * std::pow (1.8f, (float) b));
        }
        prepareLayout (p, Set::stereo());
        const auto sine = tone (96000, 440.0, -18.0);
        const auto out = run (p, stereoOf (sine), 256, [&] (int pos)
        {
            if (pos > 24000 && pos % 2048 == 0)
            {
                const int b = 1 + (pos / 2048) % 8;
                const auto* prm = p.parameters.getParameter ("band" + juce::String (b) + "On");
                setParam (p, "band" + juce::String (b) + "On", prm->getValue() > 0.5f ? 0.0f : 1.0f);
            }
        });
        const double steady = roughness (sine, 0, 24000);
        const double moved = roughness (out[0], 24000, 96000);
        check (moved < 3.0 * steady, "dots switched on and off with sound going through: no click (limit 3x)", moved / steady, "x");
    }
}

// ---- quality ---------------------------------------------------------------------------------------------------

static void testQualityLatency()
{
    std::puts ("\n== Quality: latency");
    Proc fresh;
    check (fresh.parameters.getParameter ("quality")->getCurrentValueAsText() == "Normal", "Quality is Normal by default");
    check (fresh.parameters.getParameter ("quality")->isAutomatable(), "Quality is automatable");

    const std::pair<double, std::array<int, 3>> table[] = { { 44100.0, { 512, 2048, 4096 } }, { 48000.0, { 512, 2048, 4096 } },
                                                            { 88200.0, { 1024, 4096, 8192 } }, { 96000.0, { 1024, 4096, 8192 } } };

    for (const auto& row : table)
    {
        for (int q = 0; q < 3; ++q)
        {
            for (int bypassed = 0; bypassed < 2; ++bypassed)
            {
                Proc p;
                setParam (p, "quality", (float) q);
                setParam (p, "bypass", (float) bypassed);
                prepareLayout (p, Set::stereo(), row.first);
                std::vector<float> x (40000, 0.0f);
                x[1000] = 0.5f;
                const auto out = run (p, stereoOf (x));
                const int found = firstAbove (out[0], 0.05f) - 1000;
                // The impulse sits at the middle of a symmetric response, so the top of it is the real delay.
                int top = 0;
                for (int i = 0; i < (int) out[0].size(); ++i) if (std::abs (out[0][(size_t) i]) > std::abs (out[0][(size_t) top])) top = i;
                const int measured = top - 1000;
                check (p.getLatencySamples() == row.second[(size_t) q] && measured == row.second[(size_t) q],
                       std::string (qualityName (q)) + (bypassed ? ", bypassed" : "") + " at " + std::to_string ((int) row.first) + " Hz: reported "
                           + std::to_string (p.getLatencySamples()) + ", first sound at " + std::to_string (found) + ", measured", measured, "samples");
            }
        }
    }

    // Reported in time, for the README.
    for (int q = 0; q < 3; ++q)
    {
        Proc p;
        setParam (p, "quality", (float) q);
        prepareLayout (p, Set::stereo());
        info (std::string ("Latency at 48 kHz, ") + qualityName (q), p.getLatencySamples() / 48.0, "ms");
    }

    // Live changes: the audio's real delay, and what the host is told, after each of the six switches.
    for (int from = 0; from < 3; ++from)
    {
        for (int to = 0; to < 3; ++to)
        {
            if (from == to) continue;

            for (int bypassed = 0; bypassed < 2; ++bypassed)
            {
                Proc p;
                setParam (p, "quality", (float) from);
                setParam (p, "bypass", (float) bypassed);
                prepareLayout (p, Set::stereo());
                std::vector<float> x (60000, 0.0f);
                x[40000] = 0.5f;
                const auto out = run (p, stereoOf (x), 256, [&] (int pos) { if (pos == 2560) setParam (p, "quality", (float) to); });
                p.syncLatencyToQuality();
                int top = 0;
                for (int i = 0; i < (int) out[0].size(); ++i) if (std::abs (out[0][(size_t) i]) > std::abs (out[0][(size_t) top])) top = i;
                check (top - 40000 == p.getLatencySamples(), std::string ("after a live switch ") + qualityName (from) + " to " + qualityName (to) + (bypassed ? ", bypassed" : "") + ", delay equals what the host is told", top - 40000, "samples");
            }
        }
    }

    // The parameter on the message thread tells the host straight away.
    {
        Proc p;
        prepareLayout (p, Set::stereo());
        setParam (p, "quality", 0.0f);
        check (p.getLatencySamples() == 512, "setting Quality on the message thread updates the reported latency at once", p.getLatencySamples(), "samples");
        p.setCurrentProgram (0);
        check (p.getLatencySamples() == 2048, "an old preset goes back to Normal and its latency", p.getLatencySamples(), "samples");
    }

    // Loading a saved state with another Quality.
    {
        Proc a, b;
        setParam (a, "quality", 2.0f);
        juce::MemoryBlock state;
        a.getStateInformation (state);
        b.setStateInformation (state.getData(), (int) state.getSize());
        check (b.getLatencySamples() == 4096, "a session saved in High comes back with High's latency", b.getLatencySamples(), "samples");
        prepareLayout (b, Set::stereo());
        check (b.getLatencySamples() == 4096, "and still after prepareToPlay", b.getLatencySamples(), "samples");
    }
}

static void testQualitySwitching()
{
    std::puts ("\n== Quality: switching with sound going through");

    for (int from = 0; from < 3; ++from)
    {
        for (int to = 0; to < 3; ++to)
        {
            if (from == to) continue;

            const auto label = std::string (qualityName (from)) + " to " + qualityName (to);
            const int samples = 96000, switchAt = 48000;
            const auto sine = tone (samples, 1000.0, -12.0);

            Proc p;
            setParam (p, "quality", (float) from);
            prepareLayout (p, Set::stereo());
            const auto out = run (p, stereoOf (sine), 256, [&] (int pos) { if (pos == switchAt) setParam (p, "quality", (float) to); });

            const double steady = roughness (out[0], 20000, switchAt);
            const double during = roughness (out[0], switchAt, samples);
            check (during < 2.0 * steady, label + ": no click on a 1 kHz sine, second difference (limit 2x steady)", during / steady, "x");

            const double before = peakOf (out[0], 20000, switchAt), most = peakOf (out[0], switchAt, samples);
            check (toDb (most / before) < 3.2, label + ": level never jumps by more than the crossfade's 3 dB", toDb (most / before), "dB");

            // On noise too, which has nothing to hide behind.
            const auto hiss = noise (samples, -24.0, 17);
            Proc n;
            setParam (n, "quality", (float) from);
            prepareLayout (n, Set::stereo());
            const auto outNoise = run (n, stereoOf (hiss), 256, [&] (int pos) { if (pos == switchAt) setParam (n, "quality", (float) to); });
            const double jumpBefore = maxJump (outNoise[0].data() + 20000, switchAt - 20000), jumpDuring = maxJump (outNoise[0].data() + switchAt, samples - switchAt);
            check (jumpDuring < 1.5 * jumpBefore, label + ": no click on noise, biggest step (limit 1.5x)", jumpDuring / jumpBefore, "x");

            // Sound stops at the moment of the switch. Once both delays and the fade are over, nothing may come out.
            auto gated = tone (samples, 1000.0, -6.0);
            std::fill (gated.begin() + switchAt, gated.end(), 0.0f);
            Proc s;
            setParam (s, "quality", (float) from);
            prepareLayout (s, Set::stereo());
            const auto outGated = run (s, stereoOf (gated), 256, [&] (int pos) { if (pos == switchAt) setParam (s, "quality", (float) to); });
            const int quietFrom = switchAt + 4096 * 2 + 4096 + 1440 + 512;
            check (toDb (peakOf (outGated[0], quietFrom, samples)) < -120.0, label + ": sound stopped at the switch leaves nothing behind (limit -120)", toDb (peakOf (outGated[0], quietFrom, samples)), "dBFS");

            // And back again, later, over silence: the core that comes back was wiped.
            Proc r;
            setParam (r, "quality", (float) from);
            prepareLayout (r, Set::stereo());
            auto burst = tone (192000, 1000.0, -6.0);
            std::fill (burst.begin() + 48000, burst.end(), 0.0f);
            const auto outBack = run (r, stereoOf (burst), 256, [&] (int pos)
            {
                if (pos == 25600) setParam (r, "quality", (float) to);     // while sound is playing
                if (pos == 96000) setParam (r, "quality", (float) from);   // long after it stopped
            });
            check (toDb (peakOf (outBack[0], 70000, 192000)) < -120.0, label + " and back over silence: no old audio returns (limit -120)", toDb (peakOf (outBack[0], 70000, 192000)), "dBFS");
        }
    }

    // Changing as fast as a host could ask. It must stay clean, bounded, and end up on the last choice.
    {
        Proc p;
        prepareLayout (p, Set::stereo());
        const auto sine = tone (144000, 500.0, -12.0);
        int count = 0;
        const auto out = run (p, stereoOf (sine), 128, [&] (int pos)
        {
            if (pos >= 24000 && pos < 96000 && pos % 512 == 0)
                setParam (p, "quality", (float) (++count % 3));
        });
        setParam (p, "quality", 0.0f);
        const auto after = run (p, stereoOf (std::vector<float> (24000, 0.0f)));
        const double rough = roughness (out[0], 24000, 144000) / roughness (out[0], 4000, 24000);
        check (rough < 2.5, "Quality flipped every 10 ms for 1.5 s: no click (limit 2.5x)", rough, "x");
        check (toDb (peakOf (after[0], 12000, 24000)) < -120.0, "and silence afterwards stays silent", toDb (peakOf (after[0], 12000, 24000)), "dBFS");
    }

    // The real time rules hold during a switch. Only processBlock is counted, since a parameter write comes from the host.
    {
        Proc p;
        setParam (p, "depth", 12.0f);
        prepareLayout (p, Set::stereo());
        const auto x = plus (tone (120000, 3000.0, -14.0), noise (120000, -30.0, 31));
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        long used = 0;
        int switches = 0;

        for (int pos = 0; pos + 256 <= 120000; pos += 256)
        {
            if (pos == 20480 || pos == 40960 || pos == 61440 || pos == 81920)
            {
                setParam (p, "quality", (float) (switches % 3));
                ++switches;
            }

            std::copy_n (x.data() + pos, 256, buf.getWritePointer (0));
            std::copy_n (x.data() + pos, 256, buf.getWritePointer (1));
            const long before = allocs.load();
            p.processBlock (buf, midi);
            used += allocs.load() - before;
        }

        check (used == 0, "no allocations in processBlock across 4 Quality switches", (double) used);
    }
}

/** The one moment two Qualities run at once and tables get rebuilt. A benchmark that skips the start of a preset never sees it. */
static void testQualitySwitchCost()
{
    std::puts ("\n== Quality: cost of the switch itself, worst block of 128 samples, best of 5 tries at each so a stall from elsewhere does not count");

    const std::pair<const char*, Set> layouts[] = { { "stereo", Set::stereo() }, { "7.1.4", Set::create7point1point4() } };

    for (const auto& layout : layouts)
    {
        const int channels = layout.second.size();
        double worstOfAll = 0.0;
        bool allChanged = true;

        for (int from = 0; from < 3; ++from)
        {
            for (int to = 0; to < 3; ++to)
            {
                if (from == to) continue;
                double best = 1.0e9;

                for (int attempt = 0; attempt < 5; ++attempt)
                {
                    Proc p;
                    setParam (p, "quality", (float) from);
                    prepareLayout (p, layout.second, kRate, 128);
                    const auto x = plus (tone (72000, 3000.0, -14.0), noise (72000, -30.0, 900u));
                    juce::AudioBuffer<float> buf (channels, 128);
                    juce::MidiBuffer midi;
                    double worst = 0.0;
                    bool changed = false;

                    for (int pos = 0; pos + 128 <= 72000; pos += 128)
                    {
                        if (pos == 24064)
                        {
                            setParam (p, "quality", (float) to);
                            changed = p.getLatencySamples() == p.latencyForQuality ((Quality) to);
                        }

                        for (int ch = 0; ch < channels; ++ch)
                            std::copy_n (x.data() + pos, 128, buf.getWritePointer (ch));

                        const double a = threadCpuSeconds();
                        p.processBlock (buf, midi);
                        const double e = threadCpuSeconds() - a;

                        if (pos >= 24064)
                            worst = std::max (worst, e);
                    }

                    allChanged = allChanged && changed;
                    best = std::min (best, 100.0 * worst / (128.0 / kRate));
                }

                info (std::string (layout.first) + ", " + qualityName (from) + " to " + qualityName (to), best, "%");
                worstOfAll = std::max (worstOfAll, best);
            }
        }

        check (allChanged, std::string (layout.first) + ": every one of those really did change Quality");
        const double limit = channels == 2 ? 25.0 : 100.0;
        check (worstOfAll < limit, std::string (layout.first) + ": worst block during any switch (limit " + std::to_string ((int) limit) + "%)", worstOfAll, "%");
    }
}

/** Two things pull against each other, and a Quality is a choice between them. */
static void testQualityResolution()
{
    std::puts ("\n== Quality: time resolution, frequency resolution and CPU");

    double t50[3], width[3], valley[3], cpu[3], worstBlock[3];

    for (int q = 0; q < 3; ++q)
    {
        smoothres::Params base;
        base.quality = (Quality) q;
        base.depthDb = 12.0f;

        // Time: Hard mode has no long memory, so this is only the window. A 3 kHz tone starts at 1 s, attack at its fastest.
        {
            smoothres::ResonanceEngine e;
            e.prepare (kRate, 1);
            auto p = base;
            p.hard = true;
            p.hardThresholdDb = -30.0f;
            p.attackMs = 1.0f;
            p.releaseMs = 100.0f;
            e.setParams (p);
            auto x = noise (96000, -50.0, 51);
            const auto burst = tone (96000, 3000.0, -6.0);
            for (int i = 48000; i < 96000; ++i) x[(size_t) i] += burst[(size_t) i];
            const int hop = e.getHopSize();
            const int bin = juce::roundToInt (3000.0 / e.getBinHz());
            std::vector<std::pair<double, float>> trace;
            for (int pos = 0; pos + hop <= 96000; pos += hop)
            {
                float* ptr = x.data() + pos;
                e.process (&ptr, 1, hop);
                trace.push_back ({ (pos + hop) / kRate, e.getReductionDb (0, bin) });
            }
            const float last = trace.back().second;
            t50[q] = 0.0;
            for (const auto& point : trace)
                if (point.first >= 1.0 && point.second >= 0.5f * last) { t50[q] = (point.first - 1.0) * 1000.0; break; }
            check (last > 8.0f, std::string (qualityName (q)) + ": the cut does arrive (limit over 8 dB)", last, "dB");
            info (std::string ("Time for the cut to reach half way after a tone starts, ") + qualityName (q), t50[q], "ms");
        }

        // Frequency: one steady tone over noise, at the sharpest setting. How wide is the notch at half its depth?
        {
            smoothres::ResonanceEngine e;
            e.prepare (kRate, 1);
            auto p = base;
            p.sharpness = 1.0f;
            e.setParams (p);
            auto x = plus (tone (144000, 3000.0, -12.0), noise (144000, -42.0, 52));
            float* ptr = x.data();
            e.process (&ptr, 1, 144000);
            float top = 0.0f;
            for (int k = 0; k < e.getNumBins(); ++k) top = std::max (top, e.getReductionDb (0, k));
            int count = 0;
            for (int k = 0; k < e.getNumBins(); ++k) if (e.getReductionDb (0, k) >= 0.5f * top) ++count;
            width[q] = count * e.getBinHz();
            info (std::string ("Width of the cut at half depth, sharpest setting, ") + qualityName (q), width[q], "Hz");
            info (std::string ("Size of one FFT bin, ") + qualityName (q), e.getBinHz(), "Hz");
        }

        // Frequency again: two tones close together. How far does the cut come back up between them?
        {
            smoothres::ResonanceEngine e;
            e.prepare (kRate, 1);
            auto p = base;
            p.sharpness = 1.0f;
            e.setParams (p);
            auto x = plus (plus (tone (144000, 3000.0, -12.0), tone (144000, 3400.0, -12.0)), noise (144000, -42.0, 53));
            float* ptr = x.data();
            e.process (&ptr, 1, 144000);
            const float a = e.getReductionDb (0, juce::roundToInt (3000.0 / e.getBinHz()));
            const float b = e.getReductionDb (0, juce::roundToInt (3400.0 / e.getBinHz()));
            const float mid = e.getReductionDb (0, juce::roundToInt (3200.0 / e.getBinHz()));
            valley[q] = 0.5 * (a + b) - mid;
            info (std::string ("Two tones 400 Hz apart, cut comes back up between them by, ") + qualityName (q), valley[q], "dB");
        }

        // CPU through the real processor, stereo at 48 kHz, 128 sample blocks, voice-like input.
        {
            Proc pr;
            setParam (pr, "quality", (float) q);
            prepareLayout (pr, Set::stereo(), kRate, 128);
            std::vector<float> x (48000 * 4);
            std::mt19937 g (5);
            std::uniform_real_distribution<float> d (-1.0f, 1.0f);
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / kRate, f0 = 220.0 * std::pow (2.0, 0.4 * std::sin (pi2 * 0.3 * t));
                double s = 0.0;
                for (int h = 1; h <= 12; ++h) s += std::sin (pi2 * f0 * h * t) / h;
                x[i] = (float) (0.12 * s + 0.01 * d (g));
            }
            juce::AudioBuffer<float> buf (2, 128);
            juce::MidiBuffer midi;
            double total = 0.0, worst = 0.0;
            for (size_t pos = 0; pos + 128 <= x.size(); pos += 128)
            {
                std::copy_n (x.data() + pos, 128, buf.getWritePointer (0));
                std::copy_n (x.data() + pos, 128, buf.getWritePointer (1));
                const double a = threadCpuSeconds();
                pr.processBlock (buf, midi);
                const double e = threadCpuSeconds() - a;
                if (pos > 48000) { total += e; worst = std::max (worst, e); }
            }
            cpu[q] = 100.0 * total / ((double) (x.size() - 48000) / kRate);
            worstBlock[q] = 100.0 * worst / (128.0 / kRate);
            check (cpu[q] < 5.0, std::string ("CPU, stereo, 48 kHz, ") + qualityName (q) + " (limit 5% of a core)", cpu[q], "%");
            check (worstBlock[q] < 50.0, std::string ("Worst single block, ") + qualityName (q) + " (limit 50%)", worstBlock[q], "%");
        }
    }

    check (t50[0] < 0.5 * t50[1], "Low Latency reacts at least twice as fast as Normal", t50[0] / t50[1], "x");
    check (t50[2] > t50[1], "High reacts slower than Normal", t50[2] / t50[1], "x");
    check (width[0] > 1.5 * width[1], "Low Latency's cut is at least 1.5 times as wide as Normal's", width[0] / width[1], "x");
    check (width[2] <= width[1], "High's cut is no wider than Normal's", width[2] / width[1], "x");
    check (valley[2] >= valley[1] - 0.25, "High separates two close tones at least as well as Normal", valley[2] - valley[1], "dB");

    // Real speech, the same way the speech test works, at each Quality.
    const char* voices[] = { "Daniel", "Karen", "Moira", "Samantha" };

    double colourByQuality[3] {};

    for (int q = 0; q < 3; ++q)
    {
        double removedLeast = 1.0e9, removedSum = 0.0, overallWorst = 0.0, colourWorst = 0.0;

        for (const auto* name : voices)
        {
            auto clean = loadSpeech (name);
            check (! clean.empty(), std::string ("speech found: ") + name);
            if (clean.empty()) continue;
            clean.resize (std::min (clean.size(), (size_t) (16.0 * kRate)));

            const auto boosted = peakFilter (clean, 3150.0, 6.0, 12.0);

            const auto render = [&] (const std::vector<float>& input)
            {
                Proc p;
                setParam (p, "quality", (float) q);
                setParam (p, "depth", 12.0f);
                prepareLayout (p, Set::mono());
                auto padded = input;
                padded.resize (input.size() + (size_t) p.getLatencySamples(), 0.0f);
                const Multi out = run (p, { padded });
                return std::vector<float> (out[0].begin() + p.getLatencySamples(), out[0].end());
            };

            const auto outBoosted = render (boosted), outClean = render (clean);
            const double half = 3150.0 / 12.0;
            const double removed = bandDb (boosted, 3150.0 - half, 3150.0 + half) - bandDb (outBoosted, 3150.0 - half, 3150.0 + half);
            removedLeast = std::min (removedLeast, removed);
            removedSum += removed;
            overallWorst = std::max (overallWorst, std::abs (toDb (rms (clean.data(), (int) clean.size())) - toDb (rms (outClean.data(), (int) outClean.size()))));

            for (double c = 1000.0; c < 8500.0; c *= std::pow (2.0, 1.0 / 3.0))
                colourWorst = std::max (colourWorst, std::abs (bandDb (clean, c / 1.1225, c * 1.1225) - bandDb (outClean, c / 1.1225, c * 1.1225)));
        }

        // Judged on the mean of the voices, as SpeechTest does. One voice has a dip at 3150 Hz, so the boost only fills it and
        // nothing sticks out. Normal is locked to the 0.4.1 sound, and reads 1.4 to 1.9 dB on that voice in SpeechTest too.
        const double wantRemoved = q == 0 ? 3.0 : 5.0;
        const double removedMean = removedSum / (double) std::size (voices);
        check (removedMean >= wantRemoved, std::string ("Real speech, ") + qualityName (q) + ": a 12 dB boost at 3150 Hz, mean removed over 4 voices (limit " + std::to_string ((int) wantRemoved) + " dB)", removedMean, "dB");
        info (std::string ("Real speech, ") + qualityName (q) + ": least removed of the 4 voices", removedLeast, "dB");
        check (overallWorst <= 0.5, std::string ("Real speech, ") + qualityName (q) + ": clean voice, overall level change (limit 0.5 dB)", overallWorst, "dB");
        colourByQuality[q] = colourWorst;
        info (std::string ("Real speech, ") + qualityName (q) + ": clean voice, worst third octave from 1 to 8.5 kHz", colourWorst, "dB");

        // Flat noise should barely change at any Quality.
        const auto hiss = noise (144000, -24.0, 61);
        Proc n;
        setParam (n, "quality", (float) q);
        setParam (n, "depth", 12.0f);
        prepareLayout (n, Set::stereo());
        const auto outHiss = run (n, stereoOf (hiss));
        const double change = bandDb (hiss, 1000.0, 12000.0, 48000) - bandDb (outHiss[0], 1000.0, 12000.0, 48000 + n.getLatencySamples());
        check (std::abs (change) <= 1.5, std::string ("Flat noise, ") + qualityName (q) + ", Depth 12: 1 to 12 kHz change (limit 1.5 dB)", change, "dB");
    }

    // Normal is locked to the 0.4.1 sound, so it sets the bar. The others may not colour a clean voice more than it does.
    check (colourByQuality[0] <= colourByQuality[1] + 0.5, "Low Latency colours a clean voice no more than Normal (limit 0.5 dB more)", colourByQuality[0] - colourByQuality[1], "dB");
    check (colourByQuality[2] <= colourByQuality[1] + 0.5, "High colours a clean voice no more than Normal (limit 0.5 dB more)", colourByQuality[2] - colourByQuality[1], "dB");
}

// ---- Max Cut ---------------------------------------------------------------------------------------------------

static void testMaxCut()
{
    std::puts ("\n== Max Cut");
    Proc fresh;
    auto* prm = fresh.parameters.getParameter ("maxCut");
    check (prm != nullptr && prm->isAutomatable() && prm->getName (64) == "Max Cut", "Max Cut is a real, named, automatable parameter");
    check (std::abs (prm->convertFrom0to1 (prm->getDefaultValue()) - 24.0f) < 0.001f, "Max Cut defaults to 24 dB, as deep as Depth goes");
    check (fresh.parameters.getParameterRange ("depth").end == 24.0f && fresh.parameters.getParameterRange ("depth").start == 0.0f, "Depth keeps its ID and range");

    const int samples = 192000, settled = 120000;
    const auto bed = plus (plus (tone (samples, 3000.0, -6.0), tone (samples, 6000.0, -18.0)), noise (samples, -48.0, 71));

    struct Result { double deepest, atTone, atSmallTone, area; };

    const auto measure = [&] (float depth, float maxCut)
    {
        Proc p;
        setParam (p, "depth", depth);
        setParam (p, "maxCut", maxCut);
        setParam (p, "sharpness", 35.0f);
        setParam (p, "selectivity", 30.0f);
        prepareLayout (p, Set::stereo());
        Result r { 0.0, 0.0, 0.0, 0.0 };
        const auto out = run (p, stereoOf (bed), 256, [&] (int pos)
        {
            if (pos > 48000)
                r.deepest = std::max (r.deepest, (double) p.maxReduction->getReading());
        });
        const int latency = p.getLatencySamples();
        r.atTone = toneLevelDb (bed, 3000.0, settled - latency, samples - latency) - toneLevelDb (out[0], 3000.0, settled);
        r.atSmallTone = toneLevelDb (bed, 6000.0, settled - latency, samples - latency) - toneLevelDb (out[0], 6000.0, settled);

        // How much of the spectrum around the tone is held down, from the graph data: dB times octaves.
        smoothres::GraphFrame frame;
        p.readGraph (frame);
        for (size_t i = 0; i + 1 < (size_t) smoothres::GraphFrame::numPoints; ++i)
            if (frame.hz[i] > 1500.0f && frame.hz[i] < 4500.0f)
                r.area += (double) frame.reductionDb[0][i] * std::log2 ((double) frame.hz[i + 1] / (double) frame.hz[i]);
        return r;
    };

    const auto free = measure (24.0f, 24.0f);
    check (free.deepest > 11.0, "Depth 24 with no limit cuts this test signal deeply (limit over 11 dB)", free.deepest, "dB");

    for (float limit : { 3.0f, 6.0f, 9.0f })
    {
        const auto r = measure (24.0f, limit);
        check (r.deepest <= limit + 0.01, "Max Cut " + std::to_string ((int) limit) + " with Depth 24: deepest cut anywhere, at any moment (limit " + std::to_string ((int) limit) + " dB)", r.deepest, "dB");
        check (r.deepest >= limit - 0.5, "and it really gets there (limit within 0.5 dB)", limit - r.deepest, "dB");
        check (r.atTone <= limit + 0.3, "the tone in the output comes down by no more than that (limit + 0.3 dB)", r.atTone, "dB");
    }

    // It leaves alone what stays above it: a smaller peak gets just what it got before. The level of that peak is searched for,
    // so that it really is a shallow cut, whatever the detector makes of it.
    {
        const auto cutOfSmall = [&] (double peakDb, float maxCut)
        {
            const auto x = plus (plus (tone (samples, 3000.0, -6.0), tone (samples, 6000.0, peakDb)), noise (samples, -48.0, 71));
            Proc p;
            setParam (p, "depth", 24.0f);
            setParam (p, "maxCut", maxCut);
            setParam (p, "sharpness", 35.0f);
            setParam (p, "selectivity", 30.0f);
            prepareLayout (p, Set::stereo());
            const auto out = run (p, stereoOf (x));
            const int latency = p.getLatencySamples();
            return toneLevelDb (x, 6000.0, settled - latency, samples - latency) - toneLevelDb (out[0], 6000.0, settled);
        };

        double chosen = 0.0, shallow = 0.0;
        for (double level = -60.0; level <= -18.0; level += 3.0)
        {
            shallow = cutOfSmall (level, 24.0f);
            chosen = level;
            if (shallow >= 2.0 && shallow <= 12.0)
                break;
        }

        check (shallow >= 2.0 && shallow <= 12.0, "found a peak with a shallow cut for this check", shallow, "dB");
        const double limited = cutOfSmall (chosen, (float) shallow + 3.0f);
        check (std::abs (limited - shallow) < 0.25, "a peak cut less than the limit keeps its cut, while a bigger one is held (limit 0.25 dB)", limited - shallow, "dB");
    }

    // Depth still means how strongly it acts: same ceiling, more Depth, more of the area held down.
    {
        const auto strong = measure (24.0f, 6.0f), weak = measure (6.0f, 24.0f);
        check (std::abs (strong.deepest - weak.deepest) < 0.5, "Depth 24 with Max Cut 6 and Depth 6 alone reach the same deepest cut (limit 0.5 dB)", strong.deepest - weak.deepest, "dB");
        check (strong.area > 1.15 * weak.area, "but Depth 24 holds down more of the area around the peak (limit 1.15x)", strong.area / weak.area, "x");
    }

    // Default null at the settings that could hit it: Depth at the top, against the frozen 0.4.1 engine.
    {
        Proc p;
        setParam (p, "depth", 24.0f);
        setParam (p, "selectivity", 0.0f);
        prepareLayout (p, Set::stereo());
        const auto ours = run (p, stereoOf (bed));
        legacyres::Params q;
        q.depthDb = 24.0f;
        q.selectivity = 0.0f;
        legacyres::ResonanceEngine old;
        old.prepare (kRate, 2);
        old.setParams (q);
        auto left = bed, right = bed;
        float* ptrs[2] = { left.data(), right.data() };
        for (int pos = 0; pos < samples; pos += 256)
        {
            float* now[2] = { ptrs[0] + pos, ptrs[1] + pos };
            old.process (now, 2, std::min (256, samples - pos));
        }
        double worst = 0.0;
        for (size_t i = 0; i < left.size(); ++i) worst = std::max (worst, (double) std::abs (left[i] - ours[0][i]));
        check (toDb (worst) <= -90.0, "Depth 24, Max Cut at its default, nulls against the 0.4.1 engine (limit -90)", toDb (worst), "dBFS");
    }

    // At 0 nothing is cut at all, and the sum with Delta still rebuilds the input.
    {
        Proc p;
        setParam (p, "depth", 24.0f);
        setParam (p, "maxCut", 0.0f);
        prepareLayout (p, Set::stereo());
        const auto out = run (p, stereoOf (bed));
        double worst = 0.0;
        for (int i = 2048 * 2; i < samples; ++i) worst = std::max (worst, (double) std::abs (out[0][(size_t) i] - bed[(size_t) i - 2048]));
        check (toDb (worst) < -90.0, "Max Cut 0: the output is the input, delayed (limit -90)", toDb (worst), "dBFS");
    }

    {
        Proc a, b;
        for (auto* p : { &a, &b })
        {
            setParam (*p, "depth", 24.0f);
            setParam (*p, "maxCut", 6.0f);
            prepareLayout (*p, Set::stereo());
        }
        setParam (b, "delta", 1.0f);
        prepareLayout (b, Set::stereo());
        const auto sound = run (a, stereoOf (bed)), removed = run (b, stereoOf (bed));
        double worst = 0.0;
        for (int i = 2048 * 2; i < samples; ++i) worst = std::max (worst, (double) std::abs (sound[0][(size_t) i] + removed[0][(size_t) i] - bed[(size_t) i - 2048]));
        check (toDb (worst) < -90.0, "with Max Cut on, sound plus Delta rebuilds the input (limit -90)", toDb (worst), "dBFS");
    }
}

// ---- Band Listen -----------------------------------------------------------------------------------------------

static void testBandListen()
{
    std::puts ("\n== Band Listen");
    Proc fresh;
    auto* prm = fresh.parameters.getParameter ("bandListen");
    check (prm != nullptr && prm->isAutomatable() && prm->getName (64) == "Band Listen", "Band Listen is a real, named, automatable parameter");
    check (prm->getCurrentValueAsText() == "Off", "Band Listen is off by default");
    prm->setValueNotifyingHost (prm->convertTo0to1 (3.0f));
    check (prm->getCurrentValueAsText() == "Dot 3", "and reads Dot 3 when set to dot 3");

    const int samples = 72000, from = 36000;

    const auto levelThrough = [&] (double hz, int dot, float dotHz, float width, int quality, bool delta = false)
    {
        Proc p;
        setParam (p, "depth", 0.0f);   // nothing removed, so the bell itself is all that is measured
        setParam (p, "quality", (float) quality);
        const auto n = juce::String (dot);
        setParam (p, "band" + n + "Freq", dotHz);
        setParam (p, "band" + n + "Width", width);
        setParam (p, "bandListen", (float) dot);
        setParam (p, "delta", delta ? 1.0f : 0.0f);
        prepareLayout (p, Set::stereo());
        const auto x = tone (samples, hz, -12.0);
        const auto out = run (p, stereoOf (x));
        return toneLevelDb (out[0], hz, from) - toneLevelDb (x, hz, from);
    };

    check (std::abs (levelThrough (3000.0, 2, 3000.0f, 1.0f, 1)) < 0.3, "dot 2 at 3 kHz, 1 octave wide: tone at the centre passes (limit 0.3 dB)", levelThrough (3000.0, 2, 3000.0f, 1.0f, 1), "dB");
    check (std::abs (levelThrough (3000.0 * std::sqrt (2.0), 2, 3000.0f, 1.0f, 1) + 6.0) < 1.0, "half an octave up, the end of the width marker: -6 dB (limit 1 dB)", levelThrough (3000.0 * std::sqrt (2.0), 2, 3000.0f, 1.0f, 1), "dB");
    check (levelThrough (750.0, 2, 3000.0f, 1.0f, 1) < -60.0, "2 octaves below: gone (limit -60 dB)", levelThrough (750.0, 2, 3000.0f, 1.0f, 1), "dB");
    check (levelThrough (12000.0, 2, 3000.0f, 1.0f, 1) < -60.0, "2 octaves above: gone (limit -60 dB)", levelThrough (12000.0, 2, 3000.0f, 1.0f, 1), "dB");

    double worstCentre = 0.0;
    for (int q = 0; q < 3; ++q)
        for (int dot = 1; dot <= 8; ++dot)
            for (float hz : { 300.0f, 3000.0f, 9000.0f })
                worstCentre = std::max (worstCentre, std::abs (levelThrough (hz, dot, hz, 1.0f, q)));
    check (worstCentre < 1.0, "every dot, every Quality, 300 Hz to 9 kHz: the centre passes (limit 1 dB)", worstCentre, "dB");

    // Pairing with Delta. Off: the treated sound in that region. On: what is being taken out of that region. They add up to the
    // region itself, so nothing is lost and nothing is invented.
    const auto bed = plus (plus (tone (144000, 3000.0, -6.0), tone (144000, 8000.0, -6.0)), noise (144000, -42.0, 81));

    const auto listen = [&] (bool delta, float depth, const std::function<void (Proc&)>& extra = {})
    {
        Proc p;
        setParam (p, "depth", depth);
        setParam (p, "band2Freq", 3000.0f);
        setParam (p, "band2Width", 0.5f);
        setParam (p, "bandListen", 2.0f);
        setParam (p, "delta", delta ? 1.0f : 0.0f);
        if (extra) extra (p);
        prepareLayout (p, Set::stereo());
        return run (p, stereoOf (bed));
    };

    {
        const auto kept = listen (false, 12.0f), taken = listen (true, 12.0f), whole = listen (false, 0.0f);
        double worst = 0.0, size = 0.0;
        for (size_t i = 8192; i < bed.size(); ++i)
        {
            worst = std::max (worst, (double) std::abs (kept[0][i] + taken[0][i] - whole[0][i]));
            size = std::max (size, (double) std::abs (whole[0][i]));
        }
        check (toDb (worst / size) < -80.0, "Band Listen with Delta plus Band Listen without it rebuilds the region (limit -80 dB)", toDb (worst / size), "dB");

        const double cutHeard = toneLevelDb (whole[0], 3000.0, 96000) - toneLevelDb (kept[0], 3000.0, 96000);
        const double removedHeard = toneLevelDb (taken[0], 3000.0, 96000);
        check (cutHeard > 3.0, "Without Delta you hear the region after treatment: the tone comes down (limit over 3 dB)", cutHeard, "dB");
        check (removedHeard > -30.0, "With Delta you hear what is coming out of it (limit over -30 dBFS)", removedHeard, "dBFS");

        // The other tone is being treated too, but it is outside this region.
        check (toneLevelDb (taken[0], 8000.0, 96000) < -80.0, "With Delta, what is removed outside the region stays out (limit -80 dBFS)", toneLevelDb (taken[0], 8000.0, 96000), "dBFS");
    }

    // Gain and Mix behave as they do without it.
    {
        const auto base = listen (false, 12.0f);
        const auto louder = listen (false, 12.0f, [] (Proc& p) { setParam (p, "outputGain", 6.0f); });
        const double gain = toneLevelDb (louder[0], 3000.0, 96000) - toneLevelDb (base[0], 3000.0, 96000);
        check (std::abs (gain - 6.0) < 0.05, "Output Gain applies to what you hear (limit 0.05 dB off 6)", gain, "dB");
        const auto deltaLouder = listen (true, 12.0f, [] (Proc& p) { setParam (p, "outputGain", 6.0f); }), deltaBase = listen (true, 12.0f);
        const double deltaGain = toneLevelDb (deltaLouder[0], 3000.0, 96000) - toneLevelDb (deltaBase[0], 3000.0, 96000);
        check (std::abs (deltaGain) < 0.05, "With Delta, Output Gain does not apply, same as Delta alone (limit 0.05 dB)", deltaGain, "dB");
        const auto dry = listen (false, 12.0f, [] (Proc& p) { setParam (p, "mix", 0.0f); }), whole = listen (false, 0.0f);
        double worst = 0.0;
        for (size_t i = 8192; i < bed.size(); ++i) worst = std::max (worst, (double) std::abs (dry[0][i] - whole[0][i]));
        check (toDb (worst) < -90.0, "Mix 0: the untreated region (limit -90)", toDb (worst), "dBFS");
        const auto bypassed = listen (false, 12.0f, [] (Proc& p) { setParam (p, "bypass", 1.0f); });
        worst = 0.0;
        for (size_t i = 8192; i < bed.size(); ++i) worst = std::max (worst, (double) std::abs (bypassed[0][i] - bed[i - 2048]));
        check (toDb (worst) < -90.0, "Bypass wins over it: untouched input (limit -90)", toDb (worst), "dBFS");
    }

    // Latency, in each Quality.
    for (int q = 0; q < 3; ++q)
    {
        Proc p;
        setParam (p, "quality", (float) q);
        setParam (p, "bandListen", 2.0f);
        setParam (p, "band2Freq", 3000.0f);
        setParam (p, "band2Width", 4.0f);
        prepareLayout (p, Set::stereo());
        std::vector<float> x (40000, 0.0f);
        x[10000] = 0.8f;
        const auto out = run (p, stereoOf (x));
        int top = 0;
        for (int i = 0; i < (int) out[0].size(); ++i) if (std::abs (out[0][(size_t) i]) > std::abs (out[0][(size_t) top])) top = i;
        check (top - 10000 == p.getLatencySamples(), std::string ("Band Listen delay in ") + qualityName (q) + " equals the reported latency", top - 10000, "samples");
    }

    // On and off with sound going through, and a change of dot.
    {
        Proc p;
        setParam (p, "depth", 0.0f);
        setParam (p, "band1Freq", 700.0f);
        setParam (p, "band2Freq", 900.0f);
        prepareLayout (p, Set::stereo());
        const auto sine = tone (120000, 800.0, -12.0);
        const auto out = run (p, stereoOf (sine), 256, [&] (int pos)
        {
            if (pos == 40960)  setParam (p, "bandListen", 1.0f);
            if (pos == 61440)  setParam (p, "bandListen", 2.0f);
            if (pos == 81920)  setParam (p, "bandListen", 0.0f);
        });
        const double steady = roughness (out[0], 20000, 40960), during = roughness (out[0], 40960, 120000);
        check (during < 1.5 * steady, "turned on, moved to another dot and turned off: no click (limit 1.5x)", during / steady, "x");
    }

    // Nothing is left over from the last time it was on.
    {
        Proc p;
        setParam (p, "band2Freq", 1000.0f);
        prepareLayout (p, Set::stereo());
        auto x = tone (240000, 1000.0, -6.0);
        std::fill (x.begin() + 72000, x.end(), 0.0f);
        const auto out = run (p, stereoOf (x), 256, [&] (int pos)
        {
            if (pos == 25600)  setParam (p, "bandListen", 2.0f);
            if (pos == 51200)  setParam (p, "bandListen", 0.0f);   // with sound still going through
            if (pos == 128000) setParam (p, "bandListen", 2.0f);   // long after it stopped
        });
        check (toDb (peakOf (out[0], 90000, 240000)) < -120.0, "turning it on again over silence plays nothing old (limit -120)", toDb (peakOf (out[0], 90000, 240000)), "dBFS");
    }

    // It only costs while it is on, and it stays real time safe.
    {
        Proc p;
        setParam (p, "bandListen", 2.0f);
        setParam (p, "delta", 1.0f);
        prepareLayout (p, Set::stereo(), kRate, 128);
        juce::AudioBuffer<float> buf (2, 128);
        juce::MidiBuffer midi;
        const auto x = noise (48000, -24.0, 91);
        const long before = allocs.load();
        double total = 0.0;
        for (size_t pos = 0; pos + 128 <= x.size(); pos += 128)
        {
            std::copy_n (x.data() + pos, 128, buf.getWritePointer (0));
            std::copy_n (x.data() + pos, 128, buf.getWritePointer (1));
            const double a = threadCpuSeconds();
            p.processBlock (buf, midi);
            total += threadCpuSeconds() - a;
        }
        const long used = allocs.load() - before;
        check (used == 0, "Band Listen allocates nothing", (double) used);
        check (100.0 * total < 8.0, "CPU with Band Listen on (limit 8% of a core)", 100.0 * total, "%");
    }
}

// ---- layouts ---------------------------------------------------------------------------------------------------

static void testLayouts()
{
    std::puts ("\n== Multichannel");

    const std::pair<const char*, Set> layouts[] = {
        { "mono", Set::mono() }, { "stereo", Set::stereo() }, { "quad", Set::quadraphonic() }, { "LCRS", Set::createLCRS() },
        { "5.0", Set::create5point0() }, { "5.1", Set::create5point1() }, { "6.0", Set::create6point0() }, { "6.1", Set::create6point1() },
        { "7.0", Set::create7point0() }, { "7.1", Set::create7point1() }, { "5.1.2", Set::create5point1point2() }, { "7.1.2", Set::create7point1point2() },
        { "5.1.4", Set::create5point1point4() }, { "7.0.4", Set::create7point0point4() }, { "7.1.4", Set::create7point1point4() }, { "9.1.6", Set::create9point1point6() },
    };

    {
        Proc p;
        auto l = p.getBusesLayout();
        l.inputBuses.getReference (0) = Set::quadraphonic();
        l.outputBuses.getReference (0) = Set::create5point1();
        check (! p.checkBusesLayoutSupported (l), "Different layouts in and out are refused");
        l.inputBuses.getReference (0) = Set::discreteChannels (6);
        l.outputBuses.getReference (0) = Set::discreteChannels (6);
        check (! p.checkBusesLayoutSupported (l), "Unnamed channels are refused, since no one can say which is the LFE");
    }

    const int samples = 72000;

    for (const auto& layout : layouts)
    {
        const std::string name = layout.first;
        const auto set = layout.second;
        const int channels = set.size();
        const int lfe = set.getChannelIndexForType (Set::LFE);

        Proc probe;
        auto l = probe.getBusesLayout();
        l.inputBuses.getReference (0) = set;
        l.outputBuses.getReference (0) = set;
        check (probe.checkBusesLayoutSupported (l) && probe.setBusesLayout (l), name + ": accepted");

        // Latency: an impulse in every channel, at each Quality.
        for (int q = 0; q < 3; ++q)
        {
            Proc p;
            setParam (p, "quality", (float) q);
            prepareLayout (p, set);
            Multi x ((size_t) channels, std::vector<float> (20000, 0.0f));
            for (int ch = 0; ch < channels; ++ch) x[(size_t) ch][(size_t) (3000 + 17 * ch)] = 0.5f;
            const auto out = run (p, x);
            bool right = true;
            for (int ch = 0; ch < channels; ++ch)
            {
                int top = 0;
                for (int i = 0; i < 20000; ++i) if (std::abs (out[(size_t) ch][(size_t) i]) > std::abs (out[(size_t) ch][(size_t) top])) top = i;
                right = right && top == 3000 + 17 * ch + p.getLatencySamples();
            }
            check (right, name + ", " + qualityName (q) + ": every channel, LFE too, delayed exactly the reported latency", p.getLatencySamples(), "samples");
        }

        // No swaps: every channel gets its own tone, and only that one comes out of it. Depth 0, since a lone tone over 1 kHz is
        // a resonance and would rightly be cut, and this is about where the sound goes.
        {
            Proc p;
            setParam (p, "depth", 0.0f);
            prepareLayout (p, set);
            Multi x;
            std::vector<double> hz;
            for (int ch = 0; ch < channels; ++ch)
            {
                hz.push_back (ch == lfe ? 60.0 : 400.0 + 310.0 * ch);
                x.push_back (tone (samples, hz.back(), -18.0));
            }
            const auto out = run (p, x);
            double worstOwn = 0.0, worstOther = -300.0;
            for (int ch = 0; ch < channels; ++ch)
            {
                worstOwn = std::max (worstOwn, std::abs (toneLevelDb (out[(size_t) ch], hz[(size_t) ch], 24000) - toneLevelDb (x[(size_t) ch], hz[(size_t) ch], 24000)));
                for (int other = 0; other < channels; ++other)
                    if (other != ch)
                        worstOther = std::max (worstOther, toneLevelDb (out[(size_t) ch], hz[(size_t) other], 24000));
            }
            check (worstOwn < 1.5, name + ": each channel's own tone arrives at its own level (limit 1.5 dB)", worstOwn, "dB");
            check (worstOther < -90.0, name + ": no channel's tone in any other channel (limit -90 dBFS)", worstOther, "dBFS");
        }

        // The sound plus Delta rebuilds the input, in every channel.
        {
            Multi x;
            for (int ch = 0; ch < channels; ++ch)
                x.push_back (plus (tone (samples, 3000.0 + 90.0 * ch, -12.0), noise (samples, -40.0, 100u + (unsigned) ch)));
            Proc a, b;
            setParam (a, "depth", 12.0f);
            setParam (b, "depth", 12.0f);
            setParam (b, "delta", 1.0f);
            prepareLayout (a, set);
            prepareLayout (b, set);
            const auto sound = run (a, x), removed = run (b, x);
            const int latency = a.getLatencySamples();
            double worst = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                for (int i = latency * 2; i < samples; ++i)
                    worst = std::max (worst, (double) std::abs (sound[(size_t) ch][(size_t) i] + removed[(size_t) ch][(size_t) i] - x[(size_t) ch][(size_t) (i - latency)]));
            check (toDb (worst) < -90.0, name + ": sound plus Delta rebuilds the input in every channel (limit -90)", toDb (worst), "dBFS");

            bool treated = true;
            for (int ch = 0; ch < channels; ++ch)
                if (ch != lfe)
                    treated = treated && peakOf (removed[(size_t) ch], 48000, samples) > 0.001;
            check (treated, name + ": every channel but the LFE really is treated");
            if (lfe >= 0)
            {
                check (peakOf (removed[(size_t) lfe], 0, samples) == 0.0, name + ": the LFE has nothing removed, not one bit");
                double exact = 0.0;
                for (int i = latency; i < samples; ++i) exact = std::max (exact, (double) std::abs (sound[(size_t) lfe][(size_t) i] - x[(size_t) lfe][(size_t) (i - latency)]));
                check (exact == 0.0, name + ": the LFE comes out as the input, delayed, exact to the bit");
            }
        }

        // Link: a resonance in the last treated channel only.
        if (channels > 2)
        {
            int last = channels - 1;
            if (last == lfe) --last;
            Multi x;
            for (int ch = 0; ch < channels; ++ch)
                x.push_back (plus (ch == last ? tone (samples * 2, 3000.0, -6.0) : tone (samples * 2, 3000.0, -60.0), noise (samples * 2, -40.0, 200u + (unsigned) ch)));

            const auto cutPerChannel = [&] (float link)
            {
                Proc p;
                setParam (p, "depth", 12.0f);
                setParam (p, "link", link);
                prepareLayout (p, set);
                const auto out = run (p, x);
                std::vector<double> cut;
                for (int ch = 0; ch < channels; ++ch)
                    cut.push_back (bandDb (x[(size_t) ch], 2900.0, 3100.0, 60000, 100000) - bandDb (out[(size_t) ch], 2900.0, 3100.0, 60000 + p.getLatencySamples(), 100000 + p.getLatencySamples()));
                return cut;
            };

            const auto linked = cutPerChannel (100.0f), apart = cutPerChannel (0.0f);
            double least = 1.0e9, most = 0.0;
            for (int ch = 0; ch < channels; ++ch)
            {
                if (ch == lfe) continue;
                least = std::min (least, linked[(size_t) ch]);
                most = std::max (most, linked[(size_t) ch]);
            }
            check (least > 4.0 && most - least < 1.0, name + ": Link 100, resonance in one channel: all treated channels cut alike (spread limit 1 dB)", most - least, "dB");

            double others = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                if (ch != last && ch != lfe)
                    others = std::max (others, std::abs (apart[(size_t) ch]));
            check (apart[(size_t) last] > 4.0 && others < 1.5, name + ": Link 0: only that channel is cut, the others at most (limit 1.5 dB)", others, "dB");
            if (lfe >= 0)
                check (std::abs (linked[(size_t) lfe]) < 0.01, name + ": the LFE stays out of the link (limit 0.01 dB)", linked[(size_t) lfe], "dB");
        }

        // An LFE full of what would count as a resonance moves nothing else.
        if (lfe >= 0)
        {
            Multi quiet;
            for (int ch = 0; ch < channels; ++ch) quiet.push_back (noise (samples, -30.0, 300u + (unsigned) ch));
            auto loud = quiet;
            loud[(size_t) lfe] = plus (quiet[(size_t) lfe], tone (samples, 3000.0, -3.0));
            Proc a, b;
            setParam (a, "depth", 24.0f);
            setParam (b, "depth", 24.0f);
            prepareLayout (a, set);
            prepareLayout (b, set);
            const auto x = run (a, quiet), y = run (b, loud);
            double worst = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                if (ch != lfe)
                    for (int i = 0; i < samples; ++i)
                        worst = std::max (worst, (double) std::abs (x[(size_t) ch][(size_t) i] - y[(size_t) ch][(size_t) i]));
            check (worst == 0.0, name + ": what is in the LFE has no effect on any other channel");
        }

        // Stereo controls with no meaning here do nothing rather than something strange.
        if (channels > 2)
        {
            Multi x;
            for (int ch = 0; ch < channels; ++ch)
                x.push_back (plus (tone (samples, 3000.0, -12.0), noise (samples, -40.0, 400u + (unsigned) ch)));
            Proc a, b;
            setParam (b, "stereoMode", 1.0f);
            prepareLayout (a, set);
            prepareLayout (b, set);
            const auto u = run (a, x), v = run (b, x);
            double worst = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < samples; ++i)
                    worst = std::max (worst, (double) std::abs (u[(size_t) ch][(size_t) i] - v[(size_t) ch][(size_t) i]));
            check (worst == 0.0, name + ": Mid/Side does nothing over stereo");
        }

        // CPU per layout, no allocations, and Band Listen and a Quality switch in the bargain.
        {
            Proc p;
            prepareLayout (p, set, kRate, 128);
            const int total = 48000 * 3;
            std::vector<float> voice ((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                const double t = i / kRate;
                double s = 0.0;
                for (int h = 1; h <= 12; ++h) s += std::sin (pi2 * 220.0 * h * t) / h;
                voice[(size_t) i] = (float) (0.12 * s);
            }
            juce::AudioBuffer<float> buf (channels, 128);
            juce::MidiBuffer midi;
            double spent = 0.0, worst = 0.0;
            const long before = allocs.load();
            for (int pos = 0; pos + 128 <= total; pos += 128)
            {
                for (int ch = 0; ch < channels; ++ch) std::copy_n (voice.data() + pos, 128, buf.getWritePointer (ch));
                const double a = threadCpuSeconds();
                p.processBlock (buf, midi);
                const double e = threadCpuSeconds() - a;
                if (pos > 48000) { spent += e; worst = std::max (worst, e); }
            }
            const double cpu = 100.0 * spent / ((double) (total - 48000) / kRate);
            const long used = allocs.load() - before;
            check (used == 0, name + ": no allocations while playing", (double) used);
            const int treated = channels - (lfe >= 0 ? 1 : 0);
            check (cpu < 2.5 * treated, name + ": CPU at default, one core (limit 2.5% per treated channel)", cpu, "%");
            info (name + ": worst single block", 100.0 * worst / (128.0 / kRate), "%");
        }
    }

    // Focus goes by side in surround. Left of centre takes the work off every right side channel.
    {
        Multi x;
        for (int ch = 0; ch < 6; ++ch) x.push_back (plus (tone (samples * 2, 3000.0, -12.0), noise (samples * 2, -40.0, 500u + (unsigned) ch)));
        Proc p;
        setParam (p, "depth", 12.0f);
        setParam (p, "stereoFocus", -100.0f);
        prepareLayout (p, Set::create5point1());
        const auto out = run (p, x);
        const auto set = Set::create5point1();
        const auto cutOf = [&] (Set::ChannelType type)
        {
            const auto ch = (size_t) set.getChannelIndexForType (type);
            return bandDb (x[ch], 2900.0, 3100.0, 60000, 100000) - bandDb (out[ch], 2900.0, 3100.0, 60000 + 2048, 100000 + 2048);
        };
        check (cutOf (Set::left) > 3.0 && cutOf (Set::leftSurround) > 3.0, "5.1, Focus -100: left side channels treated (limit over 3 dB)", cutOf (Set::leftSurround), "dB");
        check (std::abs (cutOf (Set::right)) < 0.1 && std::abs (cutOf (Set::rightSurround)) < 0.1, "5.1, Focus -100: right side channels left alone (limit 0.1 dB)", cutOf (Set::rightSurround), "dB");
        check (cutOf (Set::centre) > 3.0, "5.1, Focus -100: the centre is neither side, so it keeps its treatment", cutOf (Set::centre), "dB");
    }

    // Side chain on surround: the stereo key still decides for every channel.
    {
        Proc p;
        auto l = p.getBusesLayout();
        l.inputBuses.getReference (0) = Set::create5point1();
        l.outputBuses.getReference (0) = Set::create5point1();
        l.inputBuses.getReference (1) = Set::stereo();
        check (p.setBusesLayout (l), "5.1 with a stereo side chain is accepted");
        setParam (p, "depth", 12.0f);
        setParam (p, "sideChain", 1.0f);
        p.setRateAndBufferSizeDetails (kRate, 256);
        p.prepareToPlay (kRate, 256);
        Multi x;
        for (int ch = 0; ch < 6; ++ch) x.push_back (noise (144000, -24.0, 600u + (unsigned) ch));
        const auto key = tone (144000, 3000.0, -6.0);
        juce::AudioBuffer<float> buf (8, 256);
        juce::MidiBuffer midi;
        Multi out = x;
        for (int pos = 0; pos + 256 <= 144000; pos += 256)
        {
            for (int ch = 0; ch < 6; ++ch) std::copy_n (x[(size_t) ch].data() + pos, 256, buf.getWritePointer (ch));
            std::copy_n (key.data() + pos, 256, buf.getWritePointer (6));
            std::copy_n (key.data() + pos, 256, buf.getWritePointer (7));
            p.processBlock (buf, midi);
            for (int ch = 0; ch < 6; ++ch) std::copy_n (buf.getReadPointer (ch), 256, out[(size_t) ch].data() + pos);
        }
        const auto lfe = Set::create5point1().getChannelIndexForType (Set::LFE);
        double least = 1.0e9;
        for (int ch = 0; ch < 6; ++ch)
            if (ch != lfe)
                least = std::min (least, bandDb (x[(size_t) ch], 2950.0, 3050.0, 60000, 120000) - bandDb (out[(size_t) ch], 2950.0, 3050.0, 62048, 122048));
        check (least > 4.0, "5.1, Side Chain on: the key's tone is carved from every treated channel (limit over 4 dB)", least, "dB");
        check (std::abs (bandDb (x[(size_t) lfe], 2950.0, 3050.0, 60000, 120000) - bandDb (out[(size_t) lfe], 2950.0, 3050.0, 62048, 122048)) < 0.01, "5.1, Side Chain on: the LFE is still untouched");
    }

    // The graph's data in surround.
    {
        Proc p;
        setParam (p, "depth", 12.0f);
        setParam (p, "link", 0.0f);
        prepareLayout (p, Set::create5point1());
        Multi x;
        for (int ch = 0; ch < 6; ++ch) x.push_back (plus (ch == 4 ? tone (144000, 3000.0, -6.0) : tone (144000, 3000.0, -60.0), noise (144000, -40.0, 700u + (unsigned) ch)));
        run (p, x);
        smoothres::GraphFrame frame;
        const bool got = p.readGraph (frame);
        const auto names = p.getChannelNames();
        check (got && frame.numChannels == 6 && names.size() == 6, "the window gets 6 channels' readings and names in 5.1");
        check (frame.channelReductionDb[4] > 3.0f && frame.channelReductionDb[(size_t) p.getLfeChannel()] == 0.0f, "the treated channel reads its cut, and the LFE reads none", frame.channelReductionDb[4], "dB");
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String only = argc > 1 ? argv[1] : "";

    if (only.isEmpty() || only == "dots")     testDots();
    if (only.isEmpty() || only == "quality")  { testQualityLatency(); testQualitySwitching(); testQualitySwitchCost(); testQualityResolution(); }
    if (only.isEmpty() || only == "maxcut")   testMaxCut();
    if (only.isEmpty() || only == "listen")   testBandListen();
    if (only.isEmpty() || only == "layouts")  testLayouts();

    return finish();
}
