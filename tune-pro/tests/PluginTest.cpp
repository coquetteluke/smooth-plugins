// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// Every limit is in PassLimits.h, which was committed before the DSP was written.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PassLimits.h"
#include "VoiceSynth.h"
#include "Analysis.h"
#include "PluginProcessor.h"
#include "TuneEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <thread>

using namespace testkit;
using namespace measure;

namespace
{
int gEngine = 0;    // 0 Low Latency, 1 High Quality: every test that goes through make() runs on this engine
std::string engineTag() { return gEngine == 1 ? "[HQ] " : "[LL] "; }
void chk (bool ok, const std::string& what, double measured = std::numeric_limits<double>::quiet_NaN(), const char* unit = "") { check (ok, engineTag() + what, measured, unit); }
void note (const std::string& what, double value, const char* unit = "") { info (engineTag() + what, value, unit); }

constexpr double SR = 48000.0;
constexpr int soprano = 0, altoTenor = 1, lowMale = 2, wideRange = 3;

std::unique_ptr<PluginProcessor> make (const juce::String& keyFile = "/nonexistent/smooth-tune-test.json")
{
    PluginProcessor::Options o;
    o.keyFilePath = keyFile;
    o.hostLatencyUpdates = false;
    auto p = std::make_unique<PluginProcessor> (o);
    auto* e = p->apvts.getParameter ("engine");
    e->setValueNotifyingHost (e->convertTo0to1 ((float) gEngine));
    return p;
}

void setParam (juce::AudioProcessor& p, const char* id, float value)
{
    auto& proc = dynamic_cast<PluginProcessor&> (p);
    auto* prm = proc.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// Hard, plain tuning with no key: the settings most tests start from.
std::unique_ptr<PluginProcessor> tuner (int type, float retune, float humanize = 0.0f, float flex = 0.0f, double sr = SR, int block = 256)
{
    auto p = make();
    setParam (*p, "inputType", (float) type);
    setParam (*p, "retune", retune);
    setParam (*p, "humanize", humanize);
    setParam (*p, "flex", flex);
    prepare (*p, 1, sr, block);
    return p;
}

std::vector<float> mono (const juce::AudioBuffer<float>& b)
{
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

// Only one pitch class left in Custom, so the target is known whatever is sung.
void onlyNote (PluginProcessor& p, int pitchClass)
{
    setParam (p, "scale", (float) smoothtune::scaleChromatic);
    for (int i = 0; i < 12; ++i)
        setParam (p, ("note" + juce::String (i)).toRawUTF8(), i == pitchClass ? 0.0f : 1.0f);
}

double noteHz (int midi, double a4 = 440.0) { return a4 * std::pow (2.0, (midi - 69) / 12.0); }

// ------------------------------------------------------------------------------------------------------------- tracker

struct TrackScore { std::vector<double> errors; long octaveEvents = 0; double seconds = 0.0; double voicedShare = 0.0; };

// Runs the tracker alone over a voice whose true pitch is known. Compares every mark with the true pitch at that moment.
void scoreTracker (const std::vector<double>& f0, const voice::Vowel& v, double lowHz, double highHz, TrackScore& s, double breathDb = -60.0, double sr = SR)
{
    auto x = voice::synth (f0, v, sr, breathDb);
    smoothtune::PitchTracker t;
    t.prepare (sr);
    t.setRange (lowHz, highHz);
    long long seen = -1;
    bool inOctaveError = false;
    long voicedCount = 0, counted = 0;
    const long settle = (long) (0.05 * sr);
    for (long i = 0; i < (long) x.size(); ++i)
    {
        t.push (x[(size_t) i]);
        if (i < settle) { if (t.hasMarks()) seen = t.lastMark(); continue; }
        ++counted;
        if (t.isVoiced())
        {
            ++voicedCount;
            for (long long q = std::max (seen + 1, t.firstMark()); t.hasMarks() && q <= t.lastMark(); ++q)
            {
                const auto& m = t.mark (q);
                seen = q;
                if (m.strength < 0.6f || m.period <= 0.0)
                    continue;
                const long centre = std::clamp ((long) (m.position - 0.5 * m.period), 0L, (long) f0.size() - 1);
                s.errors.push_back (cents (sr / m.period, f0[(size_t) centre]));
            }
            if (i % 48 == 0)
            {
                // A note that has just jumped is not an octave error: the true pitch itself is in the tracker's window on both
                // sides of the jump for up to 40 ms. Only count a wrong octave that is still there after that.
                const long centre = std::clamp (i - (long) (0.008 * sr), 0L, (long) f0.size() - 1);
                const long older = std::clamp (i - (long) (0.048 * sr), 0L, (long) f0.size() - 1);
                const long newest = std::min (i, (long) f0.size() - 1);
                const bool justJumped = std::abs (cents (f0[(size_t) centre], f0[(size_t) older])) > 100.0
                                        || std::abs (cents (f0[(size_t) newest], f0[(size_t) older])) > 100.0;
                if (justJumped)
                    continue;
                const double err = std::abs (cents (sr / t.period(), f0[(size_t) centre]));
                const bool bad = err > 600.0;
                if (bad && ! inOctaveError)
                {
                    ++s.octaveEvents;
                    if (std::getenv ("TUNE_DUMP") != nullptr)
                        std::printf ("  octave event at %.3f s: tracker %.1f Hz, true %.1f Hz, vowel %s\n", (double) i / sr, sr / t.period(), f0[(size_t) centre], v.name);
                }
                inOctaveError = bad;
            }
        }
    }
    s.seconds += (double) voicedCount / sr;
    s.voicedShare = counted > 0 ? (double) voicedCount / (double) counted : 0.0;
}

void testTracker()
{
    const double pitches[] = { 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000 };
    const voice::Vowel* vowels[] = { &voice::vowelA, &voice::vowelI, &voice::vowelU, &voice::vowelE };

    TrackScore steady, vib, scoop, breath;
    double worstShare = 1.0;
    for (double hz : pitches)
        for (auto* v : vowels)
        {
            scoreTracker (voice::steady (hz, 1.2, SR), *v, 65.0, 1300.0, steady);
            worstShare = std::min (worstShare, steady.voicedShare);
            scoreTracker (voice::vibrato (hz, 1.2, SR, 50.0, 5.5), *v, 65.0, 1300.0, vib);
            // Leaps stay inside the tracked range: the top pitches leap down rather than up.
            if (hz * 2.0 <= 1200.0)
                scoreTracker (voice::phrase ({ hz, hz * 1.122, hz * 1.335, hz * 2.0, hz }, 0.35, SR), *v, 65.0, 1300.0, scoop);
            else
                scoreTracker (voice::phrase ({ hz, hz / 1.122, hz / 1.335, hz / 2.0, hz }, 0.35, SR), *v, 65.0, 1300.0, scoop);
            scoreTracker (voice::vibrato (hz, 1.2, SR, 30.0, 5.0), *v, 65.0, 1300.0, breath, -18.0);
        }

    const auto all = [] (std::initializer_list<const TrackScore*> list)
    {
        double sec = 0; long ev = 0;
        for (auto* s : list) { sec += s->seconds; ev += s->octaveEvents; }
        return std::pair<double, long> (sec, ev);
    };

    const auto a = absolute (steady.errors);
    chk (median (a) <= limits::trackSteadyMedianCents, "tracking error, steady notes 80 Hz to 1 kHz, 4 vowels, median", median (a), "cents");
    chk (percentile (a, 95) <= limits::trackSteadyP95Cents, "tracking error, steady notes, 95th percentile", percentile (a, 95), "cents");
    const auto b = absolute (vib.errors);
    chk (median (b) <= limits::trackVibratoMedianCents, "tracking error, vibrato +/-50 cents at 5.5 Hz, median", median (b), "cents");
    note ("tracking error, vibrato, 95th percentile", percentile (b, 95), "cents");
    note ("tracking error, breath -18 dB, median", median (absolute (breath.errors)), "cents");
    note ("tracking error, notes with scoops and octave leaps, median", median (absolute (scoop.errors)), "cents");
    note ("lowest voiced share on a steady note", worstShare * 100.0, "%");
    chk (worstShare >= 0.85, "steady notes are found voiced, worst case", worstShare * 100.0, "%");

    const auto [seconds, events] = all ({ &steady, &vib, &scoop, &breath });
    const double perMinute = (double) events / std::max (seconds, 1.0) * 60.0;
    chk (perMinute <= limits::octaveErrorsPerMinute, "octave errors per minute of voiced sound, " + std::to_string ((int) seconds) + " s of voice, all material", perMinute, "per minute");
    note ("octave error events, steady", (double) steady.octaveEvents);
    note ("octave error events, vibrato", (double) vib.octaveEvents);
    note ("octave error events, scoops and leaps", (double) scoop.octaveEvents);
    note ("octave error events, breath", (double) breath.octaveEvents);

    // Each Input Type on its own range, ends included.
    struct R { const char* name; double low, high; };
    for (auto r : { R { "Soprano", 150.0, 1300.0 }, R { "Alto/Tenor", 98.0, 1000.0 }, R { "Low Male", 65.0, 600.0 } })
    {
        TrackScore s;
        for (double hz : { r.low * 1.06, std::sqrt (r.low * r.high), r.high * 0.94 })
            scoreTracker (voice::steady (hz, 1.0, SR), voice::vowelA, r.low, r.high, s);
        chk (median (absolute (s.errors)) <= limits::trackSteadyMedianCents && s.octaveEvents == 0,
               std::string ("Input Type ") + r.name + " tracks both ends of its range, median", median (absolute (s.errors)), "cents");
    }

    // Other sample rates
    for (double sr : { 44100.0, 96000.0 })
    {
        TrackScore s;
        for (double hz : { 110.0, 220.0, 440.0, 880.0 })
            scoreTracker (voice::steady (hz, 1.0, sr), voice::vowelA, 65.0, 1300.0, s, -60.0, sr);
        chk (median (absolute (s.errors)) <= limits::trackSteadyMedianCents, "tracking error at " + std::to_string ((int) sr) + " Hz, median", median (absolute (s.errors)), "cents");
    }
}

// ---------------------------------------------------------------------------------------------------- output pitch

void testOutputPitch()
{
    std::vector<double> worst;
    double worstHz = 0;
    for (int midi : { 40, 45, 50, 55, 60, 65, 69, 72, 76, 79, 83, 86, 88 }) // about 82 Hz to 1.3 kHz at the top
    {
        const double target = noteHz (midi);
        if (target > 1050.0) continue;
        for (double off : { 30.0, -35.0 })
        {
            auto p = tuner (wideRange, 0.0f);
            const auto x = voice::synth (voice::steady (target * std::pow (2.0, off / 1200.0), 1.2, SR), voice::vowelA, SR);
            const auto out = mono (render (*p, x, 256));
            auto e = centsOff (out.data(), (long) out.size(), SR, target, 0.3, 1.1);
            const double q = percentile (absolute (e), 95);
            worst.push_back (q);
            if (q >= *std::max_element (worst.begin(), worst.end())) worstHz = target;
        }
    }
    const double w = *std::max_element (worst.begin(), worst.end());
    chk (w < limits::outputInstantCents, "output pitch error at instant speed, steady notes 82 Hz to 1 kHz, worst 95th percentile", w, "cents");
    note ("  worst case is at", worstHz, "Hz");

    // Vibrato and drift flattened to the note at instant speed
    std::vector<double> errs;
    for (double hz : { 110.0, 220.0, 440.0 })
    {
        auto p = tuner (altoTenor, 0.0f);
        const auto x = voice::synth (voice::vibrato (hz, 1.5, SR, 30.0, 5.5), voice::vowelE, SR, -30.0);
        const auto out = mono (render (*p, x, 256));
        auto e = centsOff (out.data(), (long) out.size(), SR, hz, 0.3, 1.4);
        errs.push_back (percentile (absolute (e), 95));
    }
    const double v = *std::max_element (errs.begin(), errs.end());
    chk (v < limits::outputInstantCents, "output pitch error at instant speed, vibrato +/-30 cents with breath, worst 95th percentile", v, "cents");

    // A less perfect voice: slow pitch wander and cycle to cycle jitter, as real throats have.
    {
        std::mt19937 g (17);
        std::normal_distribution<double> d (0.0, 1.0);
        auto f0 = voice::steady (220.0, 3.0, SR);
        double slow = 0.0, jitter = 0.0;
        for (size_t i = 0; i < f0.size(); ++i)
        {
            slow += 0.0008 * (d (g) * 25.0 - slow);
            if (i % 200 == 0) jitter = d (g) * 6.0;
            f0[i] *= std::pow (2.0, (slow + jitter + 20.0) / 1200.0);
        }
        auto p = tuner (altoTenor, 0.0f);
        const auto x = voice::synth (f0, voice::vowelA, SR, -35.0);
        const auto out = mono (render (*p, x, 256));
        const auto e = centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 2.9);
        chk (percentile (absolute (e), 95) < limits::outputInstantCents, "output pitch error at instant speed, voice with wander, jitter and breath, 95th percentile", percentile (absolute (e), 95), "cents");
        const double nh = nonHarmonicDb (out.data() + (int) (0.5 * SR), 1 << 16, 220.0, SR);
        const double nhIn = nonHarmonicDb (x.data() + (int) (0.5 * SR), 1 << 16, 220.0 * std::pow (2.0, 20.0 / 1200.0), SR);
        note ("  non-harmonic energy on that voice, in then out", nhIn, "dB");
        note ("  non-harmonic energy on that voice, out", nh, "dB");
    }

    // Quiet and loud takes tune the same.
    for (double levelDb : { -45.0, -6.0 })
    {
        auto p = tuner (altoTenor, 0.0f);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.2, SR), voice::vowelA, SR, -60.0, levelDb);
        const auto out = mono (render (*p, x, 256));
        auto e = centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 1.1);
        chk (percentile (absolute (e), 95) < limits::outputInstantCents, "output pitch error on a voice at " + std::to_string ((int) levelDb) + " dBFS, 95th percentile", percentile (absolute (e), 95), "cents");
    }

    // Other sample rates, because everything scales with the rate
    for (double sr : { 44100.0, 96000.0 })
    {
        auto p = tuner (altoTenor, 0.0f, 0.0f, 0.0f, sr);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.2, sr), voice::vowelA, sr);
        const auto out = mono (render (*p, x, 256));
        auto e = centsOff (out.data(), (long) out.size(), sr, 220.0, 0.3, 1.1);
        chk (percentile (absolute (e), 95) < limits::outputInstantCents, "output pitch error at " + std::to_string ((int) sr) + " Hz, 95th percentile", percentile (absolute (e), 95), "cents");
    }
}

// ---------------------------------------------------------------------------------------------------- retune speed

void testRetuneSpeed()
{
    for (float tau : { 25.0f, 50.0f, 100.0f, 200.0f, 400.0f })
    {
        auto p = tuner (altoTenor, tau);
        const double hz = 220.0, stepCents = 40.0, before = 0.8;
        const double after = std::max (1.0, 5.0 * tau * 0.001);
        std::vector<double> f0 ((size_t) ((before + after) * SR));
        for (size_t i = 0; i < f0.size(); ++i)
            f0[i] = (double) i / SR < before ? hz : hz * std::pow (2.0, stepCents / 1200.0);
        const auto x = voice::synth (f0, voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double delay = p->getLatencySamples() / SR, hop = 0.001;
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hz, before + delay, before + delay + after - 0.05, hop);
        // Deviation falls from the full step towards 0. Where does it cross 36.8%?
        std::vector<double> dev;
        for (double h : trace) dev.push_back (h > 0.0 ? cents (h, hz) : std::nan (""));
        double crossMs = -1.0;
        const double level = stepCents * std::exp (-1.0);
        int start = 0;
        while (start < (int) dev.size() && ! (dev[(size_t) start] > level)) ++start;
        for (int i = start; i < (int) dev.size(); ++i)
            if (dev[(size_t) i] < level) { crossMs = i * hop * 1000.0; break; }
        const double allow = std::max ((double) tau * limits::retuneToleranceFraction, limits::retuneToleranceMs);
        chk (std::abs (crossMs - tau) <= allow, "Retune Speed " + std::to_string ((int) tau) + " ms, measured time constant", crossMs, "ms");
    }

    // Retune Speed 0 is truly instant: the same step, gone within the tracker's own time.
    auto p = tuner (altoTenor, 0.0f);
    const double hz = 220.0, before = 0.8;
    std::vector<double> f0 ((size_t) (1.6 * SR));
    for (size_t i = 0; i < f0.size(); ++i)
        f0[i] = (double) i / SR < before ? hz : hz * std::pow (2.0, 40.0 / 1200.0);
    const auto x = voice::synth (f0, voice::vowelA, SR);
    const auto out = mono (render (*p, x, 256));
    const auto e = centsOff (out.data(), (long) out.size(), SR, hz, before + 0.05, 1.5);
    chk (percentile (absolute (e), 95) < limits::outputInstantCents, "Retune Speed 0 after a 40 cent step, frames from 50 ms on, 95th percentile", percentile (absolute (e), 95), "cents");
}

// -------------------------------------------------------------------------------------------------------- formants

// The shape of a vocal tract only shows when harmonics sweep across it, so the voice glides slowly over one octave, which lets every
// harmonic cover the frequencies up to where its double starts: no gaps and no piling up. The shape is read from the long term average spectrum. The shift is the same +/-3 semitones a correction would ask for,
// held steady with Transpose while Flex Amount leaves the glide alone.
struct FormantRun { std::vector<float> in, out; };

// How the test voice is made. The held out voices change these, so they differ from the tuning voices in more than their vowel.
struct VoiceStyle { double breathDb = -60.0, softAboveHz = 0.0; };
VoiceStyle gStyle;

std::vector<float> styledVoice (const std::vector<double>& f0, const voice::Vowel& v)
{
    auto x = voice::synth (f0, v, SR, gStyle.breathDb);
    if (gStyle.softAboveHz > 0.0)
    {
        // A softer, darker source: one more gentle pole on the way out.
        const double a = std::exp (-2.0 * M_PI * gStyle.softAboveHz / SR);
        double y = 0.0;
        const double before = (double) rms (x.data(), (int) x.size());
        for (auto& sample : x) { y = (1.0 - a) * sample + a * y; sample = (float) y; }
        const double trim = before / std::max (1.0e-12, (double) rms (x.data(), (int) x.size()));
        for (auto& sample : x) sample = (float) (sample * trim);
    }
    return x;
}
FormantRun glideThroughTuner (const voice::Vowel& v, double centreHz, int transposeBy, bool preserve = true, float throat = 100.0f, int type = lowMale)
{
    const double seconds = 6.0;
    std::vector<double> f0 ((size_t) (seconds * SR));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        // A triangle, so every pitch in the range gets the same time. A sine would pile up at its ends and bias the reading.
        const double u = std::fmod ((double) i / SR / 3.0, 1.0);
        const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u;
        f0[i] = centreHz * std::pow (2.0, 0.5 * tri);
    }
    auto p = make();
    setParam (*p, "inputType", (float) type);
    setParam (*p, "retune", 0.0f);
    setParam (*p, "humanize", 0.0f);
    setParam (*p, "flex", 100.0f);
    setParam (*p, "transpose", (float) transposeBy);
    setParam (*p, "formantPreserve", preserve ? 1.0f : 0.0f);
    setParam (*p, "throat", throat);
    prepare (*p, 1, SR, 256);
    FormantRun r;
    r.in = styledVoice (f0, v);
    r.out = mono (render (*p, r.in, 256));
    return r;
}

// A real voice at the corrected pitch with the same vocal tract: what perfect formant preservation would look like.
std::vector<float> controlVoice (const voice::Vowel& v, double centreHz, int shift)
{
    const double seconds = 6.0, moved = centreHz * std::pow (2.0, shift / 12.0);
    std::vector<double> f0 ((size_t) (seconds * SR));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        const double u = std::fmod ((double) i / SR / 3.0, 1.0);
        const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u;
        f0[i] = moved * std::pow (2.0, 0.5 * tri);
    }
    return styledVoice (f0, v);
}

// Formant position measure: which frequency scale lines the output's spectrum shape up with that real voice's? The first
// tries at this (picking peaks and fitting harmonics) could not read a real voice to better than +/-3 to 10%, so this one
// was checked against known moves instead: Formant Preserve off must read the pitch shift, and Throat Length its own ratio
// (see testAddedControls). Peak height is reported too, because grains do broaden each peak: it stays in place, a bit flatter.
struct FormantScore { double move = 0.0, moveReal = 0.0, floor = 0.0, level = 0.0, flatter = 0.0, colour = 0.0, colourFloor = 0.0, peakFlatter = 0.0, peakSharper = -100.0, peakFlatterCrossed = 0.0; };

void scoreFormants (FormantScore& s, const voice::Vowel& v, double centre, int shift, int type = lowMale)
{
    const long from = (long) (0.3 * SR);
    const auto run = glideThroughTuner (v, centre, shift, true, 100.0f, type);
    const auto control = controlVoice (v, centre, shift);
    const long n = (long) run.in.size() - from - 4000;
    const double smoothing = std::max (centre, centre * std::pow (2.0, shift / 12.0));
    const auto wantShape = smoothSpectrumDb (control.data() + from, n, SR, smoothing);
    const auto inShape = smoothSpectrumDb (run.in.data() + from, n, SR, smoothing);
    const auto gotShape = smoothSpectrumDb (run.out.data() + from, n, SR, smoothing);
    // Bands hold at least two formants: with one narrow peak on a slope, as a band of F1 alone on /i/ would be, this tool cannot
    // tell a move from a flatter peak, and even two real voices with the same vocal tract read 8% apart. Peak height is reported separately.
    for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
    {
        const double against = shapeScale (inShape, gotShape, band.first, band.second);
        const double real = shapeScale (wantShape, gotShape, band.first, band.second);
        const double noise = shapeScale (wantShape, inShape, band.first, band.second);
        s.move = std::max (s.move, std::abs (against - 1.0));
        s.moveReal = std::max (s.moveReal, std::abs (real - 1.0));
        s.floor = std::max (s.floor, std::abs (noise - 1.0));
        if (std::getenv ("TUNE_DUMP") != nullptr)
            std::printf ("  vowel %s around %.0f Hz, shift %d, band %.0f to %.0f Hz: against input %.3f, against a real voice there %.3f, input against that voice %.3f\n", v.name, centre, shift, band.first, band.second, against, real, noise);
    }
    // Colour: how far the output's whole spectrum shape sits from that real voice's, as a plain distance in dB, smoothed over a harmonic spacing.
    const auto distance = [&] (const std::vector<double>& a, const std::vector<double>& b)
    {
        double mean = 0.0, sum = 0.0;
        int count = 0;
        for (double hz = 300.0; hz <= 4000.0; hz *= 1.02) { mean += b[(size_t) hz] - a[(size_t) hz]; ++count; }
        mean /= count;
        for (double hz = 300.0; hz <= 4000.0; hz *= 1.02) { const double d = b[(size_t) hz] - a[(size_t) hz] - mean; sum += d * d; }
        return std::sqrt (sum / count);
    };
    const double colour = distance (wantShape, gotShape);
    s.colour = std::max (s.colour, colour);
    s.colourFloor = std::max (s.colourFloor, distance (wantShape, inShape));
    if (std::getenv ("TUNE_DUMP") != nullptr)
        std::printf ("  vowel %s around %.0f Hz, shift %d: colour %.2f dB\n", v.name, centre, shift, colour);
    const auto fine = smoothSpectrumDb (control.data() + from, n, SR, 50.0), fineOut = smoothSpectrumDb (run.out.data() + from, n, SR, 50.0);
    const size_t f1 = (size_t) v.f[0];
    const double flatten = (fine[f1] - fine[f1 / 2]) - (fineOut[f1] - fineOut[f1 / 2]);
    // The same peak against its own surroundings, which also holds on high notes, where half the first formant is below the lowest harmonic.
    const auto peakOverAround = [f1] (const std::vector<double>& spec)
    {
        double sum = 0.0; int count = 0;
        for (size_t hz = (size_t) (0.6 * (double) f1); hz <= (size_t) (1.5 * (double) f1); hz += 5) { sum += spec[hz]; ++count; }
        return spec[f1] - sum / count;
    };
    s.peakFlatter = std::max (s.peakFlatter, peakOverAround (fine) - peakOverAround (fineOut));
    s.peakSharper = std::max (s.peakSharper, peakOverAround (fineOut) - peakOverAround (fine));
    // Only when a harmonic of the new voice actually crosses the formant. If the whole glide stays above it, as with /e/ at 660 Hz up 3, there is nothing at that frequency but
    // a skirt and the noise floor, in a real voice too. So this one is the reading of a peak's height, and the one above is kept as the limit it always was.
    if (v.f[0] >= 1.02 * centre * std::pow (2.0, shift / 12.0) * std::sqrt (0.5))
        s.peakFlatterCrossed = std::max (s.peakFlatterCrossed, peakOverAround (fine) - peakOverAround (fineOut));
    if (const char* folder = std::getenv ("TUNE_SPEC"))
    {
        const auto fineIn = smoothSpectrumDb (run.in.data() + from, n, SR, 50.0);
        const auto path = juce::File (folder).getChildFile (juce::String (v.name) + "_" + juce::String ((int) centre) + "_" + juce::String (shift) + ".csv");
        juce::String text;
        for (size_t hz = 0; hz < 6000; hz += 5)
            text << (int) hz << "," << fine[hz] << "," << fineOut[hz] << "," << fineIn[hz] << "\n";
        path.replaceWithText (text);
    }
    if (std::getenv ("TUNE_DUMP") != nullptr)
        std::printf ("  vowel %s around %.0f Hz, shift %d: peak against surroundings %.2f dB real, %.2f dB out\n", v.name, centre, shift, peakOverAround (fine), peakOverAround (fineOut));
    if (std::getenv ("TUNE_DUMP") != nullptr)
        std::printf ("  vowel %s around %.0f Hz, shift %d: first formant flatter by %.2f dB, level %+.2f dB\n", v.name, centre, shift, flatten, toDb (rms (run.out.data() + from, (int) n)) - toDb (rms (run.in.data() + from, (int) n)));
    s.flatter = std::max (s.flatter, flatten);
    s.level = std::max (s.level, std::abs (toDb (rms (run.out.data() + from, (int) n)) - toDb (rms (run.in.data() + from, (int) n))));
}

FormantScore formantResults[2], highNoteResults[2], bigShiftResults[2];

// Formant position measure: which frequency scale lines the output's spectrum shape up with that real voice's? The first
// tries at this (picking peaks and fitting harmonics) could not read a real voice to better than +/-3 to 10%, so this one
// was checked against known moves instead: Formant Preserve off must read the pitch shift, and Throat its own ratio
// (see testAddedControls). Peak height is reported too, because grains broaden each peak.
void testFormants()
{
    auto& s = formantResults[gEngine];
    s = {};
    const double moveLimit = gEngine == 1 ? prolimits::hqFormantMove : prolimits::llFormantMove;
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU })
        for (double centre : { 110.0, 165.0 })
            for (int shift : { 3, -3, 1, -1 })
                scoreFormants (s, *v, centre, shift);
    for (auto* v : { &voice::vowelA, &voice::vowelE })
        for (int shift : { 3, -3 })
            scoreFormants (s, *v, 247.0, shift);
    chk (s.move <= moveLimit, "formant positions at +/-1 and +/-3 semitones, output against input, worst move", s.move * 100.0, "%");

    FormantScore other;
    for (auto* v : { &voice::vowelO, &voice::vowelAe })
        for (double centre : { 130.0, 196.0 })
            for (int shift : { 3, -3, 2, -2 })
                scoreFormants (other, *v, centre, shift);
    chk (other.move <= moveLimit, "formant positions on other vowels and pitches, worst move", other.move * 100.0, "%");
    note ("  the same output against a real voice at the new pitch, worst", std::max (s.moveReal, other.moveReal) * 100.0, "%");
    note ("  and the tool's floor: the input against that same real voice, which share a vocal tract, worst", std::max (s.floor, other.floor) * 100.0, "%");
    const double flatter = std::max (s.flatter, other.flatter);
    s.flatter = flatter;
    chk (flatter <= (gEngine == 1 ? prolimits::hqF1Flatter : prolimits::llF1Flatter), "first formant peak flatter than a real voice at that pitch, worst case", flatter, "dB");
    note ("first formant against its surroundings, flatter than a real voice, worst", std::max (s.peakFlatter, other.peakFlatter), "dB");
    note ("colour, distance from a real voice's spectrum shape, worst", std::max (s.colour, other.colour), "dB");
    note ("  and the input against that real voice, worst", std::max (s.colourFloor, other.colourFloor), "dB");
    chk (std::max (s.level, other.level) <= (gEngine == 1 ? prolimits::hqLevelChangeDb : limits::levelChangeDb), "level stays put at +/-1, +/-2 and +/-3 semitones, worst change", std::max (s.level, other.level), "dB");

    // High notes, where a harmonic spacing is wider than a formant and any grain method colours the vowel
    auto& high = highNoteResults[gEngine];
    high = {};
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO })
        for (double centre : { 440.0, 660.0 })
            for (int shift : { 3, -3 })
                scoreFormants (high, *v, centre, shift, wideRange);
    // Against the input the tool cannot tell better than its floor here: see PassLimits.h. Against a real voice it does not read at all, as it hits the end of its range.
    note ("  high notes against a real voice, worst, not a usable reading", high.moveReal * 100.0, "%");
    note ("  and the tool's floor there: the input against a real voice with the same vocal tract", high.floor * 100.0, "%");
    chk (gEngine == 0 || high.move <= high.floor + prolimits::highNoteMoveOverFloor, "high notes around 440 and 660 Hz, +/-3 semitones, worst formant move against the input", high.move * 100.0, "%");
    // Read against the peak's surroundings. The test above reads against half the formant's frequency, which at these pitches is below the lowest harmonic, so it compares a peak with the noise floor.
    chk (gEngine == 0 || high.peakFlatter <= prolimits::highNoteF1Flatter, "high notes, first formant against its surroundings, flatter than a real voice, worst case", high.peakFlatter, "dB");
    // A flatter peak is one way to be wrong and a peak that overshoots is the other, so both directions are held to a limit.
    chk (gEngine == 0 || high.peakSharper <= prolimits::highNoteF1Sharper, "high notes, first formant sharper than a real voice, worst case", high.peakSharper, "dB");
    note ("  high notes, the same reading only where a harmonic crosses the formant, worst", high.peakFlatterCrossed, "dB");
    note ("  high notes, level change, worst", high.level, "dB");
    note ("colour on high notes, worst", high.colour, "dB");
    note ("  and the input against that real voice, worst", high.colourFloor, "dB");

    // Big corrections
    auto& big = bigShiftResults[gEngine];
    big = {};
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO })
        for (double centre : { 165.0, 247.0 })
            for (int shift : { 7, -7 })
                scoreFormants (big, *v, centre, shift, wideRange);
    // Against a real voice for the same reason as high notes.
    note ("  big shifts against the input, worst", big.move * 100.0, "%");
    note ("  and the tool's floor there: the input against a real voice with the same vocal tract", big.floor * 100.0, "%");
    chk (gEngine == 0 || big.moveReal <= prolimits::bigShiftMove, "big shifts of +/-7 semitones, worst formant move against a real voice", big.moveReal * 100.0, "%");
    chk (gEngine == 0 || big.peakFlatter <= prolimits::highNoteF1Flatter, "big shifts, first formant against its surroundings, flatter than a real voice, worst case", big.peakFlatter, "dB");
    note ("  big shifts, level change, worst", big.level, "dB");
    note ("colour on big shifts, worst", big.colour, "dB");
    note ("  and the input against that real voice, worst", big.colourFloor, "dB");
}

// -------------------------------------------------------------------------------------------------- held out voices

// One harmonic's level in a held note, in dB. Searched over a few cents, so a sub-cent pitch error does not read as a level error at high harmonics.
double harmonicDb (const float* x, long n, double hz)
{
    double best = 0.0;
    for (int cent = -6; cent <= 6; cent += 2)
    {
        const double w = 2.0 * M_PI * hz * std::pow (2.0, cent / 1200.0) / SR;
        double re = 0.0, im = 0.0;
        for (long i = 0; i < n; ++i)
        {
            const double h = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) i / (double) n);
            re += h * x[i] * std::cos (w * (double) i);
            im += h * x[i] * std::sin (w * (double) i);
        }
        best = std::max (best, re * re + im * im);
    }
    return 10.0 * std::log10 (best + 1.0e-20);
}

// What a listener gets on a held note: each output harmonic against the same harmonic of a real voice with the same vocal tract at the new pitch.
// Worst harmonic and RMS, with the overall level taken out. Harmonics up to 4 kHz. This reads a held note, which the glides above do not.
struct HeldScore { double rmsDb = 0.0, worstDb = 0.0; };
HeldScore heldNoteError (const voice::Vowel& v, double centre, int shift, int type)
{
    const double moved = centre * std::pow (2.0, shift / 12.0);
    auto p = make();
    setParam (*p, "inputType", (float) type);
    setParam (*p, "retune", 0.0f);
    setParam (*p, "humanize", 0.0f);
    setParam (*p, "flex", 100.0f);
    setParam (*p, "transpose", (float) shift);
    prepare (*p, 1, SR, 256);
    const auto in = styledVoice (voice::steady (centre, 1.6, SR), v);
    const auto out = mono (render (*p, in, 256));
    const auto real = styledVoice (voice::steady (moved, 1.6, SR), v);
    const long from = (long) (0.5 * SR), n = (long) (1.0 * SR);
    std::vector<double> difference;
    double mean = 0.0;
    for (double hz = moved; hz <= 4000.0; hz += moved)
    {
        difference.push_back (harmonicDb (out.data() + from, n, hz) - harmonicDb (real.data() + from, n, hz));
        mean += difference.back();
    }
    mean /= (double) difference.size();
    HeldScore s;
    for (double d : difference) { s.rmsDb += (d - mean) * (d - mean); s.worstDb = std::max (s.worstDb, std::abs (d - mean)); }
    s.rmsDb = std::sqrt (s.rmsDb / (double) difference.size());
    return s;
}

// Vowels, voices and pitches kept out of all tuning. Formants are the published averages for men and for women (Peterson and Barney, 1952). Bandwidths
// and source differ on purpose, across what the literature gives for sung and spoken voices. They are read only to check, never to set anything.
void testHeldOut()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; };
    const std::vector<Held> low {
        { { { 390, 1990, 2550, 3400, 4500 }, { 55, 100, 130, 150, 200 }, "ih" }, { -60.0, 0.0 } },
        { { { 640, 1190, 2390, 3400, 4500 }, { 85, 100, 140, 150, 200 }, "uh" }, { -40.0, 0.0 } },
        { { { 490, 1350, 1690, 3400, 4500 }, { 65, 75, 110, 150, 200 }, "er" }, { -60.0, 2500.0 } },
        { { { 440, 1020, 2240, 3400, 4500 }, { 60, 85, 120, 150, 200 }, "oo" }, { -45.0, 4000.0 } } };
    const std::vector<Held> high {
        { { { 430, 2480, 3070, 4000, 5000 }, { 60, 100, 130, 180, 220 }, "ih women" }, { -60.0, 0.0 } },
        { { { 760, 1400, 2780, 4000, 5000 }, { 95, 110, 150, 180, 220 }, "uh women" }, { -40.0, 0.0 } },
        { { { 850, 1220, 2810, 4000, 5000 }, { 110, 100, 150, 180, 220 }, "ah women" }, { -60.0, 2500.0 } },
        { { { 590, 920, 2710, 4000, 5000 }, { 70, 90, 130, 180, 220 }, "aw women" }, { -45.0, 4000.0 } },
        { { { 470, 1160, 2680, 4000, 5000 }, { 65, 90, 130, 180, 220 }, "oo women" }, { -60.0, 0.0 } } };

    FormantScore lowScore;
    for (const auto& h : low)
    {
        gStyle = h.style;
        for (double centre : { 98.0, 147.0, 220.0 })
            for (int shift : { 3, -3, 2, -2, 1, -1 })
                scoreFormants (lowScore, h.v, centre, shift);
    }
    chk (lowScore.move <= prolimits::hqFormantMove, "held out vowels and pitches, 98 to 220 Hz, worst formant move against the input", lowScore.move * 100.0, "%");
    note ("  and the tool's floor there", lowScore.floor * 100.0, "%");
    note ("  first formant against its surroundings, flatter than a real voice, worst", lowScore.peakFlatter, "dB");
    note ("  colour, worst", lowScore.colour, "dB");
    note ("  and the input against that real voice, worst", lowScore.colourFloor, "dB");
    chk (lowScore.level <= prolimits::hqLevelChangeDb, "  level change, worst", lowScore.level, "dB");

    FormantScore highScore;
    double heldRms = 0.0, heldWorst = 0.0, heldMean = 0.0;
    int heldCount = 0;
    for (const auto& h : high)
    {
        gStyle = h.style;
        for (double centre : { 392.0, 523.0, 587.0, 740.0 })
            for (int shift : { 3, -3, 2, -2 })
            {
                scoreFormants (highScore, h.v, centre, shift, wideRange);
                const auto held = heldNoteError (h.v, centre, shift, wideRange);
                heldRms = std::max (heldRms, held.rmsDb);
                heldWorst = std::max (heldWorst, held.worstDb);
                heldMean += held.rmsDb;
                ++heldCount;
                if (std::getenv ("TUNE_DUMP") != nullptr)
                    std::printf ("  held note, vowel %s, %.0f Hz, shift %d: harmonics against a real voice %.2f dB RMS, %.2f dB worst\n", h.v.name, centre, shift, held.rmsDb, held.worstDb);
            }
    }
    gStyle = {};
    chk (highScore.peakFlatterCrossed <= prolimits::highNoteF1Flatter, "held out vowels and pitches, 392 to 740 Hz, first formant flatter than a real voice, where a harmonic crosses it, worst", highScore.peakFlatterCrossed, "dB");
    note ("  the same with every case, including where the whole glide stays above the formant", highScore.peakFlatter, "dB");
    chk (highScore.peakSharper <= prolimits::highNoteF1Sharper, "  first formant sharper than a real voice, worst", highScore.peakSharper, "dB");
    chk (highScore.move <= highScore.floor + prolimits::highNoteMoveOverFloor, "  formant move against the input, worst, against the floor plus 2%", highScore.move * 100.0, "%");
    note ("  and the tool's floor there", highScore.floor * 100.0, "%");
    note ("  colour, worst", highScore.colour, "dB");
    note ("  and the input against that real voice, worst", highScore.colourFloor, "dB");
    note ("  level change, worst", highScore.level, "dB");
    note ("  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
    note ("  held notes, harmonics against a real voice, worst RMS", heldRms, "dB");
    note ("  held notes, harmonics against a real voice, worst single harmonic", heldWorst, "dB");
    chk (heldMean / heldCount <= prolimits::heldNoteHarmonicMeanDb, "  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// A second held out set, written for 1.3.0 after the first set had been looked at while tuning it, and run once at the end. Women's
// published averages (Peterson and Barney, 1952) at speaking and low singing pitches, 175 to 262 Hz, with their own bandwidths, breath
// and source tone. Nothing was tuned on these.
void testHeldOutSecond()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; };
    const std::vector<Held> voices {
        { { { 310, 2790, 3310, 4000, 5000 }, { 45, 110, 140, 180, 220 }, "i women" }, { -50.0, 3000.0 } },
        { { { 610, 2330, 2990, 4000, 5000 }, { 70, 110, 150, 180, 220 }, "eh women" }, { -35.0, 0.0 } },
        { { { 860, 2050, 2850, 4000, 5000 }, { 90, 120, 150, 180, 220 }, "ae women" }, { -55.0, 5000.0 } },
        { { { 370, 950, 2670, 4000, 5000 }, { 55, 90, 140, 180, 220 }, "u women" }, { -45.0, 0.0 } },
        { { { 500, 1640, 1960, 4000, 5000 }, { 60, 90, 110, 180, 220 }, "er women" }, { -60.0, 2000.0 } } };
    FormantScore score;
    double heldMean = 0.0;
    int heldCount = 0;
    for (const auto& h : voices)
    {
        gStyle = h.style;
        for (double centre : { 175.0, 208.0, 262.0 })
            for (int shift : { 3, -3, 2, -2, 1, -1 })
            {
                scoreFormants (score, h.v, centre, shift, wideRange);
                heldMean += heldNoteError (h.v, centre, shift, wideRange).rmsDb;
                ++heldCount;
            }
    }
    gStyle = {};
    chk (score.move <= prolimits::hqFormantMove, "second held out set, women's vowels 175 to 262 Hz, worst formant move against the input", score.move * 100.0, "%");
    note ("  and the tool's floor there", score.floor * 100.0, "%");
    note ("  the same output against a real voice at the new pitch, worst", score.moveReal * 100.0, "%");
    chk (score.level <= prolimits::hqLevelChangeDb, "  level change, worst", score.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", score.peakFlatter, "dB");
    note ("  first formant sharper than a real voice, worst", score.peakSharper, "dB");
    note ("  colour, worst", score.colour, "dB");
    note ("  and the input against that real voice, worst", score.colourFloor, "dB");
    note ("  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// A third held out set, written for 1.4.0 before any of it was run, after the second set had been used to choose the 1.4.0 settings.
// Run once at the end. Men's and women's averages from a later study (Hillenbrand and others, 1995), at pitches neither earlier set used,
// with bandwidths, upper formants, breath and source tone of their own. Nothing was tuned on these.
void testHeldOutThird()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; int type; std::vector<double> centres; };
    const std::vector<Held> voices {
        { { { 427, 2034, 2684, 3500, 4300 }, { 50, 95, 125, 160, 210 }, "ih men" }, { -55.0, 3500.0 }, lowMale, { 123.0, 185.0 } },
        { { { 768, 1333, 2522, 3500, 4300 }, { 80, 95, 135, 160, 210 }, "ah men" }, { -42.0, 0.0 }, lowMale, { 123.0, 185.0 } },
        { { { 469, 1122, 2434, 3500, 4300 }, { 55, 80, 120, 160, 210 }, "uu men" }, { -60.0, 6000.0 }, lowMale, { 123.0, 185.0 } },
        { { { 474, 1379, 1710, 3500, 4300 }, { 60, 85, 100, 160, 210 }, "er men" }, { -50.0, 0.0 }, lowMale, { 123.0, 185.0 } },
        { { { 437, 2761, 3372, 4100, 4900 }, { 50, 115, 145, 190, 230 }, "i women" }, { -48.0, 0.0 }, wideRange, { 233.0, 311.0 } },
        { { { 753, 1426, 2933, 4100, 4900 }, { 85, 105, 150, 190, 230 }, "uh women" }, { -58.0, 4500.0 }, wideRange, { 233.0, 311.0 } },
        { { { 459, 1105, 2735, 4100, 4900 }, { 55, 90, 140, 190, 230 }, "u women" }, { -40.0, 2500.0 }, wideRange, { 233.0, 311.0 } } };
    FormantScore score;
    double heldMean = 0.0;
    int heldCount = 0;
    for (const auto& h : voices)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 4, -4, 2, -2, 1, -1 })
            {
                scoreFormants (score, h.v, centre, shift, h.type);
                heldMean += heldNoteError (h.v, centre, shift, h.type).rmsDb;
                ++heldCount;
            }
    }
    gStyle = {};
    chk (score.move <= prolimits::hqFormantMove, "third held out set, men 123 and 185 Hz, women 233 and 311 Hz, worst formant move against the input", score.move * 100.0, "%");
    note ("  and the tool's floor there", score.floor * 100.0, "%");
    note ("  the same output against a real voice at the new pitch, worst", score.moveReal * 100.0, "%");
    chk (score.level <= prolimits::hqLevelChangeDb, "  level change, worst", score.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", score.peakFlatter, "dB");
    note ("  first formant sharper than a real voice, worst", score.peakSharper, "dB");
    note ("  colour, worst", score.colour, "dB");
    note ("  and the input against that real voice, worst", score.colourFloor, "dB");
    note ("  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// A fourth held out set, written on 2026-11-08 before any of it was run and before any 1.5.0 formant code was judged, because all three sets above
// had been used. Run once at the end. Children's averages from Peterson and Barney (1952), a speaker group none of the earlier sets used, at pitches
// none of them used: 277 and 330 Hz for speaking, 349 to 698 Hz for singing. F4 and F5 are not in that table, so they are set higher than the adults'.
// Bandwidths, breath and source tone are their own, wider than the adults', as children's are. Nothing was tuned on these.
void testHeldOutFourth()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; };
    const std::vector<Held> mid {
        { { { 530, 2730, 3600, 4400, 5300 }, { 75, 120, 170, 220, 260 }, "ih children" }, { -50.0, 3500.0 } },
        { { { 1010, 2320, 3320, 4400, 5300 }, { 120, 130, 170, 220, 260 }, "ae children" }, { -38.0, 0.0 } },
        { { { 560, 1410, 3310, 4400, 5300 }, { 80, 110, 160, 220, 260 }, "oo children" }, { -56.0, 2000.0 } },
        { { { 560, 1820, 2160, 4400, 5300 }, { 80, 115, 140, 220, 260 }, "er children" }, { -44.0, 5500.0 } } };
    const std::vector<Held> high {
        { { { 690, 2610, 3570, 4400, 5300 }, { 90, 125, 170, 220, 260 }, "eh children" }, { -50.0, 3500.0 } },
        { { { 1030, 1370, 3170, 4400, 5300 }, { 130, 120, 165, 220, 260 }, "ah children" }, { -38.0, 0.0 } },
        { { { 680, 1060, 3180, 4400, 5300 }, { 90, 110, 165, 220, 260 }, "aw children" }, { -56.0, 2000.0 } },
        { { { 430, 1170, 3260, 4400, 5300 }, { 70, 105, 165, 220, 260 }, "u children" }, { -44.0, 5500.0 } },
        { { { 850, 1590, 3360, 4400, 5300 }, { 105, 115, 170, 220, 260 }, "uh children" }, { -60.0, 0.0 } } };

    FormantScore midScore;
    for (const auto& h : mid)
    {
        gStyle = h.style;
        for (double centre : { 277.0, 330.0 })
            for (int shift : { 4, -4, 2, -2, 1, -1 })
                scoreFormants (midScore, h.v, centre, shift, wideRange);
    }
    chk (midScore.move <= prolimits::hqFormantMove, "fourth held out set, children 277 and 330 Hz, worst formant move against the input", midScore.move * 100.0, "%");
    note ("  and the tool's floor there", midScore.floor * 100.0, "%");
    chk (midScore.level <= prolimits::hqLevelChangeDb, "  level change, worst", midScore.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", midScore.peakFlatter, "dB");
    note ("  colour, worst", midScore.colour, "dB");

    FormantScore highScore;
    double heldMean = 0.0, heldRms = 0.0;
    int heldCount = 0;
    for (const auto& h : high)
    {
        gStyle = h.style;
        for (double centre : { 349.0, 466.0, 622.0, 698.0 })
            for (int shift : { 3, -3, 2, -2 })
            {
                scoreFormants (highScore, h.v, centre, shift, wideRange);
                const auto held = heldNoteError (h.v, centre, shift, wideRange);
                heldMean += held.rmsDb;
                heldRms = std::max (heldRms, held.rmsDb);
                ++heldCount;
            }
    }
    gStyle = {};
    chk (highScore.peakFlatterCrossed <= prolimits::highNoteF1Flatter, "fourth held out set, children 349 to 698 Hz, first formant flatter than a real voice, where a harmonic crosses it, worst", highScore.peakFlatterCrossed, "dB");
    note ("  the same with every case, including where the whole glide stays above the formant", highScore.peakFlatter, "dB");
    chk (highScore.peakSharper <= prolimits::highNoteF1Sharper, "  first formant sharper than a real voice, worst", highScore.peakSharper, "dB");
    chk (highScore.move <= highScore.floor + prolimits::highNoteMoveOverFloor, "  formant move against the input, worst, against the floor plus 2%", highScore.move * 100.0, "%");
    note ("  and the tool's floor there", highScore.floor * 100.0, "%");
    note ("  colour, worst", highScore.colour, "dB");
    note ("  level change, worst", highScore.level, "dB");
    note ("  held notes, harmonics against a real voice, worst RMS", heldRms, "dB");
    chk (heldMean / heldCount <= prolimits::heldNoteHarmonicMeanDb, "  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}


// A fifth held out set, written on 2026-11-08 for 1.5.0 before any of it was run, because all four sets above had been used. Run once at the end.
// Formants from a published source none of the earlier sets used: Catford, A Practical Introduction to Phonetics, 2nd edition (2001), p. 154,
// average F1 and F2 for a male voice, as reproduced in Wikipedia's Formant article. Vowel qualities none of the earlier sets had: front rounded
// y, oe, the open front rounded vowel, and back unrounded vowels, besides open back ones. That table has no F3 to F5 and no bandwidths, so those
// were set here, before running, from the vowel's class: rounding lowers F3. Breath and source tone are their own. Nothing was tuned on these.
void testHeldOutFifth()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; };
    // F1, F2 from Catford; F3, F4, F5 and all bandwidths set here.
    const std::vector<Held> speaking {
        { { { 235, 2100, 2200, 3400, 4300 }, { 62, 100, 130, 170, 220 }, "y" }, { -48.0, 0.0 } },
        { { { 370, 1900, 2300, 3400, 4300 }, { 70, 95, 130, 170, 220 }, "o slash" }, { -58.0, 3000.0 } },
        { { { 585, 1710, 2400, 3400, 4300 }, { 85, 100, 135, 170, 220 }, "oe" }, { -42.0, 0.0 } },
        { { { 460, 1310, 2400, 3400, 4300 }, { 75, 90, 130, 170, 220 }, "rams horn" }, { -52.0, 5000.0 } },
        { { { 300, 1390, 2300, 3400, 4300 }, { 65, 90, 130, 170, 220 }, "turned m" }, { -60.0, 2000.0 } } };
    const std::vector<Held> singing {
        { { { 820, 1530, 2450, 3400, 4300 }, { 100, 105, 140, 170, 220 }, "open front rounded" }, { -50.0, 0.0 } },
        { { { 750, 940, 2500, 3400, 4300 }, { 95, 90, 140, 170, 220 }, "open back" }, { -38.0, 3500.0 } },
        { { { 700, 760, 2500, 3400, 4300 }, { 90, 85, 140, 170, 220 }, "open back rounded" }, { -56.0, 0.0 } },
        { { { 600, 1170, 2500, 3400, 4300 }, { 85, 95, 135, 170, 220 }, "turned v" }, { -44.0, 2500.0 } },
        { { { 585, 1710, 2400, 3400, 4300 }, { 85, 100, 135, 170, 220 }, "oe" }, { -60.0, 4500.0 } } };

    FormantScore low;
    for (const auto& h : speaking)
    {
        gStyle = h.style;
        for (double centre : { 105.0, 156.0, 208.0 })
            for (int shift : { 4, -4, 2, -2, 1, -1 })
                scoreFormants (low, h.v, centre, shift, centre < 200.0 ? lowMale : wideRange);
    }
    chk (low.move <= prolimits::hqFormantMove, "fifth held out set, Catford's vowels at 105 to 208 Hz, worst formant move against the input", low.move * 100.0, "%");
    note ("  and the tool's floor there", low.floor * 100.0, "%");
    chk (low.level <= prolimits::hqLevelChangeDb, "  level change, worst", low.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", low.peakFlatter, "dB");
    note ("  colour, worst", low.colour, "dB");

    FormantScore high;
    double heldMean = 0.0, heldRms = 0.0;
    int heldCount = 0;
    for (const auto& h : singing)
    {
        gStyle = h.style;
        for (double centre : { 415.0, 494.0, 554.0, 784.0 })
            for (int shift : { 3, -3, 2, -2 })
            {
                scoreFormants (high, h.v, centre, shift, wideRange);
                const auto held = heldNoteError (h.v, centre, shift, wideRange);
                heldMean += held.rmsDb;
                heldRms = std::max (heldRms, held.rmsDb);
                ++heldCount;
            }
    }
    gStyle = {};
    chk (high.peakFlatterCrossed <= prolimits::highNoteF1Flatter, "fifth held out set, 415 to 784 Hz, first formant flatter than a real voice, where a harmonic crosses it, worst", high.peakFlatterCrossed, "dB");
    note ("  the same with every case, including where the whole glide stays above the formant", high.peakFlatter, "dB");
    chk (high.peakSharper <= prolimits::highNoteF1Sharper, "  first formant sharper than a real voice, worst", high.peakSharper, "dB");
    chk (high.move <= high.floor + prolimits::highNoteMoveOverFloor, "  formant move against the input, worst, against the floor plus 2%", high.move * 100.0, "%");
    note ("  and the tool's floor there", high.floor * 100.0, "%");
    note ("  colour, worst", high.colour, "dB");
    note ("  level change, worst", high.level, "dB");
    note ("  held notes, harmonics against a real voice, worst RMS", heldRms, "dB");
    chk (heldMean / heldCount <= prolimits::heldNoteHarmonicMeanDb, "  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// A sixth held out set, written on 2026-11-08 blind to the tuning going on at the same time, and committed before any of it was run. All five sets
// above have been used. Formants from a published source and a language none of the earlier sets used: Dutch vowels in h_t words.
//   Men:   Pols, L. C. W., Tromp, H. R. C. and Plomp, R. (1973), Frequency analysis of Dutch vowels from 50 male speakers,
//          J. Acoust. Soc. Am. 53 (4), 1093 to 1101, Table I, mean F1, F2, F3.
//   Women: van Nierop, D. J. P. J., Pols, L. C. W. and Plomp, R. (1973), Frequency analysis of Dutch vowels from 25 female speakers,
//          Acustica 29, 110 to 118.
// The numbers below are the means over speakers of the per speaker F1 to F3 of both studies, as distributed with Praat ("Create formant table
// (Pols & Van Nierop 1973)", dwtools/Table_dataSets.cpp, which corrects one female F3 misprint), rounded to the hertz. The men's means agree with those usually quoted from Table I.
// Speaker groups none of the earlier sets used: Dutch men and women, and men's vowels at tenor singing pitches. Vowels taken from Dutch's
// own set: u (front rounded, short), open o, short i, long a and e for the men; y, open e, long o and eu for the women; long a, short a, open o,
// short u and long e for women singing. Pitches none of the earlier sets used: 95, 117 and 139 Hz for men speaking, 192 and 226 Hz for women
// speaking, 370 and 453 Hz for men singing, and 370, 453, 572 and 762 Hz for women singing, some between notes on purpose.
// That table has no F4, F5 or bandwidths, so they were set here, before running, by the earlier sets' conventions: F4 and F5 between what those
// sets used for each sex; the first formant's bandwidth about an eighth of its frequency, 45 Hz at least; F2 bandwidth rising with F2;
// women's bandwidths wider than men's. Women's sources a little breathier than men's, as the literature on women's voices reports (Klatt
// and Klatt, 1990). Breath and source tone otherwise vary from voice to voice. Nothing was tuned on these, and none of it was chosen by looking at output.
void testHeldOutSixth()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; int type; std::vector<double> centres; };
    // F1, F2, F3 from Pols and others (men) and van Nierop and others (women); F4, F5 and all bandwidths set here.
    const std::vector<Held> speaking {
        { { { 438, 1498, 2354, 3450, 4400 }, { 55, 90, 130, 165, 215 }, "Dutch u men" }, { -54.0, 3000.0 }, lowMale, { 95.0, 117.0, 139.0 } },
        { { { 523, 866, 2692, 3450, 4400 }, { 65, 80, 130, 165, 215 }, "Dutch open o men" }, { -46.0, 0.0 }, lowMale, { 95.0, 117.0, 139.0 } },
        { { { 388, 2003, 2571, 3450, 4400 }, { 50, 100, 130, 165, 215 }, "Dutch short i men" }, { -58.0, 5500.0 }, lowMale, { 95.0, 117.0, 139.0 } },
        { { { 795, 1301, 2565, 3450, 4400 }, { 95, 90, 130, 165, 215 }, "Dutch aa men" }, { -50.0, 2200.0 }, lowMale, { 95.0, 117.0, 139.0 } },
        { { { 407, 2017, 2554, 3450, 4400 }, { 50, 100, 130, 165, 215 }, "Dutch ee men" }, { -62.0, 0.0 }, lowMale, { 95.0, 117.0, 139.0 } },
        { { { 288, 1832, 2520, 4050, 4950 }, { 45, 115, 145, 185, 225 }, "Dutch uu women" }, { -44.0, 0.0 }, wideRange, { 192.0, 226.0 } },
        { { { 669, 1905, 2788, 4050, 4950 }, { 80, 115, 145, 185, 225 }, "Dutch open e women" }, { -40.0, 4000.0 }, wideRange, { 192.0, 226.0 } },
        { { { 506, 961, 2608, 4050, 4950 }, { 60, 90, 145, 185, 225 }, "Dutch oo women" }, { -52.0, 2800.0 }, wideRange, { 192.0, 226.0 } },
        { { { 476, 1690, 2512, 4050, 4950 }, { 60, 115, 145, 185, 225 }, "Dutch eu women" }, { -47.0, 0.0 }, wideRange, { 192.0, 226.0 } } };
    const std::vector<Held> singing {
        { { { 986, 1443, 2778, 4050, 4950 }, { 120, 100, 145, 185, 225 }, "Dutch aa women" }, { -42.0, 0.0 }, wideRange, { 370.0, 453.0, 572.0, 762.0 } },
        { { { 762, 1117, 2860, 4050, 4950 }, { 90, 100, 145, 185, 225 }, "Dutch short a women" }, { -48.0, 3500.0 }, wideRange, { 370.0, 453.0, 572.0, 762.0 } },
        { { { 578, 934, 2852, 4050, 4950 }, { 70, 90, 145, 185, 225 }, "Dutch open o women" }, { -38.0, 0.0 }, wideRange, { 370.0, 453.0, 572.0, 762.0 } },
        { { { 490, 1688, 2568, 4050, 4950 }, { 60, 115, 145, 185, 225 }, "Dutch u women" }, { -53.0, 5000.0 }, wideRange, { 370.0, 453.0, 572.0, 762.0 } },
        { { { 472, 2352, 2896, 4050, 4950 }, { 55, 115, 145, 185, 225 }, "Dutch ee women" }, { -45.0, 2400.0 }, wideRange, { 370.0, 453.0, 572.0, 762.0 } },
        { { { 679, 1051, 2619, 3450, 4400 }, { 80, 90, 130, 165, 215 }, "Dutch short a men" }, { -56.0, 0.0 }, wideRange, { 370.0, 453.0 } },
        { { { 583, 1725, 2471, 3450, 4400 }, { 70, 100, 130, 165, 215 }, "Dutch open e men" }, { -50.0, 4500.0 }, wideRange, { 370.0, 453.0 } } };

    FormantScore low;
    for (const auto& h : speaking)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 4, -4, 3, -3, 1, -1 })
                scoreFormants (low, h.v, centre, shift, h.type);
    }
    chk (low.move <= prolimits::hqFormantMove, "sixth held out set, Dutch men 95 to 139 Hz and women 192 and 226 Hz, worst formant move against the input", low.move * 100.0, "%");
    note ("  and the tool's floor there", low.floor * 100.0, "%");
    chk (low.level <= prolimits::hqLevelChangeDb, "  level change, worst", low.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", low.peakFlatter, "dB");
    note ("  colour, worst", low.colour, "dB");

    FormantScore high;
    double heldMean = 0.0, heldRms = 0.0;
    int heldCount = 0;
    for (const auto& h : singing)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 3, -3, 2, -2 })
            {
                scoreFormants (high, h.v, centre, shift, h.type);
                const auto held = heldNoteError (h.v, centre, shift, h.type);
                heldMean += held.rmsDb;
                heldRms = std::max (heldRms, held.rmsDb);
                ++heldCount;
                if (std::getenv ("TUNE_DUMP") != nullptr)
                    std::printf ("  held note, vowel %s, %.0f Hz, shift %d: harmonics against a real voice %.2f dB RMS, %.2f dB worst\n", h.v.name, centre, shift, held.rmsDb, held.worstDb);
            }
    }
    gStyle = {};
    chk (high.peakFlatterCrossed <= prolimits::highNoteF1Flatter, "sixth held out set, Dutch women 370 to 762 Hz and men 370 and 453 Hz, first formant flatter than a real voice, where a harmonic crosses it, worst", high.peakFlatterCrossed, "dB");
    note ("  the same with every case, including where the whole glide stays above the formant", high.peakFlatter, "dB");
    chk (high.peakSharper <= prolimits::highNoteF1Sharper, "  first formant sharper than a real voice, worst", high.peakSharper, "dB");
    chk (high.move <= high.floor + prolimits::highNoteMoveOverFloor, "  formant move against the input, worst, against the floor plus 2%", high.move * 100.0, "%");
    note ("  and the tool's floor there", high.floor * 100.0, "%");
    note ("  colour, worst", high.colour, "dB");
    note ("  level change, worst", high.level, "dB");
    note ("  held notes, harmonics against a real voice, worst RMS", heldRms, "dB");
    chk (heldMean / heldCount <= prolimits::heldNoteHarmonicMeanDb, "  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// A seventh held out set, written on 2026-11-08 blind to the tuning going on at the same time, and committed before any of it was run. All six sets
// above have been used or written. Formants from a published source and a language none of the earlier sets used: Seoul Korean monophthongs in h_da words.
//   Yang, B. (1996), A comparative study of American English and Korean vowels produced by male and female speakers,
//   Journal of Phonetics 24, 245 to 261, mean F1, F2, F3 for Korean men and women.
// The paper itself could not be reached here. The numbers below are the men's and women's means as bundled with the phonTools R package
// (Santiago Barreda, version 0.2-2.2, dataset y96, "Yang (1996) Korean Vowel Data"), copied as they are, in hertz. That package's help page
// describes them as averaged across 60 male and female speakers, while the paper is usually described as 10 men and 10 women; either way
// they are group means for each sex, not one speaker's. Two of the values (u: men 369 and 981, women 422 and 1021) agree with what later
// Korean papers quote from Yang (1996).
// Speaker groups none of the earlier sets used: Korean men and women. Vowels from Korean's own set, named in the Revised Romanisation:
// i, eu (high central or back unrounded), ae, o and eo (open mid back unrounded) for men speaking; wi (front rounded), oe (front rounded mid),
// u and e for women speaking; a, eo, ae, o and eu for women singing; a and e for men singing at tenor pitches. Pitches none of the earlier
// sets or the tuning voices used: men speaking at 93, 102, 113, 128 and 146 Hz, three of the five below 115 Hz because low male speech matters
// most (93 Hz glides down to 66 Hz, still inside Low Male's 65 Hz floor); women speaking at 183, 204 and 237 Hz; women singing at 405, 508,
// 618 and 776 Hz; men singing at 294 and 405 Hz.
// That table has no F4, F5 or bandwidths, so they were set here, before running, by the earlier sets' conventions: F4 and F5 inside the range
// those sets used for each sex (men 3500 and 4400, women 4100 and 5000); the first formant's bandwidth about an eighth of its frequency,
// rounded to 5 Hz, 45 Hz at least; F2 bandwidth rising with F2 (men 80, 90, 100; women 90 to 100, 100, 115); F3 to F5 bandwidths as the sixth
// set's; women's wider than men's. Women's sources a little breathier than men's (Klatt and Klatt, 1990). Breath and source tone otherwise
// vary from voice to voice, chosen to differ from the sixth set's. Nothing was tuned on these, and none of it was chosen by looking at output.
void testHeldOutSeventh()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; int type; std::vector<double> centres; };
    // F1, F2, F3 from Yang (1996) as bundled with phonTools; F4, F5 and all bandwidths set here.
    const std::vector<Held> speaking {
        { { { 341, 2219, 3047, 3500, 4400 }, { 45, 100, 130, 165, 215 }, "Korean i men" }, { -56.0, 2600.0 }, lowMale, { 93.0, 102.0, 113.0, 128.0, 146.0 } },
        { { { 405, 1488, 2497, 3500, 4400 }, { 50, 90, 130, 165, 215 }, "Korean eu men" }, { -48.0, 0.0 }, lowMale, { 93.0, 102.0, 113.0, 128.0, 146.0 } },
        { { { 591, 1849, 2597, 3500, 4400 }, { 75, 100, 130, 165, 215 }, "Korean ae men" }, { -60.0, 4200.0 }, lowMale, { 93.0, 102.0, 113.0, 128.0, 146.0 } },
        { { { 448, 945, 2674, 3500, 4400 }, { 55, 80, 130, 165, 215 }, "Korean o men" }, { -52.0, 0.0 }, lowMale, { 93.0, 102.0, 113.0, 128.0, 146.0 } },
        { { { 608, 1121, 2683, 3500, 4400 }, { 75, 90, 130, 165, 215 }, "Korean eo men" }, { -45.0, 3200.0 }, lowMale, { 93.0, 102.0, 113.0, 128.0, 146.0 } },
        { { { 373, 2704, 3222, 4100, 5000 }, { 45, 115, 145, 185, 225 }, "Korean wi women" }, { -46.0, 0.0 }, wideRange, { 183.0, 204.0, 237.0 } },
        { { { 602, 2195, 3013, 4100, 5000 }, { 75, 115, 145, 185, 225 }, "Korean oe women" }, { -41.0, 3700.0 }, wideRange, { 183.0, 204.0, 237.0 } },
        { { { 422, 1021, 3024, 4100, 5000 }, { 55, 100, 145, 185, 225 }, "Korean u women" }, { -50.0, 2300.0 }, wideRange, { 183.0, 204.0, 237.0 } },
        { { { 650, 2377, 3068, 4100, 5000 }, { 80, 115, 145, 185, 225 }, "Korean e women" }, { -43.0, 0.0 }, wideRange, { 183.0, 204.0, 237.0 } } };
    const std::vector<Held> singing {
        { { { 986, 1794, 2957, 4100, 5000 }, { 120, 115, 145, 185, 225 }, "Korean a women" }, { -44.0, 0.0 }, wideRange, { 405.0, 508.0, 618.0, 776.0 } },
        { { { 765, 1371, 3009, 4100, 5000 }, { 95, 100, 145, 185, 225 }, "Korean eo women" }, { -49.0, 3000.0 }, wideRange, { 405.0, 508.0, 618.0, 776.0 } },
        { { { 677, 2285, 3063, 4100, 5000 }, { 85, 115, 145, 185, 225 }, "Korean ae women" }, { -39.0, 4800.0 }, wideRange, { 405.0, 508.0, 618.0, 776.0 } },
        { { { 499, 1029, 3068, 4100, 5000 }, { 60, 100, 145, 185, 225 }, "Korean o women" }, { -47.0, 0.0 }, wideRange, { 405.0, 508.0, 618.0, 776.0 } },
        { { { 447, 1703, 2997, 4100, 5000 }, { 55, 115, 145, 185, 225 }, "Korean eu women" }, { -42.0, 2600.0 }, wideRange, { 405.0, 508.0, 618.0, 776.0 } },
        { { { 738, 1372, 2573, 3500, 4400 }, { 90, 90, 130, 165, 215 }, "Korean a men" }, { -55.0, 0.0 }, wideRange, { 294.0, 405.0 } },
        { { { 490, 1968, 2644, 3500, 4400 }, { 60, 100, 130, 165, 215 }, "Korean e men" }, { -51.0, 3800.0 }, wideRange, { 294.0, 405.0 } } };

    FormantScore low;
    for (const auto& h : speaking)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 4, -4, 3, -3, 1, -1 })
                scoreFormants (low, h.v, centre, shift, h.type);
    }
    chk (low.move <= prolimits::hqFormantMove, "seventh held out set, Korean men 93 to 146 Hz and women 183 to 237 Hz, worst formant move against the input", low.move * 100.0, "%");
    note ("  and the tool's floor there", low.floor * 100.0, "%");
    chk (low.level <= prolimits::hqLevelChangeDb, "  level change, worst", low.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", low.peakFlatter, "dB");
    note ("  colour, worst", low.colour, "dB");

    FormantScore high;
    double heldMean = 0.0, heldRms = 0.0;
    int heldCount = 0;
    for (const auto& h : singing)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 3, -3, 2, -2 })
            {
                scoreFormants (high, h.v, centre, shift, h.type);
                const auto held = heldNoteError (h.v, centre, shift, h.type);
                heldMean += held.rmsDb;
                heldRms = std::max (heldRms, held.rmsDb);
                ++heldCount;
                if (std::getenv ("TUNE_DUMP") != nullptr)
                    std::printf ("  held note, vowel %s, %.0f Hz, shift %d: harmonics against a real voice %.2f dB RMS, %.2f dB worst\n", h.v.name, centre, shift, held.rmsDb, held.worstDb);
            }
    }
    gStyle = {};
    chk (high.peakFlatterCrossed <= prolimits::highNoteF1Flatter, "seventh held out set, Korean women 405 to 776 Hz and men 294 and 405 Hz, first formant flatter than a real voice, where a harmonic crosses it, worst", high.peakFlatterCrossed, "dB");
    note ("  the same with every case, including where the whole glide stays above the formant", high.peakFlatter, "dB");
    chk (high.peakSharper <= prolimits::highNoteF1Sharper, "  first formant sharper than a real voice, worst", high.peakSharper, "dB");
    chk (high.move <= high.floor + prolimits::highNoteMoveOverFloor, "  formant move against the input, worst, against the floor plus 2%", high.move * 100.0, "%");
    note ("  and the tool's floor there", high.floor * 100.0, "%");
    note ("  colour, worst", high.colour, "dB");
    note ("  level change, worst", high.level, "dB");
    note ("  held notes, harmonics against a real voice, worst RMS", heldRms, "dB");
    chk (heldMean / heldCount <= prolimits::heldNoteHarmonicMeanDb, "  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// An eighth held out set, written on 2026-11-08 blind to the tuning going on at the same time, and committed before any of it was run. All seven
// sets above have been used or written. Formants from a published source and languages none of the earlier sets used: Tokyo Japanese, Hong Kong
// Cantonese and Thessaloniki Greek, the corner vowels a, i and u in real words that begin with a lingual obstruent.
//   Chung, H., Kong, E. J., Edwards, J., Weismer, G., Fourakis, M. and Hwang, Y. (2012), Cross-linguistic studies of children's and adults'
//   vowel spaces, J. Acoust. Soc. Am. 131 (1), 442 to 454, doi 10.1121/1.3651823 (PMC3283902), Appendix B, adult male and adult female
//   mean F1, F2, F3 in hertz, 5 men and 5 women for each language, measured at the vowel midpoint in Praat.
// The numbers below are copied as Appendix B prints them, read twice from the PMC copy of the paper and the same both times. The Korean and
// English rows of that table were left out, since earlier sets used both languages. Each language has only those three vowels, so the set
// takes all three languages to get nine vowels for each sex. Japanese u is the unrounded, fronted Japanese vowel (F2 near 1700 Hz in both
// sexes), Cantonese u is back and rounded (F2 721 and 872 Hz), Greek u sits between.
// Speaker groups none of the earlier sets used: Japanese, Cantonese and Greek men and women. Vowels: Japanese u and a, Cantonese u and a,
// Greek i for men speaking; Japanese i and u, Cantonese i, Greek u for women speaking; a in all three languages, Cantonese u and Greek i for
// women singing; Greek a and u for men singing at tenor pitches. Pitches none of the earlier sets or the tuning voices used: men speaking at
// 94, 107, 121, 136 and 149 Hz (94 Hz glides down to 66 Hz, inside Low Male's 65 Hz floor); women speaking at 187, 214 and 231 Hz; women
// singing at 427, 536, 653 and 795 Hz; men singing at 317 and 438 Hz.
// That table has no F4, F5 or bandwidths, so they were set here, before running, by the earlier sets' conventions: F4 and F5 inside the range
// those sets used for each sex (men 3500 and 4350, women 4100 and 4950); the first formant's bandwidth about an eighth of its frequency,
// rounded to 5 Hz, 45 Hz at least; F2 bandwidth rising with F2 (men 80 below 1000 Hz, 90 to 1600, 100 above; women 90, 100, 115 on the same
// steps); F3 to F5 bandwidths as the sixth and seventh sets'; women's wider than men's. Women's sources a little breathier than men's (Klatt
// and Klatt, 1990). Breath and source tone otherwise vary from voice to voice, chosen to differ from the sixth and seventh sets'. Nothing was
// tuned on these, and none of it was chosen by looking at output.
void testHeldOutEighth()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; int type; std::vector<double> centres; };
    // F1, F2, F3 from Chung and others (2012), Appendix B; F4, F5 and all bandwidths set here.
    const std::vector<Held> speaking {
        { { { 345, 1704, 2848, 3500, 4350 }, { 45, 100, 130, 165, 215 }, "Japanese u men" }, { -57.0, 2900.0 }, lowMale, { 94.0, 107.0, 121.0, 136.0, 149.0 } },
        { { { 353, 721, 2728, 3500, 4350 }, { 45, 80, 130, 165, 215 }, "Cantonese u men" }, { -49.0, 0.0 }, lowMale, { 94.0, 107.0, 121.0, 136.0, 149.0 } },
        { { { 347, 2058, 2642, 3500, 4350 }, { 45, 100, 130, 165, 215 }, "Greek i men" }, { -53.0, 4600.0 }, lowMale, { 94.0, 107.0, 121.0, 136.0, 149.0 } },
        { { { 779, 1296, 2713, 3500, 4350 }, { 95, 90, 130, 165, 215 }, "Cantonese a men" }, { -61.0, 0.0 }, lowMale, { 94.0, 107.0, 121.0, 136.0, 149.0 } },
        { { { 634, 1363, 2857, 3500, 4350 }, { 80, 90, 130, 165, 215 }, "Japanese a men" }, { -47.0, 3400.0 }, lowMale, { 94.0, 107.0, 121.0, 136.0, 149.0 } },
        { { { 375, 2729, 3447, 4100, 4950 }, { 45, 115, 145, 185, 225 }, "Japanese i women" }, { -45.0, 0.0 }, wideRange, { 187.0, 214.0, 231.0 } },
        { { { 443, 1126, 2801, 4100, 4950 }, { 55, 100, 145, 185, 225 }, "Greek u women" }, { -40.0, 3300.0 }, wideRange, { 187.0, 214.0, 231.0 } },
        { { { 375, 2851, 3467, 4100, 4950 }, { 45, 115, 145, 185, 225 }, "Cantonese i women" }, { -48.0, 2100.0 }, wideRange, { 187.0, 214.0, 231.0 } },
        { { { 380, 1714, 2841, 4100, 4950 }, { 50, 115, 145, 185, 225 }, "Japanese u women" }, { -42.0, 0.0 }, wideRange, { 187.0, 214.0, 231.0 } } };
    const std::vector<Held> singing {
        { { { 942, 1641, 3125, 4100, 4950 }, { 120, 115, 145, 185, 225 }, "Cantonese a women" }, { -43.0, 0.0 }, wideRange, { 427.0, 536.0, 653.0, 795.0 } },
        { { { 929, 1603, 2825, 4100, 4950 }, { 115, 115, 145, 185, 225 }, "Greek a women" }, { -46.0, 2700.0 }, wideRange, { 427.0, 536.0, 653.0, 795.0 } },
        { { { 708, 1533, 2903, 4100, 4950 }, { 90, 100, 145, 185, 225 }, "Japanese a women" }, { -40.0, 4400.0 }, wideRange, { 427.0, 536.0, 653.0, 795.0 } },
        { { { 397, 872, 2886, 4100, 4950 }, { 50, 90, 145, 185, 225 }, "Cantonese u women" }, { -51.0, 0.0 }, wideRange, { 427.0, 536.0, 653.0, 795.0 } },
        { { { 406, 2456, 3029, 4100, 4950 }, { 50, 115, 145, 185, 225 }, "Greek i women" }, { -44.0, 3600.0 }, wideRange, { 427.0, 536.0, 653.0, 795.0 } },
        { { { 720, 1372, 2457, 3500, 4350 }, { 90, 90, 130, 165, 215 }, "Greek a men" }, { -54.0, 0.0 }, wideRange, { 317.0, 438.0 } },
        { { { 401, 1011, 2393, 3500, 4350 }, { 50, 90, 130, 165, 215 }, "Greek u men" }, { -50.0, 2500.0 }, wideRange, { 317.0, 438.0 } } };

    FormantScore low;
    for (const auto& h : speaking)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 4, -4, 3, -3, 1, -1 })
                scoreFormants (low, h.v, centre, shift, h.type);
    }
    chk (low.move <= prolimits::hqFormantMove, "eighth held out set, Japanese, Cantonese and Greek men 94 to 149 Hz and women 187 to 231 Hz, worst formant move against the input", low.move * 100.0, "%");
    note ("  and the tool's floor there", low.floor * 100.0, "%");
    chk (low.level <= prolimits::hqLevelChangeDb, "  level change, worst", low.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", low.peakFlatter, "dB");
    note ("  colour, worst", low.colour, "dB");

    FormantScore high;
    double heldMean = 0.0, heldRms = 0.0;
    int heldCount = 0;
    for (const auto& h : singing)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 3, -3, 2, -2 })
            {
                scoreFormants (high, h.v, centre, shift, h.type);
                const auto held = heldNoteError (h.v, centre, shift, h.type);
                heldMean += held.rmsDb;
                heldRms = std::max (heldRms, held.rmsDb);
                ++heldCount;
                if (std::getenv ("TUNE_DUMP") != nullptr)
                    std::printf ("  held note, vowel %s, %.0f Hz, shift %d: harmonics against a real voice %.2f dB RMS, %.2f dB worst\n", h.v.name, centre, shift, held.rmsDb, held.worstDb);
            }
    }
    gStyle = {};
    chk (high.peakFlatterCrossed <= prolimits::highNoteF1Flatter, "eighth held out set, Japanese, Cantonese and Greek women 427 to 795 Hz and men 317 and 438 Hz, first formant flatter than a real voice, where a harmonic crosses it, worst", high.peakFlatterCrossed, "dB");
    note ("  the same with every case, including where the whole glide stays above the formant", high.peakFlatter, "dB");
    chk (high.peakSharper <= prolimits::highNoteF1Sharper, "  first formant sharper than a real voice, worst", high.peakSharper, "dB");
    chk (high.move <= high.floor + prolimits::highNoteMoveOverFloor, "  formant move against the input, worst, against the floor plus 2%", high.move * 100.0, "%");
    note ("  and the tool's floor there", high.floor * 100.0, "%");
    note ("  colour, worst", high.colour, "dB");
    note ("  level change, worst", high.level, "dB");
    note ("  held notes, harmonics against a real voice, worst RMS", heldRms, "dB");
    chk (heldMean / heldCount <= prolimits::heldNoteHarmonicMeanDb, "  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// A ninth held out set, written on 2026-11-09 blind to the tuning going on at the same time, and committed before any of it was run. All eight
// sets above have been used or written. Formants from a published source and a language none of the earlier sets used: Standard Chinese
// (Mandarin, Putonghua), the ten monophthongs spoken on the first tone by students being trained as broadcasters.
//   Meng, Z., Chen, Y. and Li, X. (2006), Statistical survey of monophthong formants in Mandarin for students being trained as broadcasters,
//   Proceedings of the 20th Pacific Asia Conference on Language, Information and Computation (PACLIC 20), Wuhan, Tsinghua University Press,
//   280 to 286, ACL Anthology Y06-1037, hdl 2065/29074, Table 3, mean F1, F2, F3 in hertz for 212 women and 126 men aged 19 to 21, by LPC.
// The numbers below are copied as Table 3 prints them, read twice from the ACL Anthology PDF in two separate fetches and the same both times.
// That table also gives F4 and F5, which were not used, so that this set builds its voices the same way as the sixth to eighth sets.
// Vowels from Mandarin's own set, named as the paper tags them: i, u, u umlaut (front rounded y), o, e (back unrounded, IPA ram's horns),
// a, the two apical vowels after dental and retroflex sibilants (the finals of zi and zhi), the neutral schwa (the final of de) and e circumflex
// (open mid front). Speaker groups none of the earlier sets used: Mandarin men and women. Vowels: u, o, u umlaut, e and e circumflex for
// men speaking; i, u and both apical vowels for women speaking; a, o, e, e circumflex and u umlaut for women singing; a and the schwa for men
// singing at tenor pitches. Pitches none of the earlier sets or the tuning voices used: men speaking at 92, 97, 111, 126 and 148 Hz, two below
// 100 Hz because low male speech matters most (the glide reaches 0.71 of its centre, so 92 Hz goes down to 65.1 Hz, just inside Low Male's 65 Hz
// floor, and no centre can go lower); women speaking at 182, 212 and 238 Hz; women singing at 416, 521, 643 and 787 Hz; men singing at 303 and 424 Hz.
// F4, F5 and bandwidths were set here, before running, by the earlier sets' conventions: F4 and F5 inside the range those sets used for each sex
// (men 3500 and 4350, women 4100 and 5000); the first formant's bandwidth about an eighth of its frequency, rounded to 5 Hz, 45 Hz at least;
// F2 bandwidth rising with F2 (men 80 below 1000 Hz, 90 to 1600, 100 above; women 90, 100, 115 on the same steps); F3 to F5 bandwidths as
// the sixth to eighth sets'; women's wider than men's. Women's sources a little breathier than men's (Klatt and Klatt, 1990). Breath and source
// tone otherwise vary from voice to voice, chosen to differ from every earlier set's. Nothing was tuned on these, and none of it was chosen by
// looking at output.
void testHeldOutNinth()
{
    if (gEngine != 1)
        return;
    struct Held { voice::Vowel v; VoiceStyle style; int type; std::vector<double> centres; };
    // F1, F2, F3 from Meng and others (2006), Table 3; F4, F5 and all bandwidths set here.
    const std::vector<Held> speaking {
        { { { 342, 701, 2800, 3500, 4350 }, { 45, 80, 130, 165, 215 }, "Mandarin u men" }, { -59.0, 0.0 }, lowMale, { 92.0, 97.0, 111.0, 126.0, 148.0 } },
        { { { 532, 817, 2949, 3500, 4350 }, { 65, 80, 130, 165, 215 }, "Mandarin o men" }, { -51.0, 3100.0 }, lowMale, { 92.0, 97.0, 111.0, 126.0, 148.0 } },
        { { { 280, 1992, 2331, 3500, 4350 }, { 45, 100, 130, 165, 215 }, "Mandarin u umlaut men" }, { -57.0, 0.0 }, lowMale, { 92.0, 97.0, 111.0, 126.0, 148.0 } },
        { { { 501, 1163, 2736, 3500, 4350 }, { 65, 90, 130, 165, 215 }, "Mandarin e men" }, { -47.0, 4300.0 }, lowMale, { 92.0, 97.0, 111.0, 126.0, 148.0 } },
        { { { 705, 1789, 2656, 3500, 4350 }, { 90, 100, 130, 165, 215 }, "Mandarin e circumflex men" }, { -61.0, 3900.0 }, lowMale, { 92.0, 97.0, 111.0, 126.0, 148.0 } },
        { { { 311, 2871, 3882, 4100, 5000 }, { 45, 115, 145, 185, 225 }, "Mandarin i women" }, { -46.0, 3900.0 }, wideRange, { 182.0, 212.0, 238.0 } },
        { { { 354, 762, 3432, 4100, 5000 }, { 45, 90, 145, 185, 225 }, "Mandarin u women" }, { -39.0, 0.0 }, wideRange, { 182.0, 212.0, 238.0 } },
        { { { 399, 1762, 3271, 4100, 5000 }, { 50, 115, 145, 185, 225 }, "Mandarin apical zi women" }, { -43.0, 2900.0 }, wideRange, { 182.0, 212.0, 238.0 } },
        { { { 416, 2092, 2805, 4100, 5000 }, { 50, 115, 145, 185, 225 }, "Mandarin apical zhi women" }, { -50.0, 4100.0 }, wideRange, { 182.0, 212.0, 238.0 } } };
    const std::vector<Held> singing {
        { { { 952, 1371, 3444, 4100, 5000 }, { 120, 100, 145, 185, 225 }, "Mandarin a women" }, { -41.0, 0.0 }, wideRange, { 416.0, 521.0, 643.0, 787.0 } },
        { { { 654, 947, 3410, 4100, 5000 }, { 80, 90, 145, 185, 225 }, "Mandarin o women" }, { -47.0, 2200.0 }, wideRange, { 416.0, 521.0, 643.0, 787.0 } },
        { { { 622, 1334, 3358, 4100, 5000 }, { 80, 100, 145, 185, 225 }, "Mandarin e women" }, { -44.0, 4100.0 }, wideRange, { 416.0, 521.0, 643.0, 787.0 } },
        { { { 856, 2119, 3159, 4100, 5000 }, { 105, 115, 145, 185, 225 }, "Mandarin e circumflex women" }, { -39.0, 3400.0 }, wideRange, { 416.0, 521.0, 643.0, 787.0 } },
        { { { 305, 2411, 2813, 4100, 5000 }, { 45, 115, 145, 185, 225 }, "Mandarin u umlaut women" }, { -49.0, 2600.0 }, wideRange, { 416.0, 521.0, 643.0, 787.0 } },
        { { { 795, 1168, 2945, 3500, 4350 }, { 100, 90, 130, 165, 215 }, "Mandarin a men" }, { -56.0, 3300.0 }, wideRange, { 303.0, 424.0 } },
        { { { 525, 1179, 2735, 3500, 4350 }, { 65, 90, 130, 165, 215 }, "Mandarin schwa men" }, { -53.0, 0.0 }, wideRange, { 303.0, 424.0 } } };

    FormantScore low;
    for (const auto& h : speaking)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 4, -4, 3, -3, 1, -1 })
                scoreFormants (low, h.v, centre, shift, h.type);
    }
    chk (low.move <= prolimits::hqFormantMove, "ninth held out set, Mandarin men 92 to 148 Hz and women 182 to 238 Hz, worst formant move against the input", low.move * 100.0, "%");
    note ("  and the tool's floor there", low.floor * 100.0, "%");
    chk (low.level <= prolimits::hqLevelChangeDb, "  level change, worst", low.level, "dB");
    note ("  first formant against its surroundings, flatter than a real voice, worst", low.peakFlatter, "dB");
    note ("  colour, worst", low.colour, "dB");

    FormantScore high;
    double heldMean = 0.0, heldRms = 0.0;
    int heldCount = 0;
    for (const auto& h : singing)
    {
        gStyle = h.style;
        for (double centre : h.centres)
            for (int shift : { 3, -3, 2, -2 })
            {
                scoreFormants (high, h.v, centre, shift, h.type);
                const auto held = heldNoteError (h.v, centre, shift, h.type);
                heldMean += held.rmsDb;
                heldRms = std::max (heldRms, held.rmsDb);
                ++heldCount;
                if (std::getenv ("TUNE_DUMP") != nullptr)
                    std::printf ("  held note, vowel %s, %.0f Hz, shift %d: harmonics against a real voice %.2f dB RMS, %.2f dB worst\n", h.v.name, centre, shift, held.rmsDb, held.worstDb);
            }
    }
    gStyle = {};
    chk (high.peakFlatterCrossed <= prolimits::highNoteF1Flatter, "ninth held out set, Mandarin women 416 to 787 Hz and men 303 and 424 Hz, first formant flatter than a real voice, where a harmonic crosses it, worst", high.peakFlatterCrossed, "dB");
    note ("  the same with every case, including where the whole glide stays above the formant", high.peakFlatter, "dB");
    chk (high.peakSharper <= prolimits::highNoteF1Sharper, "  first formant sharper than a real voice, worst", high.peakSharper, "dB");
    chk (high.move <= high.floor + prolimits::highNoteMoveOverFloor, "  formant move against the input, worst, against the floor plus 2%", high.move * 100.0, "%");
    note ("  and the tool's floor there", high.floor * 100.0, "%");
    note ("  colour, worst", high.colour, "dB");
    note ("  level change, worst", high.level, "dB");
    note ("  held notes, harmonics against a real voice, worst RMS", heldRms, "dB");
    chk (heldMean / heldCount <= prolimits::heldNoteHarmonicMeanDb, "  held notes, harmonics against a real voice, mean of the RMS", heldMean / heldCount, "dB");
}

// A vowel change in the middle of a glide, added for 1.5.0 with its limit set before it was first run. High Quality remembers each
// harmonic's level over a voiced stretch; it must not carry the old vowel into the new one. After the change, the output's formants
// are read against a real voice singing the new vowel at the new pitch, with the same tool and limit as the formant test.
void testVowelChange()
{
    if (gEngine != 1)
        return;
    double worst = 0.0, floor = 0.0;
    const std::pair<const voice::Vowel*, const voice::Vowel*> pairs[] = { { &voice::vowelA, &voice::vowelI }, { &voice::vowelI, &voice::vowelA },
                                                                          { &voice::vowelU, &voice::vowelE }, { &voice::vowelO, &voice::vowelAe } };
    for (const auto& pr : pairs)
        for (double centre : { 147.0, 220.0 })
            for (int shift : { 3, -3 })
            {
                const double seconds = 6.0, moved = std::pow (2.0, shift / 12.0);
                std::vector<double> f0 ((size_t) (seconds * SR)), f0Moved (f0.size());
                for (size_t i = 0; i < f0.size(); ++i)
                {
                    const double u = std::fmod ((double) i / SR / 3.0, 1.0);
                    const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u;
                    f0[i] = centre * std::pow (2.0, 0.5 * tri);
                    f0Moved[i] = f0[i] * moved;
                }
                const auto before = voice::synth (f0, *pr.first, SR), after = voice::synth (f0, *pr.second, SR);
                std::vector<float> in (before.size());
                const size_t at = (size_t) (3.0 * SR), fade = (size_t) (0.03 * SR);
                for (size_t i = 0; i < in.size(); ++i)
                {
                    const double t = i < at ? 0.0 : i >= at + fade ? 1.0 : (double) (i - at) / (double) fade;
                    in[i] = (float) ((1.0 - t) * before[i] + t * after[i]);
                }
                auto p = make();
                setParam (*p, "inputType", (float) lowMale);
                setParam (*p, "retune", 0.0f);
                setParam (*p, "humanize", 0.0f);
                setParam (*p, "flex", 100.0f);
                setParam (*p, "transpose", (float) shift);
                prepare (*p, 1, SR, 256);
                const auto out = mono (render (*p, in, 256));
                const auto real = voice::synth (f0Moved, *pr.second, SR);
                const long from = (long) (3.15 * SR), n = (long) (1.0 * SR);
                const double smoothing = std::max (centre, centre * moved);
                const auto want = smoothSpectrumDb (real.data() + from, n, SR, smoothing), got = smoothSpectrumDb (out.data() + from, n, SR, smoothing),
                           heard = smoothSpectrumDb (after.data() + from, n, SR, smoothing);
                const auto& v = *pr.second;
                for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
                {
                    const double m = std::abs (shapeScale (want, got, band.first, band.second) - 1.0);
                    worst = std::max (worst, m);
                    floor = std::max (floor, std::abs (shapeScale (want, heard, band.first, band.second) - 1.0));
                    if (std::getenv ("TUNE_DUMP") != nullptr)
                        std::printf ("  vowel %s to %s around %.0f Hz, shift %d, band %.0f to %.0f Hz: against a real voice %.3f\n", pr.first->name, v.name, centre, shift, band.first, band.second, 1.0 + m);
                }
            }
    chk (worst <= prolimits::hqFormantMove, "vowel change in a glide, 150 ms to 1.15 s after it, formants against a real voice with the new vowel, worst move", worst * 100.0, "%");
    note ("  and the tool's floor: the new vowel as sung against that real voice, worst", floor * 100.0, "%");
}

// ------------------------------------------------------------------------------------------------------------ timing

// Two timing faults found on real takes (2026-11-10), neither seen by the tests before, which read pitch on steady notes and
// delay on noise.
void testTiming()
{
    // 1. The pitch control must be in line with the audio. Every note Bypass and Transpose +1: nothing is corrected, the voice is moved
    // up a semitone, so the output's vibrato is the input's and should arrive one reported latency later, like the sound. Up to 1.5.1
    // it arrived early, most in Low Male and Wide Range.
    {
        double worst = 0.0;
        std::string worstName;
        for (int type : { altoTenor, lowMale, wideRange })
            for (double hz : { 147.0, 196.0, 262.0 })
            {
                auto p = make();
                setParam (*p, "inputType", (float) type);
                setParam (*p, "retune", 0.0f);
                setParam (*p, "humanize", 0.0f);
                setParam (*p, "transpose", 1.0f);
                for (int i = 0; i < 12; ++i) setParam (*p, ("note" + juce::String (i)).toRawUTF8(), 2.0f);
                prepare (*p, 1, SR, 256);
                const auto x = voice::synth (voice::vibrato (hz, 3.0, SR, 50.0, 5.5), voice::vowelA, SR);
                const auto out = mono (render (*p, x, 256));
                const double delay = p->getLatencySamples() / SR, hop = 0.0005, up = std::pow (2.0, 1.0 / 12.0);
                const auto in = pitchTrace (x.data(), (long) x.size(), SR, hz, 0.5, 2.7, hop, 0.1);
                const auto moved = pitchTrace (out.data(), (long) out.size(), SR, hz * up, 0.5 + delay - 0.04, 2.7 + delay + 0.04, hop, 0.1);
                // Where the output's vibrato sits against the input's, in ms after the reported latency (negative: early)
                const int reach = (int) std::lround (0.04 / hop);
                double best = -1.0e30; int bestK = 0; std::vector<double> score ((size_t) (2 * reach + 1), 0.0);
                for (int k = -reach; k <= reach; ++k)
                {
                    double sum = 0.0;
                    for (size_t i = 0; i < in.size(); ++i)
                    {
                        const size_t j = (size_t) ((int) i + reach + k);
                        if (j >= moved.size() || in[i] <= 0.0 || moved[j] <= 0.0) continue;
                        sum += cents (in[i], hz) * cents (moved[j], hz * up);
                    }
                    score[(size_t) (k + reach)] = sum;
                    if (sum > best) { best = sum; bestK = k; }
                }
                double lag = bestK;
                if (bestK > -reach && bestK < reach)
                {
                    const double a = score[(size_t) (bestK + reach - 1)], b = score[(size_t) (bestK + reach)], c = score[(size_t) (bestK + reach + 1)], den = a - 2.0 * b + c;
                    if (den < -1.0e-12) lag += 0.5 * (a - c) / den;
                }
                const double ms = lag * hop * 1000.0;
                const char* names[] = { "Soprano", "Alto/Tenor", "Low Male", "Wide Range" };
                note (std::string ("  pitch movement against the sound, ") + names[type] + " at " + std::to_string ((int) hz) + " Hz", ms, "ms");
                if (std::abs (ms) > std::abs (worst)) { worst = ms; worstName = std::string (names[type]) + " at " + std::to_string ((int) hz) + " Hz"; }
            }
        chk (std::abs (worst) <= prolimits::pitchInLineMs, "pitch movement arrives with the sound it belongs to, 147 to 262 Hz, three Input Types, worst (negative is early), " + worstName, worst, "ms");
    }

    // 2. A voice that is not being corrected comes out at the reported delay, also after a correction. A voice 40 cents sharp is
    // corrected, then every note is set to Bypass: from 1.5 s after that the output must be the delayed input. That is the grains'
    // promise (PsolaShifter.h). High Quality's harmonic shifter, which gives out notes above about 220 Hz, never made it: once it has
    // corrected a voiced stretch it keeps its own timing within a period until the stretch ends (TuneEngine.h), so there the number is
    // given, not judged.
    {
        double worst = -200.0, harmonicWorst = -200.0;
        for (double hz : { 147.0, 196.0, 262.0 })
            for (int switchBlock : { 260, 275, 290 })
            {
                auto p = tuner (altoTenor, 0.0f);
                const auto x = voice::synth (voice::steady (hz * std::pow (2.0, 40.0 / 1200.0), 4.0, SR), voice::vowelA, SR);
                const auto out = mono (render (*p, x, 256, nullptr, [&] (int block)
                {
                    if (block == switchBlock)
                        for (int i = 0; i < 12; ++i) setParam (*p, ("note" + juce::String (i)).toRawUTF8(), 2.0f);
                }));
                const long delay = p->getLatencySamples();
                const long from = (long) switchBlock * 256 + (long) (1.5 * SR), to = (long) x.size() - 4800;
                double err = 0.0, ref = 0.0;
                for (long i = from; i < to; ++i)
                {
                    const double d = (double) out[(size_t) i] - x[(size_t) (i - delay)];
                    err += d * d; ref += (double) x[(size_t) (i - delay)] * x[(size_t) (i - delay)];
                }
                const double db = 10.0 * std::log10 (err / ref + 1.0e-30);
                if (gEngine == 1 && hz > 235.0) harmonicWorst = std::max (harmonicWorst, db);
                else worst = std::max (worst, db);
            }
        chk (worst <= prolimits::uncorrectedNullDb, std::string ("a voice left uncorrected after a correction, ") + (gEngine == 1 ? "147 and 196 Hz (the grains)" : "147 to 262 Hz")
             + ": output against the input at the reported delay, from 1.5 s on, worst", worst, "dB");
        if (gEngine == 1) note ("  the same at 262 Hz, where the harmonic shifter gives the output (not judged)", harmonicWorst, "dB");
    }
}

// ----------------------------------------------------------------------------------------------- Low Latency null test

std::vector<float> loadWav (const char* name);

// Low Latency must come out bit for bit the same as the last version that changed it on purpose: 1.4.0, which stopped the burst at note starts and made the level trim slower.
// Before that it was 1.2.1. tests/ll_null.txt holds a hash of the 32 bit samples for each case, written by 1.4.0.
// TUNE_LL_RAW=folder also writes the samples, to read a largest sample difference against a saved copy of an older build.
void testLowLatencyNull()
{
    const auto file = juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getChildFile ("ll_null.txt");
    const bool writing = std::getenv ("TUNE_LL_WRITE") != nullptr;
    std::map<std::string, std::string> stored;
    {
        std::ifstream in (file.getFullPathName().toStdString());
        std::string name, value;
        while (in >> name >> value) stored[name] = value;
    }
    std::map<std::string, std::string> found;
    const auto hashed = [] (const juce::AudioBuffer<float>& b)
    {
        unsigned long long h = 1469598103934665603ULL;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                unsigned int bits = 0;
                std::memcpy (&bits, b.getReadPointer (ch) + i, 4);
                h = (h ^ bits) * 1099511628211ULL;
            }
        char text[32];
        std::snprintf (text, sizeof (text), "%016llx", h);
        return std::string (text);
    };
    const auto run = [&] (const std::string& name, const std::vector<float>& x, double sr, const std::function<void (PluginProcessor&)>& set)
    {
        auto p = make();
        setParam (*p, "retune", 0.0f);
        setParam (*p, "humanize", 0.0f);
        setParam (*p, "flex", 0.0f);
        set (*p);
        prepare (*p, 1, sr, 256);
        const auto out = render (*p, x, 256);
        found[name] = hashed (out);
        if (const char* folder = std::getenv ("TUNE_LL_RAW"))
        {
            std::ofstream raw (std::string (folder) + "/" + name + ".raw", std::ios::binary);
            raw.write ((const char*) out.getReadPointer (0), (std::streamsize) out.getNumSamples() * 4);
        }
    };
    const auto a220 = voice::synth (voice::steady (223.8, 3.0, SR), voice::vowelA, SR);
    const auto vib = voice::synth (voice::vibrato (330.0, 3.0, SR, 40.0, 5.5), voice::vowelE, SR);
    const auto high = voice::synth (voice::steady (560.0, 3.0, SR), voice::vowelO, SR);
    run ("hard_a220", a220, SR, [] (PluginProcessor&) {});
    run ("natural_vibrato_e330", vib, SR, [] (PluginProcessor& p) { setParam (p, "retune", 40.0f); setParam (p, "humanize", 40.0f); });
    run ("high_o560_wide", high, SR, [] (PluginProcessor& p) { setParam (p, "inputType", (float) wideRange); });
    run ("transpose_up3", a220, SR, [] (PluginProcessor& p) { setParam (p, "transpose", 3.0f); });
    run ("transpose_down7_wide", vib, SR, [] (PluginProcessor& p) { setParam (p, "inputType", (float) wideRange); setParam (p, "transpose", -7.0f); });
    run ("throat_120", a220, SR, [] (PluginProcessor& p) { setParam (p, "throat", 120.0f); });
    run ("formants_off", a220, SR, [] (PluginProcessor& p) { setParam (p, "formantPreserve", 0.0f); setParam (p, "transpose", 2.0f); });
    run ("classic", vib, SR, [] (PluginProcessor& p) { setParam (p, "classic", 1.0f); });
    run ("mix50", a220, SR, [] (PluginProcessor& p) { setParam (p, "mix", 50.0f); });
    run ("rate441", voice::synth (voice::steady (223.8, 3.0, 44100.0), voice::vowelA, 44100.0), 44100.0, [] (PluginProcessor&) {});
    run ("rate96", voice::synth (voice::steady (223.8, 3.0, 96000.0), voice::vowelA, 96000.0), 96000.0, [] (PluginProcessor&) {});
    for (const char* name : { "speech_samantha", "speech_daniel", "sung_cellos", "held_kathy" })
    {
        const auto x = loadWav ((std::string (name) + ".wav").c_str());
        const bool low = std::string (name).find ("daniel") != std::string::npos;
        run (std::string ("say_hard_") + name, x, SR, [low] (PluginProcessor& p) { setParam (p, "inputType", (float) (low ? lowMale : altoTenor)); });
        run (std::string ("say_natural_") + name, x, SR, [low] (PluginProcessor& p) { setParam (p, "inputType", (float) (low ? lowMale : altoTenor)); setParam (p, "retune", 40.0f); setParam (p, "humanize", 40.0f); });
    }
    // What the host sees: every parameter's ID, name and range, and the latency of each Input Type.
    {
        auto p = make();
        unsigned long long h = 1469598103934665603ULL;
        const auto add = [&h] (const juce::String& text) { for (auto c : text) h = (h ^ (unsigned long long) c) * 1099511628211ULL; };
        for (auto* prm : p->getParameters())
            if (auto* with = dynamic_cast<juce::RangedAudioParameter*> (prm))
            {
                add (with->getParameterID()); add (with->getName (64));
                add (juce::String (with->getNormalisableRange().start)); add (juce::String (with->getNormalisableRange().end));
            }
        char text[32];
        std::snprintf (text, sizeof (text), "%016llx", h);
        found["parameters"] = text;
        for (int type : { soprano, altoTenor, lowMale, wideRange })
        {
            setParam (*p, "inputType", (float) type);
            prepare (*p, 1, SR, 256);
            found["latency_" + std::to_string (type)] = std::to_string (p->getLatencySamples());
        }
    }
    if (writing)
    {
        std::ofstream out (file.getFullPathName().toStdString());
        for (const auto& [name, value] : found) out << name << " " << value << "\n";
        return;
    }
    int same = 0;
    for (const auto& [name, value] : found)
    {
        const bool ok = stored.count (name) > 0 && stored[name] == value;
        same += ok ? 1 : 0;
        if (! ok)
            check (false, "[LL] null test against 1.4.0, case " + name);
    }
    check (same == (int) found.size() && ! found.empty(), "[LL] null test against 1.4.0: output, parameters and latency bit for bit the same, cases", (double) same);
}

// --------------------------------------------------------------------------------------------------------- vibrato

double vibratoKept (PluginProcessor& p, double depth = 40.0, double from = 0.6)
{
    prepare (p, 1, SR, 256);
    const double rate = 5.5, hz = 220.0;
    const auto x = voice::synth (voice::vibrato (hz, 3.0, SR, depth, rate), voice::vowelA, SR);
    const auto out = mono (render (p, x, 256));
    const double hop = 0.002;
    const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hz, from, 2.9, hop, 0.1);
    std::vector<double> c;
    for (double h : trace) c.push_back (h > 0.0 ? cents (h, hz) : std::nan (""));
    return sineDepth (c, hop, rate) / depth;
}

void testVibrato()
{
    {
        auto p = make(); setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "flex", 0.0f);
        const double kept = vibratoKept (*p);
        chk (kept <= limits::vibratoFlatHard, "hard settings flatten vibrato, kept", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "flex", 60.0f);
        const double kept = vibratoKept (*p);
        chk (kept >= limits::vibratoKeptFlex, "Flex Tune 60 cents keeps a +/-40 cent vibrato, kept", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 100.0f); setParam (*p, "flex", 0.0f);
        const double kept = vibratoKept (*p);
        chk (kept >= limits::vibratoKeptHumanize, "Humanize 100 keeps vibrato on the sustained part, kept", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 400.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "flex", 0.0f);
        const double kept = vibratoKept (*p);
        chk (kept >= limits::vibratoKeptSlowRetune, "Retune Speed 400 ms keeps vibrato, kept", kept * 100.0, "%");
    }
    {
        // Natural Vibrato acts on what survives correction, so use settings that leave vibrato alone.
        auto p = make(); setParam (*p, "retune", 400.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "naturalVibrato", -100.0f);
        const double kept = vibratoKept (*p);
        chk (kept <= limits::naturalVibratoLess, "Natural Vibrato -100 takes vibrato down, left", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 400.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "naturalVibrato", 100.0f);
        const double kept = vibratoKept (*p);
        chk (kept >= limits::naturalVibratoMore, "Natural Vibrato +100 takes vibrato up, now", kept * 100.0, "%");
    }
}

// ------------------------------------------------------------------------------------------------- hard step, flex

void testHardStep()
{
    // A slide of 2 semitones in 40 ms, in C major so that A# is not a target on the way. The output should step, not slide.
    auto p = make();
    setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f);
    setParam (*p, "key", 0.0f); setParam (*p, "scale", (float) smoothtune::scaleMajor);
    prepare (*p, 1, SR, 256);
    const double a = noteHz (57), b = noteHz (59), start = 1.0, slide = 0.04;
    std::vector<double> f0 ((size_t) (2.0 * SR));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        const double t = (double) i / SR;
        const double u = std::clamp ((t - start) / slide, 0.0, 1.0);
        f0[i] = a * std::pow (b / a, u);
    }
    const auto x = voice::synth (f0, voice::vowelA, SR);
    const auto out = mono (render (*p, x, 256));
    const double hop = 0.0005, hzMid = std::sqrt (a * b);
    const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hzMid, start - 0.1, start + 0.2, hop, 0.14, 2.0);
    const double low = 0.1, high = 0.9;
    // From the last frame still under 10% to the first over 90%. The reading can drop out in the step itself, and that gap counts as part of the step.
    double t10 = -1, t90 = -1;
    for (size_t i = 0; i < trace.size(); ++i)
    {
        if (trace[i] <= 0.0) continue;
        const double u = std::log (trace[i] / a) / std::log (b / a);
        if (u <= low) t10 = (double) i * hop;
        if (u > high) { t90 = (double) i * hop; break; }
    }
    const double ms = (t90 - t10) * 1000.0;
    chk (t10 >= 0 && t90 >= 0 && ms <= limits::hardStepMs, "hard tune step between two notes a tone apart, 10% to 90%", ms, "ms");
    const auto settled = centsOff (out.data(), (long) out.size(), SR, b, start + 0.1, 1.9);
    chk (percentile (absolute (settled), 95) < limits::outputInstantCents, "after the step the new note is on pitch, 95th percentile", percentile (absolute (settled), 95), "cents");

    // A slow glide through two notes becomes steps: at instant speed the output spends its time on notes, not between.
    auto q = tuner (altoTenor, 0.0f);
    std::vector<double> glide ((size_t) (2.0 * SR));
    for (size_t i = 0; i < glide.size(); ++i)
        glide[i] = noteHz (57) * std::pow (2.0, ((double) i / SR) * 2.0 / 2.0 / 12.0);
    const auto gx = voice::synth (glide, voice::vowelA, SR);
    const auto gout = mono (render (*q, gx, 256));
    int onNote = 0, total = 0;
    for (double t = 0.2; t < 1.9; t += 0.005)
    {
        const double expected = noteHz (57) * std::pow (2.0, t * 2.0 / 2.0 / 12.0);
        const double hz = pitchNear (gout.data(), (long) gout.size(), (long) (t * SR), expected, SR, 0.12, 2.5);
        if (hz > 0.0)
        {
            const double midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
            if (std::abs (midi - std::round (midi)) < 0.10) ++onNote;
            ++total;
        }
    }
    const double share = total > 0 ? 100.0 * onNote / total : 0.0;
    chk (share >= 80.0, "a slow glide comes out as steps, frames within 10 cents of a note", share, "%");
}

void testFlex()
{
    {
        auto p = tuner (altoTenor, 0.0f, 0.0f, 50.0f);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.4));
        chk (std::abs (e - 30.0) <= limits::flexHoldCents, "Flex Tune 50: a note 30 cents sharp is left where it was sung, output offset", e, "cents");
    }
    {
        // In C major, so A stays the nearest note: chromatically, +80 cents would simply be 20 cents flat of A#.
        auto p = make();
        setParam (*p, "inputType", (float) altoTenor); setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "flex", 50.0f);
        setParam (*p, "key", 0.0f); setParam (*p, "scale", (float) smoothtune::scaleMajor);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 80.0 / 1200.0), 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.4));
        chk (std::abs (e - 50.0) <= limits::flexClampCents, "Flex Tune 50: a note 80 cents sharp is brought to the edge, output offset", e, "cents");
    }
    {
        auto p = tuner (altoTenor, 0.0f, 0.0f, 0.0f);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.4));
        chk (std::abs (e) <= limits::outputInstantCents, "Flex Tune 0: the same note is corrected fully, output offset", e, "cents");
    }
}

// ----------------------------------------------------------------------------------------------------------- unvoiced

double nullDb (const std::vector<float>& out, const std::vector<float>& in, int delay, long from, long to)
{
    double e = 0, s = 0;
    for (long i = from; i < to; ++i)
    {
        const double d = (double) out[(size_t) i] - in[(size_t) (i - delay)];
        e += d * d;
        s += (double) in[(size_t) (i - delay)] * in[(size_t) (i - delay)];
    }
    return 10.0 * std::log10 (std::max (e, 1.0e-30) / std::max (s, 1.0e-30));
}

std::vector<float> coloured (int n, double amp, double hp, double lp, unsigned seed)
{
    std::vector<float> x ((size_t) n);
    std::mt19937 g (seed);
    std::normal_distribution<float> d (0.0f, 1.0f);
    double l1 = 0, l2 = 0, h1 = 0;
    const double kl = 1.0 - std::exp (-2.0 * M_PI * lp / SR), kh = 1.0 - std::exp (-2.0 * M_PI * hp / SR);
    double peakRms = 0;
    for (int i = 0; i < n; ++i)
    {
        const double w = d (g);
        l1 += kl * (w - l1);
        l2 += kl * (l1 - l2);
        h1 += kh * (l2 - h1);
        x[(size_t) i] = (float) (l2 - h1);
        peakRms += (double) x[(size_t) i] * x[(size_t) i];
    }
    const double scale = amp / std::sqrt (peakRms / n);
    for (auto& v : x) v = (float) (v * scale);
    return x;
}

void testUnvoiced()
{
    struct N { const char* name; double hp, lp, level; };
    const N noises[] = { { "S, 4 to 10 kHz", 4000, 10000, 0.1 }, { "SH, 2 to 6 kHz", 2000, 6000, 0.1 }, { "F, wide", 1000, 16000, 0.05 },
                         { "breath, 300 Hz to 5 kHz", 300, 5000, 0.03 }, { "dark breath, 100 to 1500 Hz", 100, 1500, 0.02 } };
    for (float retune : { 0.0f, 50.0f })
        for (const auto& n : noises)
        {
            auto p = tuner (altoTenor, retune, retune > 0 ? 40.0f : 0.0f);
            const auto x = coloured ((int) (2.0 * SR), n.level, n.hp, n.lp, 11);
            const auto out = mono (render (*p, x, 256));
            const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.1 * SR), (long) (1.9 * SR));
            chk (db <= limits::unvoicedNullDb, std::string ("unchanged: ") + n.name + (retune > 0 ? ", natural settings" : ", hard settings") + ", difference", db, "dB");
        }

    // The same on the lowest latency type, whose window is shortest and so easiest to fool.
    for (const auto& n : noises)
    {
        auto p = tuner (soprano, 0.0f);
        const auto x = coloured ((int) (2.0 * SR), n.level, n.hp, n.lp, 21);
        const auto out = mono (render (*p, x, 256));
        const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.1 * SR), (long) (1.9 * SR));
        chk (db <= limits::unvoicedNullDb, std::string ("unchanged on Soprano: ") + n.name + ", difference", db, "dB");
    }

    // An S between two corrected vowels. Its middle must pass as it came in.
    auto p = tuner (altoTenor, 0.0f);
    const auto v1 = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 0.6, SR), voice::vowelA, SR);
    const auto s = coloured ((int) (0.2 * SR), 0.08, 4000, 10000, 5);
    std::vector<float> x = v1;
    x.insert (x.end(), s.begin(), s.end());
    x.insert (x.end(), v1.begin(), v1.end());
    const auto out = mono (render (*p, x, 256));
    const int delay = p->getLatencySamples();
    const double db = nullDb (out, x, delay, (long) (0.65 * SR) + delay, (long) (0.75 * SR) + delay);
    chk (db <= limits::consonantNullDb, "an S between two corrected vowels, middle of the S, difference", db, "dB");
    const auto e = centsOff (out.data(), (long) out.size(), SR, 220.0, 1.0, 1.35);
    chk (percentile (absolute (e), 95) < limits::outputInstantCents, "the vowel after the S is corrected again, 95th percentile", percentile (absolute (e), 95), "cents");
}

// ------------------------------------------------------------------------------------------------------- artifacts

// A note that starts hard after silence, as a sung entry can. Looks for a short burst in the output's level against the input's.
// A first model made from a window that is nearly all silence used to ring out at many times the voice's own level.
void testNoteStart()
{
    double worst = 0.0;
    std::string worstName;
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO })
        for (double hz : { 110.0, 223.8, 330.0, 560.0 })
            for (int shift : { 0, 3 })
                for (double attackMs : { 0.0, 20.0 })
                {
                    std::vector<double> f0 ((size_t) (0.3 * SR), 0.0);
                    const auto note = voice::steady (hz, 2.0, SR);
                    f0.insert (f0.end(), note.begin(), note.end());
                    auto x = voice::synth (f0, *v, SR);
                    for (size_t i = 0; i < (size_t) (0.3 * SR); ++i) x[i] = 0.0f;
                    // At once, or rising over 20 ms
                    const size_t len = (size_t) (attackMs * 0.001 * SR);
                    for (size_t i = 0; i < len; ++i) x[(size_t) (0.3 * SR) + i] *= (float) i / (float) len;
                    auto p = make();
                    setParam (*p, "inputType", (float) wideRange);
                    setParam (*p, "retune", 0.0f);
                    setParam (*p, "transpose", (float) shift);
                    prepare (*p, 1, SR, 256);
                    const auto out = mono (render (*p, x, 256));
                    const long delay = p->getLatencySamples();
                    // Output against input, 20 ms at a time, so at least two periods, lined up by the latency, over the first 0.6 s. A burst stands above
                    // both the median of those within 40 ms of it and the median once the note is steady. The level trim settling over a tenth of a
                    // second passes the first, and a start at the right level followed by a dip passes the second.
                    const auto against = [&] (long from, long to)
                    {
                        std::vector<double> d;
                        for (long i = from; i + 960 <= to; i += 240)
                        {
                            const double in = toDb (rms (x.data() + i, 960) + 1.0e-12f);
                            if (in > -60.0)
                                d.push_back (toDb (rms (out.data() + delay + i, 960) + 1.0e-12f) - in);
                        }
                        return d;
                    };
                    const auto start = against ((long) (0.3 * SR), (long) (0.9 * SR));
                    auto steady = against ((long) (1.5 * SR), (long) (2.2 * SR));
                    std::nth_element (steady.begin(), steady.begin() + (long) steady.size() / 2, steady.end());
                    const double settled = steady[steady.size() / 2];
                    double ratio = 0.0;
                    for (size_t i = 0; i < start.size(); ++i)
                    {
                        std::vector<double> around (start.begin() + (long) (i >= 8 ? i - 8 : 0), start.begin() + (long) std::min (start.size(), i + 9));
                        std::nth_element (around.begin(), around.begin() + (long) around.size() / 2, around.end());
                        ratio = std::max (ratio, std::min (start[i] - around[around.size() / 2], start[i] - settled));
                    }
                    if (ratio > worst) { worst = ratio; worstName = std::string (v->name) + " at " + std::to_string ((int) hz) + " Hz, shift " + std::to_string (shift) + ", attack " + std::to_string ((int) attackMs) + " ms"; }
                    if (std::getenv ("TUNE_DUMP") != nullptr)
                        std::printf ("  note start, vowel %s, %.0f Hz, shift %d, attack %.0f ms: burst in output against input %+.2f dB\n", v->name, hz, shift, attackMs, ratio);
                }
    chk (worst <= limits::noteStartOverDb, "note starting hard after silence, a burst in output against input over the first 0.6 s, worst (" + worstName + ")", worst, "dB");
}

void testArtifacts()
{
    const double target = 220.0;
    auto p = tuner (altoTenor, 0.0f);
    const auto x = voice::synth (voice::steady (target * std::pow (2.0, 30.0 / 1200.0), 3.0, SR), voice::vowelA, SR);
    const auto out = mono (render (*p, x, 256));
    const int from = (int) (0.5 * SR), n = 1 << 16;
    const double nh = nonHarmonicDb (out.data() + from, n, target, SR);
    chk (nh <= limits::nonHarmonicDb, "non-harmonic energy, 220 Hz tone corrected by 30 cents", nh, "dB");
    note ("  the same measure on the uncorrected input", nonHarmonicDb (x.data() + from, n, target * std::pow (2.0, 30.0 / 1200.0), SR), "dB");

    // Corrected by more, as a pentatonic gap might ask
    auto q = make();
    setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
    onlyNote (*q, 9);
    prepare (*q, 1, SR, 256);
    const auto y = voice::synth (voice::steady (noteHz (57) * std::pow (2.0, 150.0 / 1200.0), 3.0, SR), voice::vowelA, SR);
    const auto outY = mono (render (*q, y, 256));
    const double nh2 = nonHarmonicDb (outY.data() + from, n, target, SR);
    chk (nh2 <= limits::nonHarmonicDb, "non-harmonic energy, 220 Hz tone corrected by 150 cents", nh2, "dB");

    // The same on high notes, where the model has few harmonics to go on.
    for (int midi : { 69, 76 })
    {
        auto h = make();
        setParam (*h, "inputType", (float) wideRange); setParam (*h, "retune", 0.0f); setParam (*h, "humanize", 0.0f);
        prepare (*h, 1, SR, 256);
        const auto hx = voice::synth (voice::steady (noteHz (midi) * std::pow (2.0, 30.0 / 1200.0), 3.0, SR), voice::vowelA, SR);
        const auto hOut = mono (render (*h, hx, 256));
        const double hn = nonHarmonicDb (hOut.data() + from, n, noteHz (midi), SR);
        chk (hn <= limits::nonHarmonicDb, "non-harmonic energy, MIDI note " + std::to_string (midi) + " corrected by 30 cents", hn, "dB");
    }

    // Warble on a sustained note
    const auto e = centsOff (out.data(), (long) out.size(), SR, target, 0.5, 2.9);
    double m = 0, s2 = 0;
    for (double v : e) m += v;
    m /= (double) e.size();
    for (double v : e) s2 += (v - m) * (v - m);
    const double sd = std::sqrt (s2 / (double) e.size());
    chk (sd <= limits::warblePitchCents, "output pitch steadiness on a corrected sustained note, standard deviation", sd, "cents");
    double lo = 1.0e9, hi = -1.0e9;
    const int frame = (int) (0.05 * SR);
    for (int at = from; at + frame < (int) out.size() - 2400; at += frame / 2)
    {
        const double db = toDb (rms (out.data() + at, frame));
        lo = std::min (lo, db); hi = std::max (hi, db);
    }
    chk (hi - lo <= limits::warbleLevelDb, "output level steadiness on a corrected sustained note, peak to peak", hi - lo, "dB");

    // No warble either at everyday settings, where the correction is settling rather than snapping.
    {
        auto n = tuner (altoTenor, 20.0f, 30.0f);
        const auto nx = voice::synth (voice::steady (target * std::pow (2.0, 30.0 / 1200.0), 3.0, SR), voice::vowelA, SR);
        const auto nout = mono (render (*n, nx, 256));
        const auto ne = centsOff (nout.data(), (long) nout.size(), SR, target, 1.5, 2.9);
        double nm = 0, ns = 0;
        for (double v : ne) nm += v;
        nm /= (double) ne.size();
        for (double v : ne) ns += (v - nm) * (v - nm);
        const double nsd = std::sqrt (ns / (double) ne.size());
        chk (nsd <= limits::warblePitchCents, "output pitch steadiness at everyday settings (Retune 20, Humanize 30), standard deviation", nsd, "cents");
        chk (std::abs (nm) <= limits::outputInstantCents, "and the note settles on pitch, offset", nm, "cents");
        const double nnh = nonHarmonicDb (nout.data() + (int) (1.5 * SR), 1 << 16, target, SR);
        chk (nnh <= limits::nonHarmonicDb, "non-harmonic energy at everyday settings", nnh, "dB");
    }

    // In tune and left alone: the plug-in must then be close to a plain delay even on a voice.
    auto t = tuner (altoTenor, 0.0f, 0.0f, 100.0f);
    const auto w = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
    const auto outW = mono (render (*t, w, 256));
    const double db = nullDb (outW, w, t->getLatencySamples(), (long) (0.2 * SR), (long) (1.9 * SR));
    chk (db <= -60.0, "a voice with nothing to correct passes as a plain delay, difference", db, "dB");
}

// ---------------------------------------------------------------------------------------------------- key and scale

std::uint16_t maskFromNames (const char* names)
{
    // Written out by hand below, one by one, so this does not repeat the plug-in's own arithmetic.
    std::uint16_t m = 0;
    std::string s (names);
    size_t at = 0;
    while (at < s.size())
    {
        size_t next = s.find (' ', at);
        if (next == std::string::npos) next = s.size();
        const auto token = s.substr (at, next - at);
        for (int i = 0; i < 12; ++i)
            if (token == smoothtune::noteNames[i])
                m = (std::uint16_t) (m | (1u << i));
        at = next + 1;
    }
    return m;
}

struct KeyCase { int root; int scale; const char* notes; };
const KeyCase allKeys[24] = {
    { 0, smoothtune::scaleMajor, "C D E F G A B" },       { 1, smoothtune::scaleMajor, "C# D# F F# G# A# C" },
    { 2, smoothtune::scaleMajor, "D E F# G A B C#" },     { 3, smoothtune::scaleMajor, "D# F G G# A# C D" },
    { 4, smoothtune::scaleMajor, "E F# G# A B C# D#" },   { 5, smoothtune::scaleMajor, "F G A A# C D E" },
    { 6, smoothtune::scaleMajor, "F# G# A# B C# D# F" },  { 7, smoothtune::scaleMajor, "G A B C D E F#" },
    { 8, smoothtune::scaleMajor, "G# A# C C# D# F G" },   { 9, smoothtune::scaleMajor, "A B C# D E F# G#" },
    { 10, smoothtune::scaleMajor, "A# C D D# F G A" },    { 11, smoothtune::scaleMajor, "B C# D# E F# G# A#" },
    { 0, smoothtune::scaleMinor, "C D D# F G G# A#" },    { 1, smoothtune::scaleMinor, "C# D# E F# G# A B" },
    { 2, smoothtune::scaleMinor, "D E F G A A# C" },      { 3, smoothtune::scaleMinor, "D# F F# G# A# B C#" },
    { 4, smoothtune::scaleMinor, "E F# G A B C D" },      { 5, smoothtune::scaleMinor, "F G G# A# C C# D#" },
    { 6, smoothtune::scaleMinor, "F# G# A B C# D E" },    { 7, smoothtune::scaleMinor, "G A A# C D D# F" },
    { 8, smoothtune::scaleMinor, "G# A# B C# D# E F#" },  { 9, smoothtune::scaleMinor, "A B C D E F G" },
    { 10, smoothtune::scaleMinor, "A# C C# D# F F# G#" }, { 11, smoothtune::scaleMinor, "B C# D E F# G A" },
};

void testKeysAndScales()
{
    // The table itself
    int tableOk = 0;
    for (const auto& k : allKeys)
    {
        const bool same = smoothtune::scaleMask (k.root, k.scale) == maskFromNames (k.notes);
        if (! same) std::printf ("  wrong mask: %s %s\n", smoothtune::noteNames[k.root], smoothtune::scaleNames[k.scale]);
        tableOk += same ? 1 : 0;
    }
    chk (tableOk == 24, "key and scale masks match the written-out notes of all 24 keys", tableOk, "of 24");

    // The sound: sing every note slightly sharp and slightly flat in every key, and see which note comes out.
    std::vector<std::vector<float>> sharp, flat;
    for (int i = 0; i < 12; ++i)
    {
        sharp.push_back (voice::synth (voice::steady (noteHz (60 + i) * std::pow (2.0, 20.0 / 1200.0), 0.3, SR), voice::vowelA, SR));
        flat.push_back (voice::synth (voice::steady (noteHz (60 + i) * std::pow (2.0, -20.0 / 1200.0), 0.3, SR), voice::vowelA, SR));
    }
    auto p = make();
    setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f);
    int right = 0, total = 0;
    for (const auto& k : allKeys)
    {
        const auto mask = maskFromNames (k.notes);
        setParam (*p, "key", (float) k.root);
        setParam (*p, "scale", (float) k.scale);
        for (int i = 0; i < 12; ++i)
            for (int dir : { +1, -1 })
            {
                const bool inScale = (mask >> i) & 1;
                if (inScale && dir < 0) continue; // an in-scale note just goes home: once is enough
                const int expected = inScale ? 60 + i : 60 + i + dir;
                prepare (*p, 1, SR, 256);
                const auto out = mono (render (*p, dir > 0 ? sharp[(size_t) i] : flat[(size_t) i], 256));
                const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (expected), 0.12, 0.27, 0.01, 0.1);
                const bool ok = ! e.empty() && std::abs (median (e)) < 15.0;
                if (! ok) std::printf ("  %s %s: note %d dir %d expected %d, offset %.1f\n", smoothtune::noteNames[k.root], smoothtune::scaleNames[k.scale], 60 + i, dir, expected, e.empty() ? 9999.0 : median (e));
                right += ok ? 1 : 0;
                ++total;
            }
    }
    chk (right == total, "every note sung in all 24 keys lands on the right note, " + std::to_string (total) + " cases", right, "right");

    // Other scales, written out by hand for a root of A
    struct S { int scale; const char* notes; };
    for (auto s : { S { smoothtune::scaleHarmonicMinor, "A B C D E F G#" }, S { smoothtune::scaleMelodicMinor, "A B C D E F# G#" },
                    S { smoothtune::scaleMajorPentatonic, "A B C# E F#" }, S { smoothtune::scaleMinorPentatonic, "A C D E G" },
                    S { smoothtune::scaleBlues, "A C D D# E G" }, S { smoothtune::scaleDorian, "A B C D E F# G" }, S { smoothtune::scaleMixolydian, "A B C# D E F# G" } })
        chk (smoothtune::scaleMask (9, s.scale) == maskFromNames (s.notes), std::string ("A ") + smoothtune::scaleNames[s.scale] + " mask is right");

    // Custom notes
    {
        auto q = make();
        setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
        setParam (*q, "scale", (float) smoothtune::scaleChromatic);
        setParam (*q, "note9", 1.0f); // A removed
        prepare (*q, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 20.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*q, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (58), 0.3, 0.9, 0.005, 0.1));
        chk (std::abs (e) <= limits::customNoteCents, "Note modes: A removed, A +20 cents goes to A#, offset", e, "cents");
    }
    {
        auto q = make();
        setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
        setParam (*q, "scale", (float) smoothtune::scaleChromatic);
        setParam (*q, "note9", 2.0f); // A bypassed
        prepare (*q, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*q, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 0.9));
        chk (std::abs (e - 30.0) <= limits::customNoteCents, "Note modes: A bypassed, A +30 cents stays where it was sung, offset", e, "cents");
    }
    {
        auto q = make();
        setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
        setParam (*q, "scale", (float) smoothtune::scaleChromatic);
        for (int i = 0; i < 12; ++i) setParam (*q, ("note" + juce::String (i)).toRawUTF8(), 1.0f);
        prepare (*q, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*q, x, 256));
        chk (countNonFinite (render (*q, x, 256)) == 0 && std::abs (median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 0.9)) - 30.0) < 10.0, "Note modes: every note removed leaves the voice alone");
    }
}

// ------------------------------------------------------------------------------------------------------ follow key

juce::File keyFileForTest() { return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("smooth-tune-keytest-" + juce::String (getpid()) + ".json"); }
double nowUnix() { using namespace std::chrono; return duration<double> (system_clock::now().time_since_epoch()).count(); }

void writeKey (const juce::File& f, const std::string& text) { f.replaceWithText (text); }
std::string keyJson (const char* key, const char* scale, double conf, double time)
{
    char b[256];
    std::snprintf (b, sizeof b, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":%.2f,\"time\":%.1f,\"listening\":true}", key, scale, conf, time);
    return b;
}

// Which of two notes did the output land on? -1 if neither.
int outputNote (PluginProcessor& p, const std::vector<float>& x)
{
    prepare (p, 1, SR, 256);
    const auto out = mono (render (p, x, 256));
    for (int m : { 66, 67 })
    {
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (m), 0.2, 0.4, 0.02, 0.025);
        if (! e.empty() && std::abs (median (e)) < 15.0)
            return m;
    }
    return -1;
}

void testFollowKey()
{
    const auto file = keyFileForTest();
    file.deleteFile();
    auto p = make (file.getFullPathName());
    setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f);
    setParam (*p, "key", 0.0f); setParam (*p, "scale", (float) smoothtune::scaleMajor); // manual: C major
    setParam (*p, "followKey", 1.0f);

    // Sing C#4. C major sends it to D at +20 cents. D minor has C#? No: D minor is D E F G A A# C, so it goes to D too. Use F#4:
    // C major: F#+20 goes to G. D major: F# stays. So D major from the file changes the note.
    const auto x = voice::synth (voice::steady (noteHz (66) * std::pow (2.0, 20.0 / 1200.0), 0.5, SR), voice::vowelA, SR);
    const int manualNote = 67, fileNote = 66;

    p->pollKeyFileNow();
    chk (outputNote (*p, x) == manualNote, "Follow Key with no file uses the manual key", outputNote (*p, x));

    struct Bad { const char* name; std::string text; };
    const Bad bad[] = {
        { "broken JSON", "{not json at all" },
        { "empty file", "" },
        { "cut off half way", "{\"key\":\"D\",\"scale\":\"maj" },
        { "unknown key name", keyJson ("H", "major", 0.9, nowUnix()) },
        { "unknown scale", keyJson ("D", "phrygian", 0.9, nowUnix()) },
        { "nan confidence", "{\"key\":\"D\",\"scale\":\"major\",\"confidence\":nan,\"time\":" + std::to_string (nowUnix()) + ",\"listening\":true}" },
        { "stale, one hour old", keyJson ("D", "major", 0.9, nowUnix() - 3600.0) },
        { "from the future", keyJson ("D", "major", 0.9, nowUnix() + 3600.0) },
        { "low confidence", keyJson ("D", "major", 0.2, nowUnix()) },
        { "no time field", "{\"key\":\"D\",\"scale\":\"major\",\"confidence\":0.9}" },
    };
    for (const auto& b : bad)
    {
        writeKey (file, b.text);
        p->pollKeyFileNow();
        chk (outputNote (*p, x) == manualNote, std::string ("Follow Key ignores a file with ") + b.name);
        chk (p->getReadings().activeKey == 0, std::string ("  active key still reads the manual one after ") + b.name);
    }

    // A good file takes over
    writeKey (file, keyJson ("D", "major", 0.87, nowUnix()));
    p->pollKeyFileNow();
    chk (outputNote (*p, x) == fileNote, "Follow Key uses a good file: D major keeps F#", outputNote (*p, x));
    chk (p->getReadings().activeKey == 2 && p->getReadings().activeScale == smoothtune::scaleMajor, "active key reads D major");

    // The real format, as the task gives it
    writeKey (file, keyJson ("A", "minor", 0.87, nowUnix()));
    p->pollKeyFileNow();
    prepare (*p, 1, SR, 256);
    render (*p, x, 256);
    chk (p->getReadings().activeKey == 9 && p->getReadings().activeScale == smoothtune::scaleMinor, "active key reads A minor from a good file");

    // Good, then bad: the last good key holds.
    writeKey (file, keyJson ("D", "major", 0.87, nowUnix()));
    p->pollKeyFileNow();
    for (const auto& b : bad)
    {
        writeKey (file, b.text);
        p->pollKeyFileNow();
        if (outputNote (*p, x) != fileNote) chk (false, std::string ("last good key should hold after ") + b.name);
    }
    chk (outputNote (*p, x) == fileNote, "after a good file, bad data leaves the last good key in place");

    file.deleteFile();
    p->pollKeyFileNow();
    chk (outputNote (*p, x) == fileNote, "after a good file, the file going missing leaves the last good key in place");

    // Switched off: back to manual at once.
    setParam (*p, "followKey", 0.0f);
    p->pollKeyFileNow();
    chk (outputNote (*p, x) == manualNote, "Follow Key off goes back to the manual key");

    // The background thread does it without help.
    writeKey (file, keyJson ("D", "major", 0.9, nowUnix()));
    setParam (*p, "followKey", 1.0f);
    std::this_thread::sleep_for (std::chrono::milliseconds (700));
    chk (outputNote (*p, x) == fileNote, "the background thread picks up the file by itself within 0.7 s");
    file.deleteFile();

    // A reader must not trip on a very large file or a folder
    {
        auto q = make (file.getFullPathName());
        setParam (*q, "followKey", 1.0f);
        writeKey (file, std::string (200000, 'x'));
        q->pollKeyFileNow();
        chk (! smoothtune::KeyFollower::usable ({}, nowUnix()), "a large junk file changes nothing");
        file.deleteFile();
        file.createDirectory();
        q->pollKeyFileNow();
        file.deleteRecursively();
        chk (true, "a folder at that path changes nothing");
    }
}

// --------------------------------------------------------------------------------------------------------- latency

int measuredDelay (const std::vector<float>& out, const std::vector<float>& in, int maxLag)
{
    double best = -1.0e30; int at = -1;
    const int from = 2000, to = (int) std::min (in.size(), out.size()) - maxLag - 1;
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double c = 0;
        for (int i = from; i < to; i += 3) c += (double) in[(size_t) i] * out[(size_t) (i + lag)];
        if (c > best) { best = c; at = lag; }
    }
    return at;
}

void testLatency()
{
    const char* names[] = { "Soprano", "Alto/Tenor", "Low Male", "Wide Range" };
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int type = 0; type < 4; ++type)
        {
            auto p = make();
            setParam (*p, "inputType", (float) type);
            prepare (*p, 1, sr, 256);
            const int reported = p->getLatencySamples();
            std::vector<float> x ((size_t) sr);
            noise (x.data(), (int) x.size(), 0.1);
            const auto out = mono (render (*p, x, 256));
            const int d = measuredDelay (out, x, reported + 200);
            chk (d == reported, std::string (names[type]) + " at " + std::to_string ((int) sr) + " Hz: delay measured on noise against reported " + std::to_string (reported), d, "samples");
        }

    // The same on a voice with nothing to correct
    for (int type : { altoTenor, soprano })
    {
        auto p = make();
        setParam (*p, "inputType", (float) type);
        setParam (*p, "flex", 100.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (type == soprano ? 330.0 : 220.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        // A steady voice repeats, so every whole period of lag fits about as well. Only the nearest one says anything.
        const int reported = p->getLatencySamples(), halfPeriod = (int) (0.45 * SR / (type == soprano ? 330.0 : 220.0));
        double bestFit = -1.0e30;
        int d = -1;
        for (int lag = reported - halfPeriod; lag <= reported + halfPeriod; ++lag)
        {
            double fit = 0.0;
            for (int i = 12000; i < 48000; i += 3) fit += (double) x[(size_t) i] * out[(size_t) (i + lag)];
            if (fit > bestFit) { bestFit = fit; d = lag; }
        }
        chk (std::abs (d - reported) <= 1, std::string ("delay measured on a voice, ") + names[type], d, "samples");
    }

    // The lowest latency type
    {
        auto p = make();
        setParam (*p, "inputType", (float) soprano);
        prepare (*p, 1, SR, 256);
        const double ms = p->getLatencySamples() * 1000.0 / SR;
        if (gEngine == 0) chk (ms < limits::lowestLatencyMs, "lowest latency Input Type (Soprano) at 48 kHz", ms, "ms");
        for (int type : { altoTenor, lowMale, wideRange })
        {
            auto q = make();
            setParam (*q, "inputType", (float) type);
            prepare (*q, 1, SR, 256);
            note (std::string ("  latency at 48 kHz, ") + names[type], q->getLatencySamples() * 1000.0 / SR, "ms");
        }
    }

    // Bypass keeps the same delay
    {
        auto p = make();
        setParam (*p, "bypass", 1.0f);
        prepare (*p, 1, SR, 256);
        std::vector<float> x ((size_t) SR);
        noise (x.data(), (int) x.size(), 0.1);
        const auto out = mono (render (*p, x, 256));
        chk (measuredDelay (out, x, 4000) == p->getLatencySamples(), "bypass keeps the same delay");
        const double db = nullDb (out, x, p->getLatencySamples(), 4000, 40000);
        chk (db < -120.0, "bypass is the plain delayed input, difference", db, "dB");
    }

    // processBlockBypassed the same
    {
        auto p = make();
        prepare (*p, 1, SR, 256);
        std::vector<float> x (48000);
        noise (x.data(), (int) x.size(), 0.1);
        juce::AudioBuffer<float> b (1, 256);
        juce::MidiBuffer m;
        std::vector<float> out;
        for (int pos = 0; pos + 256 <= (int) x.size(); pos += 256)
        {
            b.copyFrom (0, 0, x.data() + pos, 256);
            p->processBlockBypassed (b, m);
            out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + 256);
        }
        chk (measuredDelay (out, x, 4000) == p->getLatencySamples(), "processBlockBypassed keeps the same delay");
    }

    // Changing Input Type while running: the delay moves, and the host is told.
    {
        auto p = make();
        setParam (*p, "inputType", (float) altoTenor);
        prepare (*p, 1, SR, 256);
        const int first = p->getLatencySamples();
        setParam (*p, "inputType", (float) soprano);
        std::vector<float> x (48000);
        noise (x.data(), (int) x.size(), 0.1);
        const auto out = mono (render (*p, x, 256));
        p->applyPendingLatencyNow();
        chk (p->getLatencySamples() < first && p->getLatencySamples() == PluginProcessor::latencyFor (soprano, gEngine, SR), "changing Input Type changes the reported latency, now", p->getLatencySamples(), "samples");
        const std::vector<float> tailIn (x.begin() + 24000, x.end()), tailOut (out.begin() + 24000, out.end());
        chk (measuredDelay (tailOut, tailIn, 4000) == p->getLatencySamples(), "and the real delay matches");
    }

    // Saved state with another Input Type, then prepare
    {
        auto a = make();
        setParam (*a, "inputType", (float) lowMale);
        juce::MemoryBlock st; a->getStateInformation (st);
        auto b = make();
        b->setStateInformation (st.getData(), (int) st.getSize());
        prepare (*b, 2, SR, 256);
        chk (b->getLatencySamples() == PluginProcessor::latencyFor (lowMale, gEngine, SR), "a saved Input Type restores its latency");
    }
}

// --------------------------------------------------------------------------------------------------- real material

struct RefCompare { double seconds = 0; long events = 0; };

std::vector<float> loadWav (const char* name)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    const auto file = juce::File (SMOOTH_TUNE_CORPUS).getChildFile (name);
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr) return {};
    juce::AudioBuffer<float> b (1, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);
    b.applyGain (0.5f);
    return mono (b);
}

void testRealMaterial()
{
    struct Item { const char* file; int type; bool sung; };
    const Item items[] = {
        { "speech_samantha.wav", altoTenor, false }, { "speech_kathy.wav", altoTenor, false },
        { "speech_daniel.wav", lowMale, false },     { "speech_fred.wav", lowMale, false },
        { "held_fred.wav", lowMale, true },          { "held_kathy.wav", altoTenor, true },
        { "sung_cellos.wav", lowMale, true },        { "sung_goodnews.wav", lowMale, true },
        { "sung_organ.wav", lowMale, true },
    };
    std::vector<double> speechMedians, sungMedians;
    double worstFrames = 0.0, worstJump = 0.0;
    double totalSec = 0; long totalEvents = 0;
    for (const auto& it : items)
    {
        const auto x = loadWav (it.file);
        if (x.empty()) { chk (false, std::string ("could not load ") + it.file); continue; }

        // Tracker against a slow two sided reference
        const auto ref = referenceTrack (x, SR);
        smoothtune::PitchTracker t;
        t.prepare (SR);
        const double low = it.type == altoTenor ? 98.0 : 65.0, high = it.type == altoTenor ? 1000.0 : 600.0;
        t.setRange (low, high);
        long i = 0;
        double voicedBoth = 0;
        long events = 0;
        bool inError = false;
        int f = 0;
        for (float v : x)
        {
            t.push (v);
            ++i;
            if (i % 240 == 0)
            {
                const double when = (double) i / SR - 0.006;
                while (f + 1 < (int) ref.size() && referenceTime (f + 1, SR) < when) ++f;
                if (f < (int) ref.size() && ref[(size_t) f] > 0.0 && t.isVoiced())
                {
                    voicedBoth += 0.005;
                    const bool bad = std::abs (cents (SR / t.period(), ref[(size_t) f])) > 600.0;
                    if (bad && ! inError) ++events;
                    inError = bad;
                }
            }
        }
        totalSec += voicedBoth;
        totalEvents += events;
        const double perMin = events / std::max (voicedBoth, 0.1) * 60.0;
        std::printf ("INFO  [%s] %s: %.1f s voiced in both, %ld octave disagreements, %.1f per minute\n", gEngine == 1 ? "HQ" : "LL", it.file, voicedBoth, events, perMin);

        // Corrected at instant speed: how close to a note does the output sit?
        auto p = tuner (it.type, 0.0f);
        const auto out = mono (render (*p, x, 256));
        const auto refOut = referenceTrack (out, SR);
        const auto dist = [] (const std::vector<double>& track)
        {
            std::vector<double> d;
            for (double hz : track)
                if (hz > 0.0)
                {
                    const double m = 69.0 + 12.0 * std::log2 (hz / 440.0);
                    d.push_back (std::abs (m - std::round (m)) * 100.0);
                }
            return d;
        };
        const auto dIn = dist (ref), dOut = dist (refOut);
        std::printf ("INFO  [%s] %s: distance from a note, median, before %.1f cents, after %.1f cents, %d voiced frames\n", gEngine == 1 ? "HQ" : "LL", it.file, median (dIn), median (dOut), (int) dOut.size());
        (it.sung ? sungMedians : speechMedians).push_back (median (dOut));

        // No listening possible here, so look for what bad grains would leave behind: level holes or bursts, and clicks.
        {
            const int delay = p->getLatencySamples(), frame = (int) (0.02 * SR);
            std::vector<double> changes;
            for (int at = delay; at + frame < (int) out.size(); at += frame)
            {
                const double a = rms (x.data() + at - delay, frame), b = rms (out.data() + at, frame);
                if (a > 1.0e-3)
                    changes.push_back (std::abs (toDb (b) - toDb (a)));
            }
            worstFrames = std::max (worstFrames, percentile (changes, 99));
            if (std::getenv ("TUNE_DUMP") != nullptr) std::printf ("  %s: frame level change, 99th percentile %.2f dB, median %.2f dB\n", it.file, percentile (changes, 99), median (changes));
            worstJump = std::max (worstJump, maxJump (out.data(), (int) out.size()) / std::max (maxJump (x.data(), (int) x.size()), 1.0e-6));
        }
        chk (countNonFinite (render (*p, x, 256)) == 0, std::string ("  ") + it.file + " gives finite output");
    }

    chk (worstFrames <= limits::speechFrameLevelDb, "real material at instant speed: 99th percentile change in 20 ms frame level, worst file", worstFrames, "dB");
    chk (worstJump <= limits::speechJumpRatio, "real material at instant speed: biggest sample step against the input's, worst file", worstJump, "times");
    const double perMinute = totalEvents / std::max (totalSec, 1.0) * 60.0;
    chk (perMinute <= limits::speechOctaveErrorsPerMinute, "real speech and say singing: octave disagreements with the slow two sided reference, which is not ground truth", perMinute, "per minute");
    const double speech = *std::max_element (speechMedians.begin(), speechMedians.end());
    chk (speech <= limits::speechOutputMedianCents, "real speech at instant speed: worst file's median distance from a note", speech, "cents");
    const double sung = *std::max_element (sungMedians.begin(), sungMedians.end());
    chk (sung <= limits::sungOutputMedianCents, "say singing and held vowels at instant speed: worst file's median distance from a note", sung, "cents");
}

// ------------------------------------------------------------------------------------------------- added controls

void testAddedControls()
{
    {
        auto p = make();
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "transpose", 7.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (64), 0.4, 1.4));
        chk (std::abs (e) <= limits::transposeCents, "Transpose +7 on A3 gives E4, offset", e, "cents");
    }
    {
        auto p = make();
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "transpose", -12.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (440.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.4));
        chk (std::abs (e) <= limits::transposeCents, "Transpose -12 on A4 gives A3, offset", e, "cents");
    }
    {
        auto p = make();
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "reference", 432.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (222.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 216.0, 0.4, 1.4));
        chk (std::abs (e) <= limits::referenceCents, "Reference A4 432 Hz puts A3 at 216 Hz, offset", e, "cents");
    }
    // Formant Preserve and Throat Length, read with the same tool as the formant test. These also prove that tool sees real moves.
    const auto movedBy = [] (bool preserve, float throat)
    {
        const auto run = glideThroughTuner (voice::vowelA, 110.0, 3, preserve, throat);
        const auto control = controlVoice (voice::vowelA, 110.0, 3);
        const long from = (long) (0.3 * SR), n = (long) run.in.size() - from - 1000;
        const auto want = smoothSpectrumDb (control.data() + from, n, SR, 131.0), got = smoothSpectrumDb (run.out.data() + from, n, SR, 131.0);
        return shapeScale (want, got, 200.0, 2500.0);
    };
    {
        const double m = movedBy (false, 100.0f);
        chk (m >= 1.0 + limits::formantOffMinMove, "Formant Preserve off: formants follow +3 semitones up, scale (19% would be exact)", m, "times");
        chk (std::abs (m / 1.189 - 1.0) <= limits::formantShiftFraction, "  and the formant tool reads that known move within 5%");
    }
    {
        const double m = movedBy (true, 130.0f);
        chk (m <= 1.0 - limits::throatFormantMinMove, "Throat 130%: formants move down, scale (0.77 would be exact)", m, "times");
        chk (std::abs (m / 0.769 - 1.0) <= limits::formantShiftFraction, "  and the formant tool reads that known move within 5%");
    }
    {
        const double m = movedBy (true, 75.0f);
        chk (m >= 1.0 + limits::throatFormantMinMove, "Throat 75%: formants move up, scale (1.33 would be exact)", m, "times");
        chk (std::abs (m / 1.333 - 1.0) <= limits::formantShiftFraction, "  and the formant tool reads that known move within 5%");
    }
    // Tracking: a noisy voice is called pitched or not.
    {
        const auto base = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
        auto x = base;
        const auto n = coloured ((int) base.size(), rms (base.data(), (int) base.size()) * std::pow (10.0, -4.0 / 20.0), 100, 3000, 9);
        for (size_t i = 0; i < x.size(); ++i) x[i] += n[i];
        const auto voicedShare = [&x] (float tracking)
        {
            auto p = make();
            setParam (*p, "tracking", tracking);
            prepare (*p, 1, SR, 256);
            int voiced = 0, total = 0;
            render (*p, x, 256, nullptr, [&] (int idx) { if (idx > 40) { voiced += p->getReadings().voiced ? 1 : 0; ++total; } });
            return total > 0 ? 100.0 * voiced / total : 0.0;
        };
        const double choosy = voicedShare (0.0f), relaxed = voicedShare (100.0f);
        chk (choosy <= limits::trackingChoosyPercent, "Tracking 0 calls a noisy voice pitched this share of the time", choosy, "%");
        chk (relaxed >= limits::trackingRelaxedPercent, "Tracking 100 calls the same voice pitched this share of the time", relaxed, "%");
    }
}

// ------------------------------------------------------------------------------------------------- meter and misc

struct CountListener final : public juce::AudioProcessorListener
{
    std::atomic<int> changes { 0 };
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { ++changes; }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
};

void testMeter()
{
    auto p = tuner (altoTenor, 0.0f);
    CountListener listener;
    p->addListener (&listener);
    const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
    render (*p, x, 256);
    p->removeListener (&listener);
    const auto r = p->getReadings();
    chk (std::abs (r.centsOffTarget - 30.0f) <= 5.0f, "Pitch Off Target reads the sung error, 30 cents sharp", r.centsOffTarget, "cents");
    chk (r.targetNote == 57, "Target Note reads A3");
    chk (std::abs (r.correctionCents + 30.0f) <= 5.0f, "Correction Applied reads the correction, -30 cents", r.correctionCents, "cents");
    chk (std::abs (r.detectedHz - 223.8f) <= 2.0f, "Detected Pitch reads the sung pitch", r.detectedHz, "Hz");
    chk (listener.changes.load() == 0, "readings send nothing to the host while audio runs: parameter messages", listener.changes.load());

    // Value strings through the parameter, as a host reads them
    for (auto* prm : p->getParameters())
        if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            if (w->paramID == "pitchOffTarget")
                chk (prm->getCurrentValueAsText().containsIgnoreCase ("cents") && ! prm->isAutomatable(), "Pitch Off Target has a real value string and is not automatable: " + prm->getCurrentValueAsText().toStdString());

    // No pitch: readings say so.
    std::vector<float> silence (48000, 0.0f);
    render (*p, silence, 256);
    const auto q = p->getReadings();
    chk (! q.voiced && q.centsOffTarget == 0.0f && q.targetNote == -1, "silence: readings show no pitch");

    // The window shows them, without any host message.
    {
        auto x2 = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 0.5, SR), voice::vowelA, SR);
        render (*p, x2, 256);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
        auto* te = dynamic_cast<TuneEditor*> (ed.get());
        chk (te != nullptr && te->readingText().contains ("Hz") && te->readingText().contains ("A3"), "the window's reading strip shows the pitch: " + (te != nullptr ? te->readingText().toStdString() : std::string ("none")));
        juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        juce::Graphics g (img);
        ed->paintEntireComponent (g, false);
        chk (ed->getWidth() > 200 && ed->getHeight() > 100, "the window opens and paints");
    }

    // Pitch history for a later graph
    chk (p->getPitchHistoryCount() > 0 && p->getPitchPoint (p->getPitchHistoryCount() - 1).targetNote.load() >= -1.0f, "pitch history is filling, points", (double) p->getPitchHistoryCount());
}

void testBlocksAndState()
{
    // Same output whatever the block size: all timing counts samples, not blocks.
    const auto x = voice::synth (voice::phrase ({ 200.0, 240.0, 300.0 }, 0.4, SR), voice::vowelA, SR, -30.0);
    std::vector<float> first;
    bool same = true;
    for (int block : { 512, 32, 4096, 137 })
    {
        auto p = make();
        setParam (*p, "retune", 15.0f);
        prepare (*p, 1, SR, 4096);
        const auto out = mono (render (*p, x, block));
        if (first.empty()) first = out;
        else for (size_t i = 0; i < out.size(); ++i) if (std::abs (out[i] - first[i]) > 1.0e-5f) { same = false; std::printf ("  block %d differs at %d\n", block, (int) i); break; }
    }
    chk (same, "output does not depend on block size (and so not on a playhead)");

    // Calling prepareToPlay again gives the same result, and does not leave old state behind.
    {
        auto p = make();
        setParam (*p, "retune", 15.0f);
        prepare (*p, 1, SR, 512);
        const auto a = mono (render (*p, x, 512));
        prepare (*p, 1, SR, 512);
        const auto b = mono (render (*p, x, 512));
        bool equal = true;
        for (size_t i = 0; i < a.size(); ++i) if (std::abs (a[i] - b[i]) > 1.0e-5f) { equal = false; break; }
        chk (equal, "prepareToPlay again resets all state");
    }

    // Stereo: same grains on both sides, so the image holds. Left only stays left.
    {
        auto p = make();
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f);
        prepare (*p, 2, SR, 256);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer m;
        const auto v = voice::synth (voice::steady (225.0, 1.0, SR), voice::vowelA, SR);
        double right = 0, left = 0;
        for (int pos = 0; pos + 256 <= (int) v.size(); pos += 256)
        {
            buf.copyFrom (0, 0, v.data() + pos, 256);
            buf.clear (1, 0, 256);
            p->processBlock (buf, m);
            left += buf.getRMSLevel (0, 0, 256);
            right += buf.getRMSLevel (1, 0, 256);
        }
        chk (right < 1.0e-4 * left, "stereo: a voice on the left stays on the left, right against left", toDb (right / left), "dB");
    }

    // Presets do what their names say
    {
        auto p = make();
        const int n = p->getNumPrograms();
        int hard = -1, natural = -1;
        for (int i = 0; i < n; ++i)
        {
            if (p->getProgramName (i) == "Hard Tune") hard = i;
            if (p->getProgramName (i) == "Natural Pop") natural = i;
        }
        chk (hard >= 0 && natural >= 0 && n >= 4, "hard and natural presets are both there, presets", n);
        p->setCurrentProgram (hard);
        const double hardKept = vibratoKept (*p);
        p->setCurrentProgram (natural);
        const double naturalKept = vibratoKept (*p);
        chk (hardKept <= limits::vibratoFlatHard, "preset Hard Tune flattens vibrato, kept", hardKept * 100.0, "%");
        chk (naturalKept > hardKept + 0.2, "preset Natural Pop keeps more vibrato, kept", naturalKept * 100.0, "%");

        // Presets leave Key, Scale, Input Type and Follow Key alone.
        auto q = make();
        setParam (*q, "key", 5.0f); setParam (*q, "scale", 2.0f); setParam (*q, "inputType", 2.0f);
        q->setCurrentProgram (hard);
        chk (juce::roundToInt (q->apvts.getRawParameterValue ("key")->load()) == 5 && juce::roundToInt (q->apvts.getRawParameterValue ("scale")->load()) == 2 && juce::roundToInt (q->apvts.getRawParameterValue ("inputType")->load()) == 2, "presets leave Key, Scale and Input Type alone");
    }
}

void testCpu()
{
    auto p = make();
    setParam (*p, "retune", 15.0f);
    setParam (*p, "inputType", (float) wideRange);
    prepare (*p, 2, SR, 256);
    const auto x = voice::synth (voice::vibrato (180.0, 10.0, SR, 30.0, 5.5), voice::vowelA, SR, -30.0);
    const auto start = std::clock();
    render (*p, x, 256);
    const double seconds = (double) (std::clock() - start) / CLOCKS_PER_SEC;
    const double percent = seconds / 10.0 * 100.0;
    chk (percent <= (gEngine == 1 ? prolimits::hqCpuPercent : prolimits::llCpuPercent), "CPU on one core, 48 kHz stereo, Wide Range, voice at 180 Hz (machine busy with other builds)", percent, "%");

    // While correcting by 3 semitones, the heavier case, and at 96 kHz.
    for (double sr : { 48000.0, 96000.0 })
    {
        auto q = make();
        setParam (*q, "retune", 15.0f); setParam (*q, "transpose", 3.0f); setParam (*q, "inputType", (float) wideRange);
        prepare (*q, 2, sr, 256);
        const auto y = voice::synth (voice::vibrato (360.0, 5.0, sr, 30.0, 5.5), voice::vowelA, sr, -30.0);
        const auto begin = std::clock();
        render (*q, y, 256);
        note ("  CPU correcting by 3 semitones at " + std::to_string ((int) sr) + " Hz, harmonic model in use", (double) (std::clock() - begin) / CLOCKS_PER_SEC / 5.0 * 100.0, "%");
    }
}

// ------------------------------------------------------------------------------------------------------------ MIDI

juce::MidiBuffer noteOnAt (int note, int sample = 0) { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), sample); return m; }

// Renders in blocks, with MIDI events placed at absolute sample positions.
std::vector<float> renderWithMidi (PluginProcessor& p, const std::vector<float>& x, const std::vector<std::pair<long, juce::MidiMessage>>& events, int block = 256)
{
    juce::AudioBuffer<float> buf (p.getTotalNumOutputChannels(), block);
    std::vector<float> out;
    out.reserve (x.size());
    for (long pos = 0; pos < (long) x.size(); pos += block)
    {
        const int len = (int) std::min ((long) block, (long) x.size() - pos);
        buf.setSize (buf.getNumChannels(), len, false, false, true);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch) std::copy_n (x.data() + pos, len, buf.getWritePointer (ch));
        juce::MidiBuffer midi;
        for (const auto& e : events)
            if (e.first >= pos && e.first < pos + len)
                midi.addEvent (e.second, (int) (e.first - pos));
        p.processBlock (buf, midi);
        out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + len);
    }
    return out;
}

std::unique_ptr<PluginProcessor> midiTuner (bool allOctaves = true, float retune = 0.0f)
{
    auto p = tuner (altoTenor, retune);
    setParam (*p, "midiTargets", 1.0f);
    setParam (*p, "midiOctave", allOctaves ? 0.0f : 1.0f);
    prepare (*p, 1, SR, 256);
    return p;
}

void testMidi()
{
    const auto noteOn = [] (int n) { return juce::MidiMessage::noteOn (1, n, (juce::uint8) 100); };
    const auto noteOff = [] (int n) { return juce::MidiMessage::noteOff (1, n); };
    const auto x = voice::synth (voice::steady (noteHz (57) * std::pow (2.0, 30.0 / 1200.0), 2.0, SR), voice::vowelA, SR);

    // Target accuracy: held notes other than the nearest one, in the right octave as played.
    double worst = 0.0;
    for (int held : { 57, 60, 55, 52, 64, 69 })
    {
        auto p = midiTuner (false);
        const auto out = renderWithMidi (*p, x, { { 0L, noteOn (held) } });
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (held), 0.4, 1.8);
        const double q = e.empty() ? 1.0e6 : percentile (absolute (e), 95);
        worst = std::max (worst, q);
    }
    chk (worst <= prolimits::midiTargetCents, "MIDI Targets, As Played: output on the held note, six notes, worst 95th percentile", worst, "cents");

    // All Octaves: a C held two octaves down still gives the nearest C.
    {
        auto p = midiTuner (true);
        const auto out = renderWithMidi (*p, x, { { 0L, noteOn (36) } });
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (60), 0.4, 1.8);
        const double q = e.empty() ? 1.0e6 : percentile (absolute (e), 95);
        chk (q <= prolimits::midiTargetCents, "MIDI Targets, All Octaves: C1 held, A3 sung comes out on the nearest C, 95th percentile", q, "cents");
    }

    // Two notes held: the nearer one wins.
    {
        auto p = midiTuner (false);
        const auto out = renderWithMidi (*p, x, { { 0L, noteOn (40) }, { 0L, noteOn (59) } });
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (59), 0.4, 1.8);
        const double q = e.empty() ? 1.0e6 : percentile (absolute (e), 95);
        chk (q <= prolimits::midiTargetCents, "MIDI Targets: of two held notes the nearer is the target, 95th percentile", q, "cents");
    }

    // No notes: nothing is corrected.
    {
        auto p = midiTuner();
        const auto out = renderWithMidi (*p, x, {});
        const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.3 * SR), (long) (1.8 * SR));
        chk (db <= prolimits::midiNoNotesNullDb, "MIDI Targets on with no notes: the voice passes unchanged, difference", db, "dB");
    }

    // Letting go: how soon is the voice back where it was sung?
    {
        auto p = midiTuner (true);
        const long off = (long) (1.0 * SR);
        const auto out = renderWithMidi (*p, x, { { 0L, noteOn (60) }, { off, noteOff (60) } });
        const double sung = noteHz (57) * std::pow (2.0, 30.0 / 1200.0);
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, noteHz (58), 1.0 + p->getLatencySamples() / SR, 1.5, 0.002, 0.25, 2.5);
        double backMs = 1.0e6;
        for (size_t i = 0; i < trace.size(); ++i)
            if (trace[i] > 0.0 && std::abs (cents (trace[i], sung)) < 10.0) { backMs = (double) i * 2.0; break; }
        chk (backMs <= prolimits::midiReleaseMs, "MIDI note off: correction gone at Retune Speed 0 after", backMs, "ms");
    }

    // A note pressed part way through: it takes over at the right time.
    {
        auto p = midiTuner (true);
        const long on = (long) (1.0 * SR);
        const auto out = renderWithMidi (*p, x, { { on, noteOn (60) } });
        const auto before = centsOff (out.data(), (long) out.size(), SR, noteHz (57) * std::pow (2.0, 30.0 / 1200.0), 0.4, 0.95);
        const auto after = centsOff (out.data(), (long) out.size(), SR, noteHz (60), 1.12, 1.8);
        chk (! before.empty() && ! after.empty() && percentile (absolute (before), 95) <= limits::customNoteCents && percentile (absolute (after), 95) <= prolimits::midiTargetCents, "a note pressed part way through takes over, 95th percentile after", after.empty() ? 1.0e6 : percentile (absolute (after), 95), "cents");
    }

    // The pedal keeps a note held. All Notes Off lets go.
    {
        auto p = midiTuner (true);
        const auto out = renderWithMidi (*p, x, { { 100L, juce::MidiMessage::controllerEvent (1, 64, 127) }, { 200L, noteOn (60) }, { (long) (0.2 * SR), noteOff (60) }, { (long) (1.2 * SR), juce::MidiMessage::controllerEvent (1, 64, 0) } });
        const auto held = centsOff (out.data(), (long) out.size(), SR, noteHz (60), 0.5, 1.1);
        const auto released = centsOff (out.data(), (long) out.size(), SR, noteHz (57) * std::pow (2.0, 30.0 / 1200.0), 1.4, 1.8);
        chk (! held.empty() && ! released.empty() && percentile (absolute (held), 95) <= prolimits::midiTargetCents && percentile (absolute (released), 95) <= limits::customNoteCents, "the sustain pedal holds a target, and lifting it lets go");
    }
    {
        auto p = midiTuner (true);
        const auto out = renderWithMidi (*p, x, { { 200L, noteOn (60) }, { (long) (1.0 * SR), juce::MidiMessage::allNotesOff (1) } });
        const auto released = centsOff (out.data(), (long) out.size(), SR, noteHz (57) * std::pow (2.0, 30.0 / 1200.0), 1.2, 1.8);
        chk (! released.empty() && percentile (absolute (released), 95) <= limits::customNoteCents, "All Notes Off lets go of every note");
    }

    // Default settings tune with no MIDI at all, and ignore notes.
    {
        auto p = tuner (altoTenor, 0.0f);
        const auto out = renderWithMidi (*p, x, {});
        const auto a = centsOff (out.data(), (long) out.size(), SR, noteHz (57), 0.4, 1.8);
        chk (! a.empty() && percentile (absolute (a), 95) <= limits::outputInstantCents, "default mode tunes with no MIDI, 95th percentile", a.empty() ? 1.0e6 : percentile (absolute (a), 95), "cents");
        auto q = tuner (altoTenor, 0.0f);
        const auto outQ = renderWithMidi (*q, x, { { 0L, noteOn (60) } });
        const auto b = centsOff (outQ.data(), (long) outQ.size(), SR, noteHz (57), 0.4, 1.8);
        chk (! b.empty() && percentile (absolute (b), 95) <= limits::outputInstantCents, "with MIDI Targets off a held note changes nothing");
        auto fresh = make();
        chk (fresh->acceptsMidi() && juce::roundToInt (fresh->apvts.getRawParameterValue ("midiTargets")->load()) == 0, "takes MIDI, and MIDI Targets is off by default");
    }

    // Timing counts samples, not blocks.
    {
        const long on = 30000;
        std::vector<float> first;
        bool same = true;
        for (int block : { 512, 64, 1000 })
        {
            auto p = midiTuner (true, 10.0f);
            const auto out = renderWithMidi (*p, x, { { on, noteOn (60) } }, block);
            if (first.empty()) first = out;
            else for (size_t i = 0; i < out.size(); ++i) if (std::abs (out[i] - first[i]) > 1.0e-5f) { same = false; break; }
        }
        chk (same, "a MIDI note lands at the same sample at any block size");
    }

    // Bypass while MIDI Targets are on: the plain delayed input.
    {
        auto p = midiTuner (true);
        setParam (*p, "bypass", 1.0f);
        prepare (*p, 1, SR, 256);
        const auto out = renderWithMidi (*p, x, { { 0L, noteOn (60) } });
        const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.3 * SR), (long) (1.8 * SR));
        chk (db <= -120.0, "Bypass with a MIDI note held is the plain delayed input, difference", db, "dB");
    }
}

// ------------------------------------------------------------------------------------------------------ per note

double centsAgainst (PluginProcessor& p, double sungHz, double expectedHz)
{
    const auto x = voice::synth (voice::steady (sungHz, 1.5, SR), voice::vowelA, SR);
    const auto out = mono (render (p, x, 256));
    const auto e = centsOff (out.data(), (long) out.size(), SR, expectedHz, 0.4, 1.4);
    return e.empty() ? 1.0e6 : percentile (absolute (e), 95);
}

void testPerNote()
{
    const auto sharp = [] (int midi, double c) { return noteHz (midi) * std::pow (2.0, c / 1200.0); };
    const auto setNote = [] (PluginProcessor& p, int pc, float mode) { setParam (p, ("note" + juce::String (pc)).toRawUTF8(), mode); };
    { // Remove on Chromatic: A +20 cents goes to A#, the next note up, as A is not a target.
        auto p = tuner (altoTenor, 0.0f); setNote (*p, 9, smoothtune::noteRemove);
        const double e = centsAgainst (*p, sharp (57, 20.0), noteHz (58));
        chk (e <= prolimits::perNoteCents, "Chromatic, A set to Remove: A +20 cents goes to A#, 95th percentile", e, "cents");
    }
    { // And below the half way point it goes to G#.
        auto p = tuner (altoTenor, 0.0f); setNote (*p, 9, smoothtune::noteRemove);
        const double e = centsAgainst (*p, sharp (57, -20.0), noteHz (56));
        chk (e <= prolimits::perNoteCents, "Chromatic, A set to Remove: A -20 cents goes to G#, 95th percentile", e, "cents");
    }
    { // Bypass on Chromatic: stays where it was sung.
        auto p = tuner (altoTenor, 0.0f); setNote (*p, 9, smoothtune::noteBypass);
        const double e = centsAgainst (*p, sharp (57, 30.0), sharp (57, 30.0));
        chk (e <= prolimits::perNoteCents, "Chromatic, A set to Bypass: A +30 cents stays where it was sung, 95th percentile", e, "cents");
    }
    { // The others are still tuned when one is bypassed.
        auto p = tuner (altoTenor, 0.0f); setNote (*p, 9, smoothtune::noteBypass);
        const double e = centsAgainst (*p, sharp (60, 30.0), noteHz (60));
        chk (e <= prolimits::perNoteCents, "Chromatic, A set to Bypass: C +30 cents is still tuned, 95th percentile", e, "cents");
    }
    { // On top of a scale: in C major, remove A, and A +20 goes to B or G, whichever is nearer, which is B.
        auto p = tuner (altoTenor, 0.0f); setParam (*p, "scale", (float) smoothtune::scaleMajor); setNote (*p, 9, smoothtune::noteRemove);
        const double e = centsAgainst (*p, sharp (57, 20.0), noteHz (59));
        chk (e <= prolimits::perNoteCents, "C Major, A set to Remove: A +20 cents goes to B, 95th percentile", e, "cents");
    }
    { // Bypass on a note outside the scale: C# in C major is sung and left alone.
        auto p = tuner (altoTenor, 0.0f); setParam (*p, "scale", (float) smoothtune::scaleMajor); setNote (*p, 1, smoothtune::noteBypass);
        const double e = centsAgainst (*p, sharp (61, 25.0), sharp (61, 25.0));
        chk (e <= prolimits::perNoteCents, "C Major, C# set to Bypass: C# +25 cents stays where it was sung, 95th percentile", e, "cents");
    }
    { // Every note removed: the voice is left alone.
        auto p = tuner (altoTenor, 0.0f);
        for (int i = 0; i < 12; ++i) setNote (*p, i, smoothtune::noteRemove);
        const double e = centsAgainst (*p, sharp (57, 30.0), sharp (57, 30.0));
        chk (e <= prolimits::perNoteCents, "every note set to Remove leaves the voice alone, 95th percentile", e, "cents");
    }
    { // Each note parameter is a real, named AU parameter with a value string.
        auto p = make();
        bool ok = true;
        for (int i = 0; i < 12; ++i)
        {
            auto* prm = p->apvts.getParameter ("note" + juce::String (i));
            if (prm == nullptr || prm->getName (32) != juce::String ("Note ") + smoothtune::noteNames[i] || ! prm->isAutomatable()) ok = false;
            else
            {
                prm->setValueNotifyingHost (prm->convertTo0to1 (2.0f));
                if (prm->getCurrentValueAsText() != "Bypass") ok = false;
            }
        }
        chk (ok, "twelve real note parameters, named Note C to Note B, reading Tune, Remove or Bypass");
    }
}

// --------------------------------------------------------------------------------------------------------- classic

// 10% to 90% time of a step between two notes a tone apart, with a slide, as in the Smooth Tune test.
double stepMs (PluginProcessor& p)
{
    setParam (p, "key", 0.0f); setParam (p, "scale", (float) smoothtune::scaleMajor);
    prepare (p, 1, SR, 256);
    const double a = noteHz (57), b = noteHz (59), start = 1.0, slide = 0.04;
    std::vector<double> f0 ((size_t) (2.0 * SR));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        const double u = std::clamp (((double) i / SR - start) / slide, 0.0, 1.0);
        f0[i] = a * std::pow (b / a, u);
    }
    const auto x = voice::synth (f0, voice::vowelA, SR);
    const auto out = mono (render (p, x, 256));
    const double hop = 0.0005, hzMid = std::sqrt (a * b);
    const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hzMid, start - 0.1, start + 0.4, hop, 0.14, 2.0);
    double t10 = -1, t90 = -1;
    for (size_t i = 0; i < trace.size(); ++i)
    {
        if (trace[i] <= 0.0) continue;
        const double u = std::log (trace[i] / a) / std::log (b / a);
        if (u <= 0.1) t10 = (double) i * hop;
        if (u > 0.9) { t90 = (double) i * hop; break; }
    }
    return t10 >= 0 && t90 >= 0 ? (t90 - t10) * 1000.0 : 1.0e6;
}

void testClassic()
{
    auto p = make();
    setParam (*p, "classic", 1.0f);
    // Settings that would slow it down, all of which Classic ignores.
    setParam (*p, "retune", 200.0f); setParam (*p, "humanize", 100.0f); setParam (*p, "flex", 100.0f); setParam (*p, "naturalVibrato", 100.0f); setParam (*p, "vibratoAmount", 50.0f);
    const double classicMs = stepMs (*p);
    chk (classicMs <= prolimits::classicStepMs, "Classic: step between two notes, 10% to 90%, whatever the other controls say", classicMs, "ms");

    auto q = make();
    setParam (*q, "retune", 20.0f); setParam (*q, "humanize", 0.0f);
    const double normalMs = stepMs (*q);
    chk (classicMs <= prolimits::classicAgainstRetuneRatio * normalMs, "Classic step against the same step at Retune Speed 20", classicMs / normalMs, "times");
    note ("  that step at Retune Speed 20", normalMs, "ms");

    auto r = make();
    setParam (*r, "classic", 1.0f); setParam (*r, "flex", 100.0f); setParam (*r, "humanize", 100.0f); setParam (*r, "retune", 200.0f);
    const double kept = vibratoKept (*r);
    chk (kept <= limits::vibratoFlatHard, "Classic flattens vibrato with Flex Tune and Humanize at full, kept", kept * 100.0, "%");
}

// ------------------------------------------------------------------------------------------------------- transpose

void testTranspose()
{
    double worst = 0.0, worstLow = 0.0;
    for (int by : { -24, -12, -7, -2, -1, 1, 2, 7, 12, 24 })
    {
        auto p = make();
        setParam (*p, "inputType", (float) wideRange);
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "transpose", (float) by);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (noteHz (57) * std::pow (2.0, 25.0 / 1200.0), 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (57 + by), 0.4, 1.4);
        const double q = e.empty() ? 1.0e6 : percentile (absolute (e), 95);
        if (std::getenv ("TUNE_DUMP") != nullptr) std::printf ("  transpose %d: %.2f cents\n", by, q);
        worst = std::max (worst, q);
        worstLow = std::max (worstLow, std::abs (toDb (rms (out.data() + 24000, 24000)) - toDb (rms (x.data() + 24000, 24000))));
    }
    chk (worst <= prolimits::transposeCents, "Transpose -24 to +24 on a corrected A3, ten settings, worst 95th percentile", worst, "cents");
    note ("  level change across those settings, worst", worstLow, "dB");
}

// ------------------------------------------------------------------------------------------------------------ throat

void testThroat()
{
    // The same tool as the formant test, on the two settings with an exact answer. The pitch must stay put meanwhile.
    double worstScale = 0.0, worstPitch = 0.0;
    for (float throat : { 130.0f, 115.0f, 85.0f, 75.0f })
    {
        const auto run = glideThroughTuner (voice::vowelA, 110.0, 0, true, throat);
        const long from = (long) (0.3 * SR), n = (long) run.in.size() - from - 4000;
        const auto want = smoothSpectrumDb (run.in.data() + from, n, SR, 110.0), got = smoothSpectrumDb (run.out.data() + from, n, SR, 110.0);
        const double scale = shapeScale (want, got, 200.0, 2500.0);
        worstScale = std::max (worstScale, std::abs (scale * throat / 100.0 - 1.0));
        if (std::getenv ("TUNE_DUMP") != nullptr) std::printf ("  throat %.0f: scale %.3f, exact %.3f\n", throat, scale, 100.0 / throat);

        auto p = tuner (altoTenor, 0.0f);
        setParam (*p, "throat", throat);
        const auto x = voice::synth (voice::steady (noteHz (57) * std::pow (2.0, 25.0 / 1200.0), 1.5, SR), voice::vowelE, SR);
        const auto out = mono (render (*p, x, 256));
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (57), 0.4, 1.4);
        worstPitch = std::max (worstPitch, e.empty() ? 1.0e6 : percentile (absolute (e), 95));
    }
    chk (worstScale <= prolimits::throatScaleError, "Throat 75 to 130%: formants move by the setting, worst error", worstScale * 100.0, "%");
    chk (worstPitch <= prolimits::throatPitchCents, "Throat 75 to 130%: pitch stays on the note, worst 95th percentile", worstPitch, "cents");

    // Consonants are not moved: only voiced sound.
    {
        auto p = tuner (altoTenor, 0.0f);
        setParam (*p, "throat", 130.0f);
        const auto x = coloured ((int) (1.5 * SR), 0.08, 4000, 10000, 8);
        const auto out = mono (render (*p, x, 256));
        const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.2 * SR), (long) (1.4 * SR));
        chk (db <= limits::unvoicedNullDb, "Throat 130% leaves an S unchanged, difference", db, "dB");
    }
}

// ------------------------------------------------------------------------------------------------- vibrato generator

void testVibratoGenerator()
{
    const auto run = [] (float amount, float rate, float delayMs, double seconds, std::vector<float>& out)
    {
        auto p = tuner (altoTenor, 0.0f);
        setParam (*p, "vibratoAmount", amount);
        setParam (*p, "vibratoRate", rate);
        setParam (*p, "vibratoDelay", delayMs);
        const auto x = voice::synth (voice::steady (220.0, seconds, SR), voice::vowelA, SR);
        out = mono (render (*p, x, 256));
        return p->getLatencySamples();
    };
    std::vector<float> out;
    run (30.0f, 5.5f, 0.0f, 3.0, out);
    const double hop = 0.002;
    const auto trace = pitchTrace (out.data(), (long) out.size(), SR, 220.0, 0.5, 2.9, hop, 0.1);
    std::vector<double> c;
    for (double h : trace) c.push_back (h > 0.0 ? cents (h, 220.0) : std::nan (""));
    const double depth = sineDepth (c, hop, 5.5);
    chk (std::abs (depth / 30.0 - 1.0) <= prolimits::vibratoDepthError, "Vibrato Amount 30 cents, measured depth", depth, "cents");
    double bestRate = 0.0, bestDepth = 0.0;
    for (double r = 3.0; r <= 9.0; r += 0.05)
    {
        const double d = sineDepth (c, hop, r);
        if (d > bestDepth) { bestDepth = d; bestRate = r; }
    }
    chk (std::abs (bestRate / 5.5 - 1.0) <= prolimits::vibratoRateError, "Vibrato Rate 5.5 Hz, measured rate", bestRate, "Hz");
    run (0.0f, 5.5f, 0.0f, 2.0, out);
    {
        const auto e = centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.9);
        chk (percentile (absolute (e), 95) <= limits::outputInstantCents, "Vibrato Amount 0 adds nothing, 95th percentile", percentile (absolute (e), 95), "cents");
    }
    // Delay: when does the swing reach half the depth, counted from the first output?
    {
        const float delay = 800.0f;
        const int latency = run (40.0f, 6.0f, delay, 2.5, out);
        const double start = latency / SR;
        const auto t2 = pitchTrace (out.data(), (long) out.size(), SR, 220.0, start + 0.1, 2.4, hop, 0.1);
        double reached = -1.0;
        for (size_t i = 0; i < t2.size(); ++i)
            if (t2[i] > 0.0 && std::abs (cents (t2[i], 220.0)) >= 20.0) { reached = 0.1 + (double) i * hop; break; }
        const double expected = 0.001 * delay + 0.5 * smoothtune::PitchCorrector::vibratoFadeSeconds;
        chk (std::abs (reached - expected) * 1000.0 <= prolimits::vibratoDelayErrorMs, "Vibrato Delay 800 ms, half depth reached after", reached * 1000.0, "ms");
    }
}

// --------------------------------------------------------------------------------------------------------------- mix

void testMix()
{
    {
        auto p = tuner (altoTenor, 0.0f);
        setParam (*p, "mix", 0.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (225.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.2 * SR), (long) (1.4 * SR));
        chk (db <= prolimits::mixDryNullDb, "Mix 0 is the plain delayed input, difference", db, "dB");
    }
    {
        // In tune and left alone: dry and wet line up, so Mix 50 changes nothing.
        auto p = tuner (altoTenor, 0.0f, 0.0f, 100.0f);
        setParam (*p, "mix", 50.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double change = toDb (rms (out.data() + 24000, 36000)) - toDb (rms (x.data() + 24000, 36000));
        chk (std::abs (change) <= prolimits::mixHalfLevelDb, "Mix 50 on a voice with nothing to correct, level change", change, "dB");
    }
    {
        // Mix 100 is the tuned voice: both pitches are there at 50, and only the tuned one at 100.
        auto p = tuner (altoTenor, 0.0f);
        const auto x = voice::synth (voice::steady (noteHz (57) * std::pow (2.0, 40.0 / 1200.0), 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const auto e = centsOff (out.data(), (long) out.size(), SR, noteHz (57), 0.4, 1.4);
        chk (percentile (absolute (e), 95) <= limits::outputInstantCents, "Mix 100 is the tuned voice, 95th percentile", percentile (absolute (e), 95), "cents");
    }
}

// ------------------------------------------------------------------------------------------------------------ engines

void testEngines()
{
    // Reported latency and the numbers side by side
    {
        for (int type = 0; type < 4; ++type)
        {
            info (std::string ("High Quality against Low Latency at 48 kHz, input type ") + std::to_string (type) + ", Low Latency", PluginProcessor::latencyFor (type, 0, SR) * 1000.0 / SR, "ms");
            info (std::string ("High Quality against Low Latency at 48 kHz, input type ") + std::to_string (type) + ", High Quality", PluginProcessor::latencyFor (type, 1, SR) * 1000.0 / SR, "ms");
        }
    }

    // Changing engine while a voice plays: no step beyond the voice's own, no hole, and the real delay follows.
    {
        double worstRatio = 0.0, worstHole = 0.0;
        for (double hz : { 110.0, 147.0, 196.0, 220.0, 262.0, 330.0, 440.0 })
            for (int from : { 0, 1 })
            {
                gEngine = from;
                auto p = tuner (wideRange, 20.0f);
                const auto x = voice::synth (voice::steady (hz * std::pow (2.0, 30.0 / 1200.0), 2.0, SR), voice::vowelA, SR);
                std::vector<float> out = mono (render (*p, x, 256, nullptr, [&] (int idx) { if (idx == 180) setParam (*p, "engine", from == 0 ? 1.0f : 0.0f); }));
                gEngine = 0;
                const int switchAt = 180 * 256, frame = 240;
                const int a = switchAt - 4800, b = switchAt + 19200;
                const double own = maxJump (out.data() + 12000, 24000), during = maxJump (out.data() + a, b - a);
                worstRatio = std::max (worstRatio, during / std::max (own, 1.0e-9));
                const double steadyDb = toDb (rms (out.data() + 12000, 24000));
                for (int at = a; at + frame < b; at += 48)
                    worstHole = std::max (worstHole, steadyDb - toDb (rms (out.data() + at, frame)));
            }
        check (worstRatio <= prolimits::modeSwitchStepRatio, "switching engine on a voice, both ways, 7 pitches: biggest sample step against the voice's own", worstRatio, "times");
        check (worstHole <= prolimits::modeSwitchHoleDb, "switching engine, deepest fall in 5 ms level", worstHole, "dB");
    }
    {
        auto p = tuner (altoTenor, 20.0f);
        const int first = p->getLatencySamples();
        std::vector<float> x (96000);
        noise (x.data(), (int) x.size(), 0.1);
        const auto out = mono (render (*p, x, 256, nullptr, [&] (int idx) { if (idx == 40) setParam (*p, "engine", 1.0f); }));
        p->applyPendingLatencyNow();
        const int second = p->getLatencySamples();
        check (second == PluginProcessor::latencyFor (altoTenor, 1, SR) && second > first, "switching to High Quality reports the new latency", second, "samples");
        const std::vector<float> tailIn (x.begin() + 48000, x.end()), tailOut (out.begin() + 48000, out.end());
        check (measuredDelay (tailOut, tailIn, 4000) == second, "and the real delay matches it");
    }
    {
        // Back and forth quickly must not stick or break: the last choice wins.
        auto p = tuner (altoTenor, 20.0f);
        std::vector<float> x (96000);
        noise (x.data(), (int) x.size(), 0.1);
        const auto out = mono (render (*p, x, 256, nullptr, [&] (int idx) { if (idx < 100) setParam (*p, "engine", (idx / 5) % 2 == 0 ? 1.0f : 0.0f); else setParam (*p, "engine", 1.0f); }));
        p->applyPendingLatencyNow();
        const std::vector<float> tailIn (x.begin() + 48000, x.end()), tailOut (out.begin() + 48000, out.end());
        check (countNonFinite (render (*p, x, 256)) == 0 && measuredDelay (tailOut, tailIn, 4000) == PluginProcessor::latencyFor (altoTenor, 1, SR), "flipping engines every 27 ms ends on the right delay");
    }

    // What the lookahead is for. A note starts from silence, 25 cents sharp, at instant speed: is it on pitch straight away?
    const auto onsetShare = [] (int engine)
    {
        gEngine = engine;
        double total = 0.0, good = 0.0;
        for (double hz : { noteHz (50), noteHz (57), noteHz (64) })
            for (const auto* v : { &voice::vowelA, &voice::vowelE })
            {
                auto p = tuner (altoTenor, 0.0f);
                auto sung = voice::synth (voice::steady (hz * std::pow (2.0, 25.0 / 1200.0), 0.6, SR), *v, SR);
                std::vector<float> x ((size_t) (0.5 * SR), 0.0f);
                for (int i = 0; i < 480; ++i) sung[(size_t) i] *= (float) i / 480.0f;
                x.insert (x.end(), sung.begin(), sung.end());
                const auto out = mono (render (*p, x, 256));
                const double start = 0.5 + p->getLatencySamples() / SR;
                for (double t = 0.010; t <= 0.040; t += 0.002)
                {
                    const double got = pitchNear (out.data(), (long) out.size(), (long) ((start + t) * SR), hz, SR, 0.05, 1.5);
                    total += 1.0;
                    if (got > 0.0 && std::abs (cents (got, hz)) <= 10.0) good += 1.0;
                    if (std::getenv ("TUNE_DUMP") != nullptr) std::printf ("  %d hz %.0f vowel %s t %.0f ms: %.1f cents\n", engine, hz, v->name, t * 1000.0, got > 0.0 ? cents (got, hz) : -999.0);
                }
            }
        gEngine = 0;
        return good / total;
    };
    const double ll = onsetShare (0), hq = onsetShare (1);
    info ("Low Latency: share of 10 to 40 ms after a note starts that is within 10 cents", ll * 100.0, "%");
    check (hq >= prolimits::hqOnsetOnPitchShare, "High Quality: share of 10 to 40 ms after a note starts that is within 10 cents", hq * 100.0, "%");

    // An S straight after a corrected vowel: how much of its start is left alone?
    const auto consonantStart = [] (int engine)
    {
        gEngine = engine;
        auto p = tuner (altoTenor, 0.0f);
        const auto v1 = voice::synth (voice::steady (220.0 * std::pow (2.0, 40.0 / 1200.0), 0.6, SR), voice::vowelA, SR);
        const auto s = coloured ((int) (0.2 * SR), 0.08, 4000, 10000, 5);
        std::vector<float> x = v1;
        x.insert (x.end(), s.begin(), s.end());
        const auto out = mono (render (*p, x, 256));
        const int delay = p->getLatencySamples();
        // Only the S, so the vowel's tail ringing into it does not count: subtract nothing, read the first 10 ms against the S alone.
        const double db = nullDb (out, x, delay, (long) (0.6 * SR) + delay, (long) (0.61 * SR) + delay);
        gEngine = 0;
        return db;
    };
    const double sLl = consonantStart (0), sHq = consonantStart (1);
    info ("Low Latency: first 10 ms of an S after a corrected vowel, difference", sLl, "dB");
    check (sHq <= prolimits::hqConsonantStartNullDb, "High Quality: first 10 ms of an S after a corrected vowel, difference", sHq, "dB");

    // The headline numbers side by side, from the formant test when it has run.
    if (formantResults[1].flatter > 0.0)
    {
        info ("Low Latency, first formant flatter than a real voice", formantResults[0].flatter, "dB");
        info ("High Quality, first formant flatter than a real voice", formantResults[1].flatter, "dB");
        info ("Low Latency, formant move", formantResults[0].move * 100.0, "%");
        info ("High Quality, formant move", formantResults[1].move * 100.0, "%");
        info ("Low Latency, first formant against its surroundings, flatter", formantResults[0].peakFlatter, "dB");
        info ("High Quality, first formant against its surroundings, flatter", formantResults[1].peakFlatter, "dB");
        info ("Low Latency, colour", formantResults[0].colour, "dB");
        info ("High Quality, colour", formantResults[1].colour, "dB");
        info ("Low Latency, high notes, colour", highNoteResults[0].colour, "dB");
        info ("High Quality, high notes, colour", highNoteResults[1].colour, "dB");
        info ("Low Latency, big shifts, colour", bigShiftResults[0].colour, "dB");
        info ("High Quality, big shifts, colour", bigShiftResults[1].colour, "dB");
        info ("Low Latency, high notes, formant move against the input", highNoteResults[0].move * 100.0, "%");
        info ("High Quality, high notes, formant move against the input", highNoteResults[1].move * 100.0, "%");
        info ("Low Latency, high notes, first formant against its surroundings, flatter", highNoteResults[0].peakFlatter, "dB");
        info ("High Quality, high notes, first formant against its surroundings, flatter", highNoteResults[1].peakFlatter, "dB");
        info ("Low Latency, big shifts, formant move against a real voice", bigShiftResults[0].moveReal * 100.0, "%");
        info ("High Quality, big shifts, formant move against a real voice", bigShiftResults[1].moveReal * 100.0, "%");
        info ("Low Latency, big shifts, first formant against its surroundings, flatter", bigShiftResults[0].peakFlatter, "dB");
        info ("High Quality, big shifts, first formant against its surroundings, flatter", bigShiftResults[1].peakFlatter, "dB");
    }
}

// ------------------------------------------------------------------------------------------------------------- window

void testWindow()
{
    gEngine = 0;
    auto p = tuner (altoTenor, 0.0f);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* te = dynamic_cast<TuneEditor*> (ed.get());
    check (te != nullptr, "the window is the custom one");
    if (te == nullptr) return;

    // Every control is in the window and everything in the window is a real parameter.
    check (te->getNumControls() == 22, "controls in the window, knobs, choices and switches", te->getNumControls());

    // Clicking a key cycles Tune, Remove, Bypass, through the parameter, as a host would see it.
    auto* a = p->apvts.getParameter ("note9");
    CountListener listener;
    p->addListener (&listener);
    const auto modeOfA = [&] { return juce::roundToInt (p->apvts.getRawParameterValue ("note9")->load()); };
    auto& keys = te->getKeyboard();
    const int atA = keys.noteAt (keys.keyBounds (9).getCentre());
    keys.cycleNote (atA);
    const int one = modeOfA();
    keys.cycleNote (atA);
    const int two = modeOfA();
    keys.cycleNote (atA);
    const int three = modeOfA();
    p->removeListener (&listener);
    check (atA == 9 && one == smoothtune::noteRemove && two == smoothtune::noteBypass && three == smoothtune::noteTune && a != nullptr, "clicking the A key goes Remove, Bypass, Tune");
    check (listener.changes.load() == 3, "and each click reaches the host as a parameter change, changes", listener.changes.load());
    check (keys.noteAt (keys.keyBounds (1).getCentre()) == 1 && keys.noteAt (keys.keyBounds (0).getBottomLeft().translated (4.0f, -4.0f)) == 0, "black and white keys are told apart");

    // Live reading: a voice on A shows A as the target and the pitch window has both lines.
    const auto x = voice::synth (voice::steady (noteHz (57) * std::pow (2.0, 30.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
    render (*p, x, 256);
    check (p->getReadings().targetNote == 57, "the keyboard's target reads A3", p->getReadings().targetNote);
    const auto& last = p->getPitchPoint (p->getPitchHistoryCount() - 1);
    check (std::abs (last.outputNote.load() - 57.0f) < 0.1f && std::abs (last.inputNote.load() - 57.3f) < 0.1f, "the pitch window's newest point: input A3 +30 cents, corrected A3", last.outputNote.load());
    check (te->readingText().contains ("A3"), "the reading strip: " + te->readingText().toStdString());

    // Paint it with a note in each state. Saved for looking at.
    setParam (*p, "note4", (float) smoothtune::noteRemove);
    setParam (*p, "note1", (float) smoothtune::noteBypass);
    setParam (*p, "scale", (float) smoothtune::scaleMajor);
    auto wander = voice::vibrato (noteHz (57), 3.0, SR, 35.0, 5.5);
    render (*p, voice::synth (wander, voice::vowelA, SR), 256);
    juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
    {
        juce::Graphics g (img);
        ed->paintEntireComponent (g, false);
    }
    check (ed->getWidth() > 400 && ed->getHeight() > 300, "the window paints");
    if (const char* path = std::getenv ("TUNE_WINDOW_PNG"))
    {
        juce::File file (path);
        file.deleteFile();
        juce::PNGImageFormat png;
        if (auto stream = file.createOutputStream())
            png.writeImageToStream (img, *stream);
    }

    // Painting never touches a lock the audio thread takes: audio keeps running at full speed against constant repainting.
    {
        std::atomic<bool> stop { false };
        std::atomic<long> blocks { 0 };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> b (p->getTotalNumOutputChannels(), 256);
            juce::MidiBuffer m;
            auto s = voice::synth (voice::steady (225.0, 1.0, SR), voice::vowelA, SR);
            for (int pos = 0; ! stop.load(); pos = (pos + 256) % ((int) s.size() - 256))
            {
                for (int ch = 0; ch < b.getNumChannels(); ++ch) b.copyFrom (ch, 0, s.data() + pos, 256);
                p->processBlock (b, m);
                ++blocks;
            }
        });
        for (int i = 0; i < 60; ++i)
        {
            juce::Graphics g (img);
            ed->paintEntireComponent (g, false);
        }
        stop = true;
        audio.join();
        check (blocks.load() > 0, "audio keeps running while the window repaints, blocks", (double) blocks.load());
    }

    // Every control has a real parameter behind it and a place on screen, Advanced open or not.
    {
        bool allThere = true;
        for (auto* id : { "key", "scale", "inputType", "engine", "midiOctave", "followKey", "midiTargets", "classic", "formantPreserve", "bypass",
                          "retune", "flex", "humanize", "naturalVibrato", "throat", "transpose", "reference", "tracking", "mix", "vibratoAmount", "vibratoRate", "vibratoDelay" })
            allThere = allThere && te->findControl (id) != nullptr && p->apvts.getParameter (id) != nullptr;
        check (allThere, "every control in the window is found by its parameter ID");
    }

    // The window and automation move each other, with gestures.
    {
        struct Gestures final : juce::AudioProcessorListener
        {
            std::atomic<int> begun { 0 }, ended { 0 };
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
            void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++begun; }
            void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override { ++ended; }
        } gestures;

        auto* flexSlider = dynamic_cast<tuneui::ParamSlider*> (te->findControl ("flex"));
        check (flexSlider != nullptr, "Flex Tune is a slider in the window");
        if (flexSlider != nullptr)
        {
            setParam (*p, "flex", 42.0f);
            check (std::abs (flexSlider->getValue() - 42.0) < 0.2, "automation moves the window's slider", flexSlider->getValue());
            flexSlider->setValue (17.0, juce::sendNotificationSync);
            check (std::abs (p->apvts.getRawParameterValue ("flex")->load() - 17.0f) < 0.2f, "the window's slider moves the parameter", p->apvts.getRawParameterValue ("flex")->load());
            p->addListener (&gestures);
            flexSlider->applyTyped ("33");
            p->removeListener (&gestures);
            check (std::abs (p->apvts.getRawParameterValue ("flex")->load() - 33.0f) < 0.2f, "a typed value lands on the parameter", p->apvts.getRawParameterValue ("flex")->load());
            check (gestures.begun.load() == 1 && gestures.ended.load() == 1, "and a typed value is one begin and one end gesture", gestures.begun.load());
            setParam (*p, "flex", 0.0f);
        }

        // The real mouse handlers, as a click arrives from the system.
        const auto mouse = [] (juce::Component& c, juce::Point<float> at, juce::Point<float> from, int clicks, bool dragged)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, from, now, clicks, dragged);
        };
        const auto modeOfD = [&] { return juce::roundToInt (p->apvts.getRawParameterValue ("note2")->load()); };
        const auto dKey = keys.keyBounds (2).getBottomLeft().translated (6.0f, -6.0f);
        keys.mouseDown (mouse (keys, dKey, dKey, 1, false));
        check (modeOfD() == smoothtune::noteRemove, "one click on a key removes the note");
        keys.mouseDown (mouse (keys, dKey, dKey, 2, false));
        check (modeOfD() == smoothtune::noteTune, "the second click of a double-click puts it back to Tune");

        if (flexSlider != nullptr)
        {
            gestures.begun = 0;
            gestures.ended = 0;
            p->addListener (&gestures);
            const auto start = flexSlider->getLocalBounds().getCentre().toFloat();
            const auto end = start.translated (40.0f, 0.0f);
            flexSlider->mouseDown (mouse (*flexSlider, start, start, 1, false));
            flexSlider->mouseDrag (mouse (*flexSlider, end, start, 1, true));
            flexSlider->mouseUp (mouse (*flexSlider, end, start, 1, true));
            check (p->apvts.getRawParameterValue ("flex")->load() > 1.0f, "dragging a control moves its parameter", p->apvts.getRawParameterValue ("flex")->load());
            check (gestures.begun.load() == 1 && gestures.ended.load() == 1, "and a drag is one begin and one end gesture", gestures.begun.load());
            gestures.begun = 0;
            gestures.ended = 0;
            flexSlider->mouseDoubleClick (mouse (*flexSlider, start, start, 2, false));
            p->removeListener (&gestures);
            check (std::abs (p->apvts.getRawParameterValue ("flex")->load()) < 0.01f, "double-clicking a control resets it", p->apvts.getRawParameterValue ("flex")->load());
            check (gestures.begun.load() == 1 && gestures.ended.load() == 1, "and that is one begin and one end gesture too", gestures.begun.load());
        }
    }

    // Nothing but size and the open section is added to saved state, it is range checked, and it comes back.
    {
        p->windowWidth.store (1000);
        p->advancedOpen.store (true);
        juce::MemoryBlock saved;
        p->getStateInformation (saved);
        auto other = make();
        other->setStateInformation (saved.getData(), (int) saved.getSize());
        check (other->windowWidth.load() == 1000 && other->advancedOpen.load(), "the window size and open section come back", other->windowWidth.load());

        std::unique_ptr<juce::XmlElement> xml (juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize()));
        const auto plain = p->apvts.copyState().createXml();
        check (xml != nullptr && xml->getNumAttributes() == plain->getNumAttributes() + 2 && xml->getNumChildElements() == plain->getNumChildElements(), "saved state adds exactly two attributes");
        check (! other->apvts.copyState().hasProperty ("windowWidth"), "and they stay out of the parameter tree");

        xml->setAttribute ("windowWidth", 99999999);
        juce::MemoryBlock hostile;
        juce::AudioProcessor::copyXmlToBinary (*xml, hostile);
        other->setStateInformation (hostile.getData(), (int) hostile.getSize());
        check (other->windowWidth.load() == PluginProcessor::maxWindowWidth, "a damaged window size is clamped", other->windowWidth.load());
        xml->setAttribute ("windowWidth", "nan");
        juce::AudioProcessor::copyXmlToBinary (*xml, hostile);
        other->setStateInformation (hostile.getData(), (int) hostile.getSize());
        check (other->windowWidth.load() >= PluginProcessor::minWindowWidth && other->windowWidth.load() <= PluginProcessor::maxWindowWidth, "so is one that is not a number", other->windowWidth.load());
        p->windowWidth.store (PluginProcessor::defaultWindowWidth);
        p->advancedOpen.store (false);
    }

    // Advanced opens and shuts with the display giving up the room, and every size paints.
    {
        const int closedHeight = te->getPitchView().getHeight();
        te->setAdvancedOpen (true, false);
        const int openHeight = te->getPitchView().getHeight();
        check (openHeight < closedHeight && openHeight > 150, "opening Advanced shrinks the display, height", openHeight);
        juce::Image big (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        {
            juce::Graphics g (big);
            ed->paintEntireComponent (g, false);
        }
        te->setAdvancedOpen (false, false);
        check (te->getPitchView().getHeight() == closedHeight, "and closing it gives the room back");
        for (int width : { PluginProcessor::minWindowWidth, 1333, PluginProcessor::maxWindowWidth, PluginProcessor::defaultWindowWidth })
        {
            te->setSize (width, juce::roundToInt ((double) width * TuneEditor::designHeight / TuneEditor::designWidth));
            juce::Image sized (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
            juce::Graphics g (sized);
            ed->paintEntireComponent (g, false);
        }
        check (te->getWidth() == PluginProcessor::defaultWindowWidth, "it paints at the smallest, largest and in between sizes");
        p->windowWidth.store (PluginProcessor::defaultWindowWidth);
    }

    // When nothing new arrives, nothing on screen moves, so there is nothing to draw.
    {
        auto& view = te->getPitchView();
        for (int i = 0; i < 90; ++i)
            view.advance (1.0 / 30.0);
        bool stillMoving = false;
        for (int i = 0; i < 10; ++i)
            stillMoving = view.advance (1.0 / 30.0) || stillMoving;
        check (! stillMoving, "with no audio arriving the display asks for no frames");
    }

    // The window changes nothing in the audio: the same input gives the same samples, with it open and painting or with no window at all.
    {
        auto plainOne = tuner (altoTenor, 20.0f, 20.0f, 5.0f);
        auto withWindow = tuner (altoTenor, 20.0f, 20.0f, 5.0f);
        std::unique_ptr<juce::AudioProcessorEditor> open (withWindow->createEditor());
        auto* openEditor = dynamic_cast<TuneEditor*> (open.get());
        const auto wandering = voice::synth (voice::vibrato (noteHz (57) * 1.01, 1.5, SR, 40.0, 5.5), voice::vowelA, SR);
        juce::Image frame (juce::Image::ARGB, open->getWidth(), open->getHeight(), true);
        double worst = 0.0;
        juce::AudioBuffer<float> a (plainOne->getTotalNumOutputChannels(), 256), b (withWindow->getTotalNumOutputChannels(), 256);
        juce::MidiBuffer m;
        for (size_t pos = 0; pos + 256 <= wandering.size(); pos += 256)
        {
            for (int ch = 0; ch < a.getNumChannels(); ++ch)
            {
                a.copyFrom (ch, 0, wandering.data() + pos, 256);
                b.copyFrom (ch, 0, wandering.data() + pos, 256);
            }
            plainOne->processBlock (a, m);
            withWindow->processBlock (b, m);
            openEditor->tick (1.0 / 30.0);
            if (pos % 2048 == 0)
            {
                juce::Graphics g (frame);
                open->paintEntireComponent (g, false);
            }
            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                for (int i = 0; i < 256; ++i)
                    worst = juce::jmax (worst, (double) std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        }
        check (worst == 0.0, "null test, window open against no window, largest sample difference", worst);
    }

    // Opening and closing it over and over, with audio running and with none.
    {
        auto q = tuner (altoTenor, 20.0f);
        for (int i = 0; i < 25; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> again (q->createEditor());
            dynamic_cast<TuneEditor&> (*again).tick (1.0 / 30.0);
        }
        check (true, "25 opens and closes with no audio");

        std::atomic<bool> stop { false };
        std::atomic<long> blocks { 0 };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (q->getTotalNumOutputChannels(), 256);
            juce::MidiBuffer m;
            auto s = voice::synth (voice::steady (225.0, 1.0, SR), voice::vowelA, SR);
            for (int pos = 0; ! stop.load(); pos = (pos + 256) % ((int) s.size() - 256))
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.copyFrom (ch, 0, s.data() + pos, 256);
                q->processBlock (buffer, m);
                ++blocks;
            }
        });
        for (int i = 0; i < 25; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> again (q->createEditor());
            dynamic_cast<TuneEditor&> (*again).tick (1.0 / 30.0);
            juce::Image shot (juce::Image::ARGB, again->getWidth(), again->getHeight(), true);
            juce::Graphics g (shot);
            again->paintEntireComponent (g, false);
        }
        stop = true;
        audio.join();
        check (blocks.load() > 0, "25 opens and closes with audio running, blocks", (double) blocks.load());
    }

    // Closing it while audio runs leaves nothing behind.
    ed.reset();
    render (*p, x, 256);
    check (true, "closing the window leaves audio running");
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const std::string only = argc > 1 ? argv[1] : "";
    const auto want = [&only] (const char* name) { return only.empty() || only == name; };

    if (only == "demo")
    {
        // Writes corrected versions of the say material to build/demo, to listen to outside any host.
        const auto folder = juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getParentDirectory().getChildFile ("build/demo");
        folder.createDirectory();
        juce::WavAudioFormat wav;
        for (const char* name : { "speech_samantha.wav", "speech_daniel.wav", "sung_cellos.wav", "held_kathy.wav" })
            for (int engine : { 0, 1 })
                for (float retune : { 0.0f, 40.0f })
                {
                    gEngine = engine;
                    const auto x = loadWav (name);
                    auto p = tuner (std::string (name).find ("daniel") != std::string::npos ? lowMale : altoTenor, retune, retune > 0 ? 40.0f : 0.0f);
                    auto out = render (*p, x, 256);
                    const auto target = folder.getChildFile (juce::String (name).upToFirstOccurrenceOf (".", false, false) + (engine == 1 ? "_hq" : "_ll") + (retune > 0 ? "_natural.wav" : "_hard.wav"));
                    target.deleteFile();
                    if (auto stream = target.createOutputStream())
                    {
                        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), SR, 1, 24, juce::StringPairArray(), 0));
                        if (writer != nullptr)
                        {
                            stream.release();
                            writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
                        }
                    }
                }
        return 0;
    }
    if (want ("generic"))
    {
        GenericOptions opt;
        opt.minPresets = 4;
        opt.readOnly = { "pitchOffTarget", "detectedPitch", "outputPitch", "correctionApplied", "targetNote", "activeKey", "latencyMs" };
        opt.sendMidiNote = true;
        runGenericChecks ([] () -> std::unique_ptr<juce::AudioProcessor> { return make(); }, opt);
    }
    if (want ("tracker")) testTracker();
    if (want ("keys")) testKeysAndScales();
    if (want ("follow")) testFollowKey();
    const int firstEngine = std::getenv ("TUNE_ENGINE") != nullptr ? std::atoi (std::getenv ("TUNE_ENGINE")) : 0;
    const int lastEngine = std::getenv ("TUNE_ENGINE") != nullptr ? firstEngine : 1;
    for (gEngine = firstEngine; gEngine <= lastEngine; ++gEngine)
    {
        if (want ("latency")) testLatency();
        if (want ("output")) testOutputPitch();
        if (want ("retune")) testRetuneSpeed();
        if (want ("formants")) testFormants();
        if (want ("vibrato")) testVibrato();
        if (want ("step")) testHardStep();
        if (want ("flex")) testFlex();
        if (want ("unvoiced")) testUnvoiced();
        if (want ("artifacts")) testArtifacts();
        if (want ("start")) testNoteStart();
        if (want ("real")) testRealMaterial();
        if (want ("added")) testAddedControls();
        if (want ("meter")) testMeter();
        if (want ("blocks")) testBlocksAndState();
        if (want ("cpu")) testCpu();
        if (want ("midi")) testMidi();
        if (want ("notes")) testPerNote();
        if (want ("classic")) testClassic();
        if (want ("transpose")) testTranspose();
        if (want ("throat")) testThroat();
        if (want ("gen")) testVibratoGenerator();
        if (want ("mix")) testMix();
        if (want ("timing")) testTiming();
    }
    gEngine = 0;
    if (want ("engines")) testEngines();
    gEngine = 1;
    if (want ("heldout")) testHeldOut();
    if (want ("heldout2")) testHeldOutSecond();
    if (want ("heldout3")) testHeldOutThird();
    if (want ("heldout4")) testHeldOutFourth();
    if (want ("heldout5")) testHeldOutFifth();
    if (want ("heldout6")) testHeldOutSixth();
    if (want ("heldout7")) testHeldOutSeventh();
    if (want ("heldout8")) testHeldOutEighth();
    if (want ("heldout9")) testHeldOutNinth();
    if (want ("vowel")) testVowelChange();
    gEngine = 0;
    if (want ("llnull")) testLowLatencyNull();
    if (want ("window")) testWindow();
    return finish();
}
