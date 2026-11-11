// Held Notes: the control added in 0.6.0. Off, which is the default, is the 0.5.1 sound to the sample. On, Soft mode also takes
// down a single harmonic that rings on a held note. Everything here goes through the real processor and its real parameters,
// looked up by ID, so this file builds against a version that has no such control and fails there for that reason.
//
// The ring in these signals stands 12 dB over the two harmonics either side of it, as the median ring on Luke's real takes does
// (12.1 dB). The first version of this file, committed before the control existed, used a fixed +14 dB filter, which only stood
// 9 to 10 dB tall once its skirts and the formants beside it were counted; the limits below were not moved.
//
// Limits come from the real vocal checks (realvox src/extra/resonance.cpp): a ring that lasts comes down 3 dB or more at the
// default Depth of 6, measured from 100 ms after it starts; S and consonants are dulled no more than 1.5 dB, and no more than
// 0.3 dB further than with the control off.
#include "TestKit.h"
#include "PluginProcessor.h"
#include "editor/ResonanceEditor.h"
#include "legacy/LegacyEngine.h"

#include <juce_dsp/juce_dsp.h>

using namespace testkit;
using Proc = SmoothResonanceProcessor;

#ifndef BASELINE_DIR
 #define BASELINE_DIR "tests/baseline"
#endif

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256, kLatency = 2048;
static constexpr double kNote = 0.75, kSung = 0.6;   // a note every 0.75 s, sung for 0.6 s
static const char* const kId = "heldNotes";

// ---- the processor ------------------------------------------------------------------------------------------------

static juce::RangedAudioParameter* control (Proc& p)  { return p.parameters.getParameter (kId); }

/** False when this build has no such control. */
static bool setHeld (Proc& p, bool on)
{
    if (auto* c = control (p))
    {
        c->setValueNotifyingHost (on ? 1.0f : 0.0f);
        return true;
    }

    return false;
}

static bool isHeld (Proc& p)  { auto* c = control (p); return c != nullptr && c->getValue() > 0.5f; }

static void setParam (Proc& p, const char* id, float value)
{
    auto* prm = p.parameters.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static float real (Proc& p, const juce::String& id)
{
    auto* prm = p.parameters.getParameter (id);
    return prm->convertFrom0to1 (prm->getValue());
}

static void prepare (Proc& p, int block = kBlock)
{
    p.releaseResources();
    p.setRateAndBufferSizeDetails (kRate, block);
    p.prepareToPlay (kRate, block);
}

/** Stereo with the same signal on both sides. Returns the left output, delay left in. beforeBlock runs ahead of each block. */
static std::vector<float> render (Proc& p, const std::vector<float>& x, int block = kBlock, const std::function<void (int)>& beforeBlock = {})
{
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    std::vector<float> out (x.size());
    const int total = (int) x.size();

    for (int pos = 0; pos < total; pos += block)
    {
        const int len = std::min (block, total - pos);

        if (beforeBlock)
            beforeBlock (pos);

        buf.setSize (2, len, false, false, true);
        std::copy_n (x.data() + pos, len, buf.getWritePointer (0));
        std::copy_n (x.data() + pos, len, buf.getWritePointer (1));
        p.processBlock (buf, midi);
        std::copy_n (buf.getReadPointer (0), len, out.data() + pos);
    }

    return out;
}

static std::vector<float> renderWith (const std::vector<float>& x, bool held, const std::function<void (Proc&)>& setUp = {})
{
    Proc p;

    if (setUp)
        setUp (p);

    setHeld (p, held);
    prepare (p);
    return render (p, x);
}

/** The frozen 0.4.1 engine with the processor's settings: the sound of 0.5.1 and of every session saved before 0.6.0. */
static std::vector<float> renderOldEngine (Proc& p, const std::vector<float>& x)
{
    legacyres::Params q;
    q.depthDb = real (p, "depth");
    q.sharpness = real (p, "sharpness") * 0.01f;
    q.selectivity = real (p, "selectivity") * 0.01f;
    q.attackMs = real (p, "attack");
    q.releaseMs = real (p, "release");
    q.lowHz = real (p, "lowFreq");
    q.highHz = real (p, "highFreq");
    q.mix = real (p, "mix") * 0.01f;
    q.outputDb = real (p, "outputGain");
    q.hard = real (p, "mode") > 0.5f;
    q.hardThresholdDb = real (p, "hardThreshold");
    q.link = real (p, "link") * 0.01f;

    for (int b = 0; b < legacyres::Params::numBands; ++b)
    {
        const auto n = juce::String (b + 1);
        q.bands[b] = { real (p, "band" + n + "Freq"), real (p, "band" + n + "Sens"), real (p, "band" + n + "Width") };
    }

    legacyres::ResonanceEngine engine;
    engine.prepare (kRate, 2);
    engine.setParams (q);
    std::vector<float> left (x), right (x);

    for (int pos = 0; pos < (int) x.size(); pos += kBlock)
    {
        const int len = std::min (kBlock, (int) x.size() - pos);
        float* ptrs[2] = { left.data() + pos, right.data() + pos };
        engine.process (ptrs, 2, len);
    }

    return left;
}

static double nullDb (const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;

    if (a.size() != b.size())
        return 999.0;

    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));

    return toDb (worst);
}

// ---- signals. None of them is a real voice --------------------------------------------------------------------------

static std::vector<float> peakFilter (std::vector<float> x, double hz, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * hz / kRate;
    const double alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
    const double a0 = 1.0 + alpha / A;
    const double b0 = (1.0 + alpha * A) / a0, b1 = -2.0 * c / a0, b2 = (1.0 - alpha * A) / a0, a1 = -2.0 * c / a0, a2 = (1.0 - alpha / A) / a0;
    double z1 = 0, z2 = 0;

    for (auto& v : x)
    {
        const double in = v, out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        v = (float) out;
    }

    return x;
}

static std::vector<float> highPass (std::vector<float> x, double hz)
{
    const double w = juce::MathConstants<double>::twoPi * hz / kRate, c = std::cos (w), alpha = std::sin (w) / (2.0 * 0.707), a0 = 1.0 + alpha;
    const double b0 = (1.0 + c) * 0.5 / a0, b1 = -(1.0 + c) / a0, b2 = b0, a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;

    for (int pass = 0; pass < 2; ++pass)
    {
        double z1 = 0, z2 = 0;

        for (auto& v : x)
        {
            const double in = v, out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            v = (float) out;
        }
    }

    return x;
}

/** Notes with a little vibrato, 0.6 s each with 0.15 s between, whose harmonics stay level from 1.5 to 5 kHz. */
static std::vector<float> heldNotes (double seconds, double f0)
{
    std::vector<float> x ((size_t) (seconds * kRate), 0.0f);
    const int harmonics = (int) std::floor (9000.0 / f0);
    std::vector<double> phases ((size_t) harmonics + 1, 0.0);
    const double twoPi = juce::MathConstants<double>::twoPi;

    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / kRate, inNote = std::fmod (t, kNote);
        const double f = f0 * (1.0 + 0.012 * std::sin (twoPi * 5.5 * t));
        const double gate = inNote < kSung ? std::min ({ 1.0, inNote / 0.02, (kSung - inNote) / 0.02 }) : 0.0;
        double y = 0.0;

        for (int h = 1; h <= harmonics; ++h)
        {
            phases[(size_t) h] += twoPi * h * f / kRate;
            const double hz = h * f;
            y += std::sin (phases[(size_t) h]) / (hz < 1500.0 ? std::pow (hz / 1500.0, 0.7) * 6.0 : (hz < 5000.0 ? 6.0 : 6.0 * std::pow (hz / 5000.0, 2.0)));
        }

        x[i] = (float) (0.05 * gate * y);
    }

    return x;
}

static double ringHzFor (double f0)  { return std::round (3000.0 / f0) * f0; }

static double bandDb (const std::vector<float>& x, int delay, const std::vector<std::pair<int, int>>& spans, double lowHz, double highHz);
static std::vector<std::pair<int, int>> ringSpans (double seconds);

/** How far the harmonic at the ring stands over the mean of the two harmonics below it and the two above, in dB: the measure the
    real vocal check uses to call something a ring. */
static double ringStands (const std::vector<float>& x, double seconds, double f0)
{
    const double hz = ringHzFor (f0);
    const auto spans = ringSpans (seconds);
    const auto power = [&] (double at) { return std::pow (10.0, 0.1 * bandDb (x, 0, spans, at - 40.0, at + 40.0)); };
    return 10.0 * std::log10 (power (hz) / (0.25 * (power (hz - 2.0 * f0) + power (hz - f0) + power (hz + f0) + power (hz + 2.0 * f0))));
}

static constexpr double kRingStands = 12.0;   // the median ring on Luke's real takes stands 12.1 dB over its neighbours

/** Those notes with a formant at 2200 Hz and one at 4100 Hz, and between them a narrow ring on the harmonic nearest 3000 Hz:
    one harmonic standing 12 dB over the ones beside it for as long as each note lasts, with taller formants either side.
    The ring is a peak filter of Q 20 whose gain is raised in half dB steps until the harmonic stands that tall, because the
    filter's skirts and the formants' slopes lift the neighbours too. */
static std::vector<float> ringingVoice (double seconds, double f0, bool withRing = true)
{
    auto voice = peakFilter (peakFilter (heldNotes (seconds, f0), 2200.0, 5.0, 12.0), 4100.0, 5.0, 12.0);

    if (! withRing)
        return voice;

    for (double gain = 10.0; gain < 30.0; gain += 0.5)
    {
        auto ringing = peakFilter (voice, ringHzFor (f0), 20.0, gain);

        if (ringStands (ringing, seconds, f0) >= kRingStands)
            return ringing;
    }

    return peakFilter (voice, ringHzFor (f0), 20.0, 30.0);
}

struct Spoken { std::vector<float> x; std::vector<int> sStarts, tStarts; int sLength = 0; };

/** The ringing voice with an S straight after every note and a T just before the next one. */
static Spoken voiceWithConsonants (double seconds, double f0)
{
    Spoken s;
    s.x = ringingVoice (seconds, f0);
    s.sLength = (int) (0.09 * kRate);
    std::mt19937 rng (77);
    std::normal_distribution<float> noise (0.0f, 1.0f);
    std::vector<float> hiss (s.x.size(), 0.0f), burst (s.x.size(), 0.0f);

    for (double t0 = 0.0; t0 + kNote < seconds; t0 += kNote)
    {
        const int sStart = (int) ((t0 + kSung - 0.01) * kRate), tStart = (int) ((t0 + kNote - 0.03) * kRate);
        s.sStarts.push_back (sStart);
        s.tStarts.push_back (tStart);

        for (int i = 0; i < s.sLength; ++i)
            hiss[(size_t) (sStart + i)] = 0.25f * noise (rng) * (float) std::min ({ 1.0, i / 240.0, (s.sLength - i) / 240.0 });

        for (int i = 0; i < (int) (0.025 * kRate); ++i)
            burst[(size_t) (tStart + i)] = 0.3f * noise (rng) * (float) std::exp (-i / 250.0);
    }

    hiss = highPass (hiss, 5000.0);
    burst = highPass (burst, 2000.0);

    for (size_t i = 0; i < s.x.size(); ++i)
        s.x[i] += hiss[i] + burst[i];

    return s;
}

// ---- measuring ----------------------------------------------------------------------------------------------------

/** Power from lowHz to highHz in 2048 sample frames whose centre lies in [from, to), in dB. delay shifts the signal back. */
static double bandDb (const std::vector<float>& x, int delay, const std::vector<std::pair<int, int>>& spans, double lowHz, double highHz)
{
    constexpr int order = 11, n = 1 << order, hop = 256;
    static juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) n * 2);
    const int first = (int) std::ceil (lowHz * n / kRate), last = (int) std::floor (highHz * n / kRate);
    double sum = 0.0;

    for (const auto& span : spans)
        for (int centre = span.first; centre < span.second; centre += hop)
        {
            const int start = centre - n / 2 + delay;

            if (start < 0 || start + n > (int) x.size())
                continue;

            for (int i = 0; i < n; ++i)
                buf[(size_t) i] = x[(size_t) (start + i)] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n));

            fft.performRealOnlyForwardTransform (buf.data(), true);

            for (int k = first; k <= last; ++k)
                sum += (double) buf[(size_t) k * 2] * buf[(size_t) k * 2] + (double) buf[(size_t) k * 2 + 1] * buf[(size_t) k * 2 + 1];
        }

    return 10.0 * std::log10 (std::max (sum, 1.0e-30));
}

/** Each note from 100 ms after it starts to 30 ms before it stops, leaving out the first two notes. */
static std::vector<std::pair<int, int>> ringSpans (double seconds)
{
    std::vector<std::pair<int, int>> spans;

    for (double t0 = 2.0 * kNote; t0 + kNote < seconds; t0 += kNote)
        spans.push_back ({ (int) ((t0 + 0.1) * kRate), (int) ((t0 + kSung - 0.03) * kRate) });

    return spans;
}

/** How far the ring comes down, in dB, from 100 ms after each note starts. Positive is quieter. */
static double ringCut (const std::vector<float>& dry, const std::vector<float>& out, double seconds, double f0)
{
    const double hz = ringHzFor (f0);
    return bandDb (dry, 0, ringSpans (seconds), hz - 60.0, hz + 60.0) - bandDb (out, kLatency, ringSpans (seconds), hz - 60.0, hz + 60.0);
}

static double largestStep (const std::vector<float>& x, int from)
{
    double worst = 0.0;

    for (size_t i = (size_t) from + 1; i < x.size(); ++i)
        worst = std::max (worst, (double) std::abs (x[i] - x[i - 1]));

    return worst;
}

// ---- the tests ----------------------------------------------------------------------------------------------------

static void testControl()
{
    std::puts ("\n== The control: a real parameter, off by default");
    Proc p;
    auto* c = control (p);
    check (c != nullptr, "a parameter with the ID heldNotes exists");
    check (c != nullptr && c->getName (64) == "Held Notes", "it is named Held Notes");
    check (c != nullptr && c->isAutomatable() && c->getNumSteps() == 2, "it is an on or off control that can be automated");
    check (c != nullptr && c->getDefaultValue() < 0.5f && ! isHeld (p), "it is off by default");
    check (c != nullptr && c->getAllValueStrings().size() == 2 && ! hasEmDash (c->getName (64)), "its two values have names and nothing has an em dash");

    if (c != nullptr)
        check (p.parameters.getParameter ("mode")->getNumSteps() == 2, "Mode still has two choices, Soft and Hard", p.parameters.getParameter ("mode")->getNumSteps());

    Proc on;
    setHeld (on, true);
    prepare (p);
    prepare (on);
    check (p.getLatencySamples() == kLatency && on.getLatencySamples() == kLatency, "latency is 2048 with it off and with it on", on.getLatencySamples(), "samples");
}

static void testOffIsTheOldSound()
{
    std::puts ("\n== Off is the 0.5.1 sound: against the frozen 0.4.1 engine, which 0.5.1 nulls against");
    const auto x = ringingVoice (6.0, 180.0);

    struct Case { const char* name; std::function<void (Proc&)> setUp; };
    const Case cases[] = {
        { "default settings", [] (Proc&) {} },
        { "Depth 12, Selectivity 30, 600 Hz to 10 kHz", [] (Proc& p) { setParam (p, "depth", 12.0f); setParam (p, "selectivity", 30.0f); setParam (p, "lowFreq", 600.0f); setParam (p, "highFreq", 10000.0f); } },
        { "Hard mode at -50 dB", [] (Proc& p) { setParam (p, "mode", 1.0f); setParam (p, "hardThreshold", -50.0f); } },
    };

    for (const auto& c : cases)
    {
        Proc untouched;
        c.setUp (untouched);
        prepare (untouched);
        const auto old = renderOldEngine (untouched, x);
        const double a = nullDb (render (untouched, x), old);
        check (a <= -90.0, std::string (c.name) + ", control never touched: against the old engine (limit -90)", a, "dBFS");

        Proc off;
        c.setUp (off);
        setHeld (off, true);
        setHeld (off, false);
        prepare (off);
        const double b = nullDb (render (off, x), old);
        check (b <= -90.0, std::string (c.name) + ", turned on and off again before playing: against the old engine (limit -90)", b, "dBFS");
    }

    // Turned off while playing, it is back on the old sound once the reduction it made has let go.
    {
        Proc p;
        prepare (p);
        const bool has = setHeld (p, true);
        const auto old = renderOldEngine (p, x);
        const auto out = render (p, x, kBlock, [&p] (int pos) { if (pos >= 96000 && pos < 96000 + kBlock) setHeld (p, false); });
        const std::vector<float> tailOut (out.begin() + 192000, out.end()), tailOld (old.begin() + 192000, old.end());
        const double db = nullDb (tailOut, tailOld);
        check (has && db <= -60.0, "turned off at 2 s while playing: from 4 s on it is the old sound again (limit -60)", db, "dBFS");
    }
}

static void testOnCutsARing()
{
    std::puts ("\n== On takes down a ring that lasts: one harmonic 12 dB over its neighbours on 0.6 s notes, a taller formant either side, default settings");
    constexpr double seconds = 7.5;

    for (double f0 : { 130.0, 180.0, 250.0 })
    {
        const auto x = ringingVoice (seconds, f0);
        const auto clean = ringingVoice (seconds, f0, false);
        const auto off = renderWith (x, false), on = renderWith (x, true);
        const double cutOff = ringCut (x, off, seconds, f0), cutOn = ringCut (x, on, seconds, f0);
        const double hz = ringHzFor (f0);

        char line[200];
        std::snprintf (line, sizeof (line), "F0 %.0f Hz, ring at %.0f Hz", f0, hz);
        info (std::string (line) + ": the ring stands over the two harmonics either side by", ringStands (x, seconds, f0), "dB");
        info (std::string (line) + ": with it off the ring comes down", cutOff, "dB");
        check (cutOn >= 3.0, std::string (line) + ": with it on, from 100 ms into each note (limit 3 or more)", cutOn, "dB");

        // Away from the ring, on and off are the same sound.
        const auto spans = ringSpans (seconds);
        const double below = bandDb (on, kLatency, spans, 300.0, hz - 400.0) - bandDb (off, kLatency, spans, 300.0, hz - 400.0);
        const double above = bandDb (on, kLatency, spans, hz + 400.0, 12000.0) - bandDb (off, kLatency, spans, hz + 400.0, 12000.0);
        check (std::abs (below) <= 0.5 && std::abs (above) <= 0.5, std::string (line) + ": on against off, more than 400 Hz below it (limit 0.5)", below, "dB");
        info (std::string (line) + ": on against off, more than 400 Hz above it", above, "dB");

        // The same notes with no ring: on changes nothing where the ring would be.
        const auto cleanOff = renderWith (clean, false), cleanOn = renderWith (clean, true);
        const double noRing = bandDb (cleanOn, kLatency, spans, hz - 60.0, hz + 60.0) - bandDb (cleanOff, kLatency, spans, hz - 60.0, hz + 60.0);
        check (std::abs (noRing) <= 1.0, std::string (line) + ": the same notes with no ring, on against off at that place (limit 1)", noRing, "dB");
    }

    // Depth is still the most any spot is turned down, and Hard mode is not touched.
    {
        const auto x = ringingVoice (seconds, 180.0);
        const double atDefault = ringCut (x, renderWith (x, true), seconds, 180.0);
        const double shallow = ringCut (x, renderWith (x, true, [] (Proc& p) { setParam (p, "depth", 3.0f); }), seconds, 180.0);
        const double deep = ringCut (x, renderWith (x, true, [] (Proc& p) { setParam (p, "depth", 12.0f); }), seconds, 180.0);
        check (shallow >= 1.5 && shallow <= 3.2, "Depth 3 with it on: the ring comes down, and no more than Depth (1.5 to 3.2)", shallow, "dB");
        check (deep >= atDefault - 0.1 && deep <= 12.5, "Depth 12 with it on: no less than at Depth 6, and no more than Depth", deep, "dB");
        info ("the same ring at Depth 6", atDefault, "dB");

        const auto hard = [] (Proc& p) { setParam (p, "mode", 1.0f); };
        Proc probe;
        const bool has = control (probe) != nullptr;
        const double same = nullDb (renderWith (x, true, hard), renderWith (x, false, hard));
        check (has && same <= -120.0, "in Hard mode it does nothing: on against off (limit -120)", same, "dBFS");
    }
}

static void testConsonants()
{
    std::puts ("\n== On leaves S and T alone: an S straight after every note and a T just before the next");
    constexpr double seconds = 7.5;
    const auto s = voiceWithConsonants (seconds, 180.0);
    const auto off = renderWith (s.x, false), on = renderWith (s.x, true);
    std::vector<std::pair<int, int>> sSpans, tSpans;

    for (size_t i = 2; i < s.sStarts.size(); ++i)
    {
        sSpans.push_back ({ s.sStarts[i] + 1024, s.sStarts[i] + s.sLength - 1024 });
        tSpans.push_back ({ s.tStarts[i] + 512, s.tStarts[i] + 1440 });
    }

    const double sDry = bandDb (s.x, 0, sSpans, 4000.0, 20000.0), tDry = bandDb (s.x, 0, tSpans, 2500.0, 20000.0);
    const double sOff = bandDb (off, kLatency, sSpans, 4000.0, 20000.0) - sDry, sOn = bandDb (on, kLatency, sSpans, 4000.0, 20000.0) - sDry;
    const double tOff = bandDb (off, kLatency, tSpans, 2500.0, 20000.0) - tDry, tOn = bandDb (on, kLatency, tSpans, 2500.0, 20000.0) - tDry;

    Proc probe;
    check (control (probe) != nullptr && ringCut (s.x, on, seconds, 180.0) >= 3.0, "the ring in the same signal still comes down with it on (limit 3 or more)", ringCut (s.x, on, seconds, 180.0), "dB");
    info ("S above 4 kHz with it off", sOff, "dB");
    check (sOn >= -1.5 && sOn >= sOff - 0.3, "S above 4 kHz with it on (no more than 1.5 down, and no more than 0.3 below off)", sOn, "dB");
    info ("T above 2.5 kHz with it off", tOff, "dB");
    check (tOn >= -1.5 && tOn >= tOff - 0.3, "T above 2.5 kHz with it on (no more than 1.5 down, and no more than 0.3 below off)", tOn, "dB");

    // Noise alone has no harmonics, so on is off to the sample.
    {
        std::vector<float> hiss ((size_t) (3.0 * kRate));
        std::mt19937 rng (5);
        std::normal_distribution<float> noise (0.0f, 0.1f);
        for (auto& v : hiss) v = noise (rng);
        const double db = nullDb (renderWith (hiss, true), renderWith (hiss, false));
        check (db <= -120.0, "white noise: on against off (limit -120)", db, "dBFS");
    }
}

static void testState()
{
    std::puts ("\n== Saved sessions");

    // What 0.5.1 saved: today's state with no Held Notes entry in it.
    {
        Proc saver;
        setParam (saver, "depth", 9.0f);
        juce::MemoryBlock saved;
        saver.getStateInformation (saved);
        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        int removed = 0;

        for (auto* child = xml->getFirstChildElement(); child != nullptr;)
        {
            auto* next = child->getNextElement();

            if (child->getStringAttribute ("id") == kId)
            {
                xml->removeChildElement (child, true);
                ++removed;
            }

            child = next;
        }

        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);

        Proc p;
        const bool has = setHeld (p, true);
        p.setStateInformation (old.getData(), (int) old.getSize());
        check (has && removed == 1 && ! isHeld (p) && std::abs (real (p, "depth") - 9.0f) < 0.01f, "a session saved by 0.5.1 loads with it off, even into a plug-in that had it on");
    }

    // The session 0.4.1 really saved, kept in tests/baseline.
    {
        juce::MemoryBlock state;
        const bool found = juce::File (BASELINE_DIR).getChildFile ("default.state").loadFileAsData (state);
        Proc p;
        const bool has = setHeld (p, true);
        p.setStateInformation (state.getData(), (int) state.getSize());
        check (found && has && ! isHeld (p), "the default session saved by 0.4.1 loads with it off");
    }

    // On is saved, and off is saved.
    {
        Proc a;
        const bool has = setHeld (a, true);
        juce::MemoryBlock saved;
        a.getStateInformation (saved);
        Proc b;
        b.setStateInformation (saved.getData(), (int) saved.getSize());
        check (has && isHeld (b), "a session saved with it on reloads with it on");

        setHeld (a, false);
        juce::MemoryBlock savedOff;
        a.getStateInformation (savedOff);
        Proc c;
        setHeld (c, true);
        c.setStateInformation (savedOff.getData(), (int) savedOff.getSize());
        check (has && ! isHeld (c), "a session saved with it off reloads with it off");
    }
}

static void testAutomation()
{
    std::puts ("\n== Switching it while the voice rings does not click");
    const auto x = ringingVoice (6.0, 180.0);
    const auto off = renderWith (x, false), on = renderWith (x, true);

    Proc p;
    prepare (p, 64);
    int flips = 0;
    bool state = false, has = true;
    const auto toggled = render (p, x, 64, [&] (int pos)
    {
        if (pos % 1792 == 0)   // every 37 ms, on any part of a note
        {
            state = ! state;
            has = setHeld (p, state) && has;
            ++flips;
        }
    });

    const double steady = std::max (largestStep (off, 48000), largestStep (on, 48000)), moving = largestStep (toggled, 48000);
    check (has && flips > 100 && moving <= 1.1 * steady, "switched every 37 ms: the largest step between samples against the steady renders (limit 1.1 times)", moving / steady, "times");

    // What switching adds stays small next to the voice: no more than the difference between on and off themselves.
    double worstOnOff = 0.0, worstToggle = 0.0;

    for (size_t i = 48000; i < x.size(); ++i)
    {
        worstOnOff = std::max (worstOnOff, (double) std::abs (on[i] - off[i]));
        worstToggle = std::max (worstToggle, std::min ((double) std::abs (toggled[i] - off[i]), (double) std::abs (toggled[i] - on[i])));
    }

    check (has && worstToggle <= worstOnOff, "switched every 37 ms: every sample lies no further from on or off than they lie from each other", toDb (worstToggle) - toDb (worstOnOff), "dB");
}

static void testPresets()
{
    std::puts ("\n== Presets: the old ones keep their places and leave it off");
    const char* const old[] = { "Gentle Vocal Polish", "Harsh Lead Vocal", "Vocal Stack Smoother", "Wide Stack Mid Side", "Sibilance Soother", "Boxy Low Mids",
                                "Hard Whistle Catcher", "Side Chain Carve", "Tracking Low Latency", "Eight Dot Vocal Sculpt", "High Detail With Ceiling" };
    Proc p;
    bool sameNames = p.getNumPrograms() >= 11, allOff = control (p) != nullptr;

    for (int i = 0; i < 11 && sameNames; ++i)
        sameNames = p.getProgramName (i) == old[i];

    check (sameNames, "presets 0 to 10 have the names and the order they had in 0.5.1");

    for (int i = 0; i < 11 && i < p.getNumPrograms(); ++i)
    {
        setHeld (p, true);
        p.setCurrentProgram (i);
        allOff = allOff && ! isHeld (p);
    }

    check (allOff, "each of them turns Held Notes off, as a preset sets every other control");

    const int last = p.getNumPrograms() - 1;
    check (p.getNumPrograms() == 12 && p.getProgramName (last) == "Gentle Polish With Held Notes", "one new preset, at the end: Gentle Polish With Held Notes", p.getNumPrograms(), "presets");

    Proc fresh, loaded;
    loaded.setCurrentProgram (last);
    bool sameAsDefault = isHeld (loaded);

    for (auto* prm : loaded.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (ranged->getParameterID() != kId && ! ranged->getParameterID().startsWith ("maxReduction"))
                sameAsDefault = sameAsDefault && std::abs (ranged->getValue() - fresh.parameters.getParameter (ranged->getParameterID())->getValue()) < 1.0e-6f;

    check (sameAsDefault, "it turns Held Notes on and leaves every other control at its default");
}

static void testWindow()
{
    std::puts ("\n== The window");
    Proc p;
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    auto* window = dynamic_cast<ResonanceEditor*> (editor.get());
    auto& panel = window->getPanel();
    window->tick (1.0 / 30.0);
    auto* c = panel.findChildWithID (kId);
    auto* button = dynamic_cast<juce::Button*> (c);

    check (c != nullptr && c->isVisible() && ! window->isAdvancedOpen(), "Held Notes has a control that shows with Advanced closed");
    check (button != nullptr && panel.getLocalBounds().contains (c->getBounds()) && c->getBottom() <= ResonanceEditor::designHeight - 60, "it sits inside the side panel, above the hint line");

    if (button != nullptr)
    {
        const float textWidth = juce::GlyphArrangement::getStringWidth (ui::font (ui::Weight::medium, 13.0f), button->getButtonText());
        check (textWidth + 12.0f <= (float) c->getWidth(), "its label fits its button with room to spare: " + button->getButtonText().toStdString(), textWidth, "of the button's width in points");

        auto* mode = panel.findChildWithID ("mode");
        const float widest = std::max (juce::GlyphArrangement::getStringWidth (ui::font (ui::Weight::medium, 12.0f), "Soft"),
                                       juce::GlyphArrangement::getStringWidth (ui::font (ui::Weight::medium, 12.0f), "Hard"));
        check (mode->getBottom() <= c->getY() || mode->getRight() <= c->getX(), "it does not overlap Mode");
        check (((float) mode->getWidth() - 84.0f) / 2.0f >= widest + 12.0f, "Soft and Hard still fit their buttons", ((float) mode->getWidth() - 84.0f) / 2.0f, "points each");

        button->setToggleState (true, juce::sendNotificationSync);
        check (isHeld (p), "switching it in the window turns the parameter on");
        setHeld (p, false);
        check (! button->getToggleState(), "automation turning it off shows in the window");

        setParam (p, "mode", 1.0f);
        window->tick (1.0 / 30.0);
        check (! c->isEnabled(), "in Hard mode, where it does nothing, it is greyed out");
        setParam (p, "mode", 0.0f);
        window->tick (1.0 / 30.0);
        check (c->isEnabled(), "and back in Soft mode it is live again");
    }

    if (auto* hinted = dynamic_cast<ui::Hinted*> (c))
        check (hinted->hint.isNotEmpty() && ! hasEmDash (hinted->hint), "it has a one line hint with no em dash: " + hinted->hint.toStdString());
    else
        check (false, "it has a one line hint");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::puts ("Held Notes, 48 kHz stereo");
    testControl();
    testOffIsTheOldSound();
    testOnCutsARing();
    testConsonants();
    testState();
    testAutomation();
    testPresets();
    testWindow();
    return finish();
}
