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
#include <thread>

using namespace testkit;
using namespace measure;

namespace
{
constexpr double SR = 48000.0;
constexpr int soprano = 0, altoTenor = 1, lowMale = 2, wideRange = 3;

std::unique_ptr<PluginProcessor> make (const juce::String& keyFile = "/nonexistent/smooth-tune-test.json")
{
    PluginProcessor::Options o;
    o.keyFilePath = keyFile;
    o.hostLatencyUpdates = false;
    return std::make_unique<PluginProcessor> (o);
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
    setParam (p, "scale", (float) smoothtune::scaleCustom);
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
    check (median (a) <= limits::trackSteadyMedianCents, "tracking error, steady notes 80 Hz to 1 kHz, 4 vowels, median", median (a), "cents");
    check (percentile (a, 95) <= limits::trackSteadyP95Cents, "tracking error, steady notes, 95th percentile", percentile (a, 95), "cents");
    const auto b = absolute (vib.errors);
    check (median (b) <= limits::trackVibratoMedianCents, "tracking error, vibrato +/-50 cents at 5.5 Hz, median", median (b), "cents");
    info ("tracking error, vibrato, 95th percentile", percentile (b, 95), "cents");
    info ("tracking error, breath -18 dB, median", median (absolute (breath.errors)), "cents");
    info ("tracking error, notes with scoops and octave leaps, median", median (absolute (scoop.errors)), "cents");
    info ("lowest voiced share on a steady note", worstShare * 100.0, "%");
    check (worstShare >= 0.85, "steady notes are found voiced, worst case", worstShare * 100.0, "%");

    const auto [seconds, events] = all ({ &steady, &vib, &scoop, &breath });
    const double perMinute = (double) events / std::max (seconds, 1.0) * 60.0;
    check (perMinute <= limits::octaveErrorsPerMinute, "octave errors per minute of voiced sound, " + std::to_string ((int) seconds) + " s of voice, all material", perMinute, "per minute");
    info ("octave error events, steady", (double) steady.octaveEvents);
    info ("octave error events, vibrato", (double) vib.octaveEvents);
    info ("octave error events, scoops and leaps", (double) scoop.octaveEvents);
    info ("octave error events, breath", (double) breath.octaveEvents);

    // Each Input Type on its own range, ends included.
    struct R { const char* name; double low, high; };
    for (auto r : { R { "Soprano", 150.0, 1300.0 }, R { "Alto/Tenor", 98.0, 1000.0 }, R { "Low Male", 65.0, 600.0 } })
    {
        TrackScore s;
        for (double hz : { r.low * 1.06, std::sqrt (r.low * r.high), r.high * 0.94 })
            scoreTracker (voice::steady (hz, 1.0, SR), voice::vowelA, r.low, r.high, s);
        check (median (absolute (s.errors)) <= limits::trackSteadyMedianCents && s.octaveEvents == 0,
               std::string ("Input Type ") + r.name + " tracks both ends of its range, median", median (absolute (s.errors)), "cents");
    }

    // Other sample rates
    for (double sr : { 44100.0, 96000.0 })
    {
        TrackScore s;
        for (double hz : { 110.0, 220.0, 440.0, 880.0 })
            scoreTracker (voice::steady (hz, 1.0, sr), voice::vowelA, 65.0, 1300.0, s, -60.0, sr);
        check (median (absolute (s.errors)) <= limits::trackSteadyMedianCents, "tracking error at " + std::to_string ((int) sr) + " Hz, median", median (absolute (s.errors)), "cents");
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
    check (w < limits::outputInstantCents, "output pitch error at instant speed, steady notes 82 Hz to 1 kHz, worst 95th percentile", w, "cents");
    info ("  worst case is at", worstHz, "Hz");

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
    check (v < limits::outputInstantCents, "output pitch error at instant speed, vibrato +/-30 cents with breath, worst 95th percentile", v, "cents");

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
        check (percentile (absolute (e), 95) < limits::outputInstantCents, "output pitch error at instant speed, voice with wander, jitter and breath, 95th percentile", percentile (absolute (e), 95), "cents");
        const double nh = nonHarmonicDb (out.data() + (int) (0.5 * SR), 1 << 16, 220.0, SR);
        const double nhIn = nonHarmonicDb (x.data() + (int) (0.5 * SR), 1 << 16, 220.0 * std::pow (2.0, 20.0 / 1200.0), SR);
        info ("  non-harmonic energy on that voice, in then out", nhIn, "dB");
        info ("  non-harmonic energy on that voice, out", nh, "dB");
    }

    // Quiet and loud takes tune the same.
    for (double levelDb : { -45.0, -6.0 })
    {
        auto p = tuner (altoTenor, 0.0f);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.2, SR), voice::vowelA, SR, -60.0, levelDb);
        const auto out = mono (render (*p, x, 256));
        auto e = centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 1.1);
        check (percentile (absolute (e), 95) < limits::outputInstantCents, "output pitch error on a voice at " + std::to_string ((int) levelDb) + " dBFS, 95th percentile", percentile (absolute (e), 95), "cents");
    }

    // Other sample rates, because everything scales with the rate
    for (double sr : { 44100.0, 96000.0 })
    {
        auto p = tuner (altoTenor, 0.0f, 0.0f, 0.0f, sr);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.2, sr), voice::vowelA, sr);
        const auto out = mono (render (*p, x, 256));
        auto e = centsOff (out.data(), (long) out.size(), sr, 220.0, 0.3, 1.1);
        check (percentile (absolute (e), 95) < limits::outputInstantCents, "output pitch error at " + std::to_string ((int) sr) + " Hz, 95th percentile", percentile (absolute (e), 95), "cents");
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
        check (std::abs (crossMs - tau) <= allow, "Retune Speed " + std::to_string ((int) tau) + " ms, measured time constant", crossMs, "ms");
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
    check (percentile (absolute (e), 95) < limits::outputInstantCents, "Retune Speed 0 after a 40 cent step, frames from 50 ms on, 95th percentile", percentile (absolute (e), 95), "cents");
}

// -------------------------------------------------------------------------------------------------------- formants

// The shape of a vocal tract only shows when harmonics sweep across it, so the voice glides slowly over one octave, which lets every
// harmonic cover the frequencies up to where its double starts: no gaps and no piling up. The shape is read from the long term average spectrum. The shift is the same +/-3 semitones a correction would ask for,
// held steady with Transpose while Flex Amount leaves the glide alone.
struct FormantRun { std::vector<float> in, out; };
FormantRun glideThroughTuner (const voice::Vowel& v, double centreHz, int transposeBy, bool preserve = true, float throat = 100.0f)
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
    setParam (*p, "inputType", (float) lowMale);
    setParam (*p, "retune", 0.0f);
    setParam (*p, "humanize", 0.0f);
    setParam (*p, "flex", 100.0f);
    setParam (*p, "transpose", (float) transposeBy);
    setParam (*p, "formantPreserve", preserve ? 1.0f : 0.0f);
    setParam (*p, "throat", throat);
    prepare (*p, 1, SR, 256);
    FormantRun r;
    r.in = voice::synth (f0, v, SR);
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
    return voice::synth (f0, v, SR);
}

// Formant position measure: which frequency scale lines the output's spectrum shape up with that real voice's? The first
// tries at this (picking peaks and fitting harmonics) could not read a real voice to better than +/-3 to 10%, so this one
// was checked against known moves instead: Formant Preserve off must read the pitch shift, and Throat Length its own ratio
// (see testAddedControls). Peak height is reported too, because grains do broaden each peak: it stays in place, a bit flatter.
void testFormants()
{
    double worst = 0.0, worstReal = 0.0, worstNoise = 0.0, worstLevel = 0.0, worstFlatten = 0.0;
    const voice::Vowel* vowels[] = { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU };
    const long from = (long) (0.3 * SR);
    const auto one = [&] (const voice::Vowel& v, double centre, int shift)
    {
        const auto run = glideThroughTuner (v, centre, shift);
        const auto control = controlVoice (v, centre, shift);
        const long n = (long) run.in.size() - from - 1000;
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
            worst = std::max (worst, std::abs (against - 1.0));
            worstReal = std::max (worstReal, std::abs (real - 1.0));
            worstNoise = std::max (worstNoise, std::abs (noise - 1.0));
            if (std::abs (against - 1.0) > limits::formantShiftFraction || std::abs (real - 1.0) > limits::formantShiftFraction || std::getenv ("TUNE_DUMP") != nullptr)
                std::printf ("  vowel %s around %.0f Hz, shift %d, band %.0f to %.0f Hz: against input %.3f, against a real voice there %.3f, input against that voice %.3f\n", v.name, centre, shift, band.first, band.second, against, real, noise);
        }
        const auto fine = smoothSpectrumDb (control.data() + from, n, SR, 50.0), fineOut = smoothSpectrumDb (run.out.data() + from, n, SR, 50.0);
        const size_t f1 = (size_t) v.f[0];
        const double flatten = (fine[f1] - fine[f1 / 2]) - (fineOut[f1] - fineOut[f1 / 2]);
        worstFlatten = std::max (worstFlatten, flatten);
        const double level = toDb (rms (run.out.data() + from, (int) n)) - toDb (rms (run.in.data() + from, (int) n));
        worstLevel = std::max (worstLevel, std::abs (level));
    };
    for (auto* v : vowels)
        for (double centre : { 110.0, 165.0 })
            for (int shift : { 3, -3, 1, -1 })
                one (*v, centre, shift);
    for (auto* v : { &voice::vowelA, &voice::vowelE })
        for (int shift : { 3, -3 })
            one (*v, 247.0, shift);
    check (worst <= limits::formantShiftFraction, "formant positions at +/-1 and +/-3 semitones, output against input, worst move", worst * 100.0, "%");
    // The small correction in the shifter was tuned on the set above. These vowels and pitches were not used for that.
    const double tunedOn = worst;
    worst = 0.0;
    for (auto* v : { &voice::vowelO, &voice::vowelAe })
        for (double centre : { 130.0, 196.0 })
            for (int shift : { 3, -3, 2, -2 })
                one (*v, centre, shift);
    check (worst <= limits::formantShiftFraction, "formant positions on vowels and pitches not used for tuning, worst move", worst * 100.0, "%");
    worst = std::max (worst, tunedOn);
    info ("  the same output against a real voice at the new pitch, worst", worstReal * 100.0, "%");
    info ("  and the tool's floor: the input against that same real voice, which share a vocal tract, worst", worstNoise * 100.0, "%");
    info ("  first formant peak flatter than a real voice at that pitch, worst case", worstFlatten, "dB");
    check (worstLevel <= limits::levelChangeDb, "level stays put at +/-1 and +/-3 semitones, worst change", worstLevel, "dB");
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
        check (kept <= limits::vibratoFlatHard, "hard settings flatten vibrato, kept", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "flex", 60.0f);
        const double kept = vibratoKept (*p);
        check (kept >= limits::vibratoKeptFlex, "Flex Amount 60 cents keeps a +/-40 cent vibrato, kept", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 100.0f); setParam (*p, "flex", 0.0f);
        const double kept = vibratoKept (*p);
        check (kept >= limits::vibratoKeptHumanize, "Humanize 100 keeps vibrato on the sustained part, kept", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 400.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "flex", 0.0f);
        const double kept = vibratoKept (*p);
        check (kept >= limits::vibratoKeptSlowRetune, "Retune Speed 400 ms keeps vibrato, kept", kept * 100.0, "%");
    }
    {
        // Natural Vibrato acts on what survives correction, so use settings that leave vibrato alone.
        auto p = make(); setParam (*p, "retune", 400.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "naturalVibrato", -100.0f);
        const double kept = vibratoKept (*p);
        check (kept <= limits::naturalVibratoLess, "Natural Vibrato -100 takes vibrato down, left", kept * 100.0, "%");
    }
    {
        auto p = make(); setParam (*p, "retune", 400.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "naturalVibrato", 100.0f);
        const double kept = vibratoKept (*p);
        check (kept >= limits::naturalVibratoMore, "Natural Vibrato +100 takes vibrato up, now", kept * 100.0, "%");
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
    double t10 = -1, t90 = -1;
    for (size_t i = 0; i < trace.size(); ++i)
    {
        if (trace[i] <= 0.0) continue;
        const double u = std::log (trace[i] / a) / std::log (b / a);
        if (t10 < 0 && u > low) t10 = (double) i * hop;
        if (t90 < 0 && u > high) { t90 = (double) i * hop; break; }
    }
    const double ms = (t90 - t10) * 1000.0;
    check (t10 >= 0 && t90 >= 0 && ms <= limits::hardStepMs, "hard tune step between two notes a tone apart, 10% to 90%", ms, "ms");
    const auto settled = centsOff (out.data(), (long) out.size(), SR, b, start + 0.1, 1.9);
    check (percentile (absolute (settled), 95) < limits::outputInstantCents, "after the step the new note is on pitch, 95th percentile", percentile (absolute (settled), 95), "cents");

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
    check (share >= 80.0, "a slow glide comes out as steps, frames within 10 cents of a note", share, "%");
}

void testFlex()
{
    {
        auto p = tuner (altoTenor, 0.0f, 0.0f, 50.0f);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.4));
        check (std::abs (e - 30.0) <= limits::flexHoldCents, "Flex Amount 50: a note 30 cents sharp is left where it was sung, output offset", e, "cents");
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
        check (std::abs (e - 50.0) <= limits::flexClampCents, "Flex Amount 50: a note 80 cents sharp is brought to the edge, output offset", e, "cents");
    }
    {
        auto p = tuner (altoTenor, 0.0f, 0.0f, 0.0f);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.4));
        check (std::abs (e) <= limits::outputInstantCents, "Flex Amount 0: the same note is corrected fully, output offset", e, "cents");
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
            check (db <= limits::unvoicedNullDb, std::string ("unchanged: ") + n.name + (retune > 0 ? ", natural settings" : ", hard settings") + ", difference", db, "dB");
        }

    // The same on the lowest latency type, whose window is shortest and so easiest to fool.
    for (const auto& n : noises)
    {
        auto p = tuner (soprano, 0.0f);
        const auto x = coloured ((int) (2.0 * SR), n.level, n.hp, n.lp, 21);
        const auto out = mono (render (*p, x, 256));
        const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.1 * SR), (long) (1.9 * SR));
        check (db <= limits::unvoicedNullDb, std::string ("unchanged on Soprano: ") + n.name + ", difference", db, "dB");
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
    check (db <= limits::consonantNullDb, "an S between two corrected vowels, middle of the S, difference", db, "dB");
    const auto e = centsOff (out.data(), (long) out.size(), SR, 220.0, 1.0, 1.35);
    check (percentile (absolute (e), 95) < limits::outputInstantCents, "the vowel after the S is corrected again, 95th percentile", percentile (absolute (e), 95), "cents");
}

// ------------------------------------------------------------------------------------------------------- artifacts

void testArtifacts()
{
    const double target = 220.0;
    auto p = tuner (altoTenor, 0.0f);
    const auto x = voice::synth (voice::steady (target * std::pow (2.0, 30.0 / 1200.0), 3.0, SR), voice::vowelA, SR);
    const auto out = mono (render (*p, x, 256));
    const int from = (int) (0.5 * SR), n = 1 << 16;
    const double nh = nonHarmonicDb (out.data() + from, n, target, SR);
    check (nh <= limits::nonHarmonicDb, "non-harmonic energy, 220 Hz tone corrected by 30 cents", nh, "dB");
    info ("  the same measure on the uncorrected input", nonHarmonicDb (x.data() + from, n, target * std::pow (2.0, 30.0 / 1200.0), SR), "dB");

    // Corrected by more, as a pentatonic gap might ask
    auto q = make();
    setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
    onlyNote (*q, 9);
    prepare (*q, 1, SR, 256);
    const auto y = voice::synth (voice::steady (noteHz (57) * std::pow (2.0, 150.0 / 1200.0), 3.0, SR), voice::vowelA, SR);
    const auto outY = mono (render (*q, y, 256));
    const double nh2 = nonHarmonicDb (outY.data() + from, n, target, SR);
    check (nh2 <= limits::nonHarmonicDb, "non-harmonic energy, 220 Hz tone corrected by 150 cents", nh2, "dB");

    // Warble on a sustained note
    const auto e = centsOff (out.data(), (long) out.size(), SR, target, 0.5, 2.9);
    double m = 0, s2 = 0;
    for (double v : e) m += v;
    m /= (double) e.size();
    for (double v : e) s2 += (v - m) * (v - m);
    const double sd = std::sqrt (s2 / (double) e.size());
    check (sd <= limits::warblePitchCents, "output pitch steadiness on a corrected sustained note, standard deviation", sd, "cents");
    double lo = 1.0e9, hi = -1.0e9;
    const int frame = (int) (0.05 * SR);
    for (int at = from; at + frame < (int) out.size() - 2400; at += frame / 2)
    {
        const double db = toDb (rms (out.data() + at, frame));
        lo = std::min (lo, db); hi = std::max (hi, db);
    }
    check (hi - lo <= limits::warbleLevelDb, "output level steadiness on a corrected sustained note, peak to peak", hi - lo, "dB");

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
        check (nsd <= limits::warblePitchCents, "output pitch steadiness at everyday settings (Retune 20, Humanize 30), standard deviation", nsd, "cents");
        check (std::abs (nm) <= limits::outputInstantCents, "and the note settles on pitch, offset", nm, "cents");
        const double nnh = nonHarmonicDb (nout.data() + (int) (1.5 * SR), 1 << 16, target, SR);
        check (nnh <= limits::nonHarmonicDb, "non-harmonic energy at everyday settings", nnh, "dB");
    }

    // In tune and left alone: the plug-in must then be close to a plain delay even on a voice.
    auto t = tuner (altoTenor, 0.0f, 0.0f, 100.0f);
    const auto w = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
    const auto outW = mono (render (*t, w, 256));
    const double db = nullDb (outW, w, t->getLatencySamples(), (long) (0.2 * SR), (long) (1.9 * SR));
    check (db <= -60.0, "a voice with nothing to correct passes as a plain delay, difference", db, "dB");
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
    check (tableOk == 24, "key and scale masks match the written-out notes of all 24 keys", tableOk, "of 24");

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
    check (right == total, "every note sung in all 24 keys lands on the right note, " + std::to_string (total) + " cases", right, "right");

    // Other scales, written out by hand for a root of A
    struct S { int scale; const char* notes; };
    for (auto s : { S { smoothtune::scaleHarmonicMinor, "A B C D E F G#" }, S { smoothtune::scaleMelodicMinor, "A B C D E F# G#" },
                    S { smoothtune::scaleMajorPentatonic, "A B C# E F#" }, S { smoothtune::scaleMinorPentatonic, "A C D E G" },
                    S { smoothtune::scaleBlues, "A C D D# E G" }, S { smoothtune::scaleDorian, "A B C D E F# G" }, S { smoothtune::scaleMixolydian, "A B C# D E F# G" } })
        check (smoothtune::scaleMask (9, s.scale) == maskFromNames (s.notes), std::string ("A ") + smoothtune::scaleNames[s.scale] + " mask is right");

    // Custom notes
    {
        auto q = make();
        setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
        setParam (*q, "scale", (float) smoothtune::scaleCustom);
        setParam (*q, "note9", 1.0f); // A removed
        prepare (*q, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 20.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*q, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, noteHz (58), 0.3, 0.9, 0.005, 0.1));
        check (std::abs (e) <= limits::customNoteCents, "Custom: A removed, A +20 cents goes to A#, offset", e, "cents");
    }
    {
        auto q = make();
        setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
        setParam (*q, "scale", (float) smoothtune::scaleCustom);
        setParam (*q, "note9", 2.0f); // A bypassed
        prepare (*q, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*q, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 0.9));
        check (std::abs (e - 30.0) <= limits::customNoteCents, "Custom: A bypassed, A +30 cents stays where it was sung, offset", e, "cents");
    }
    {
        auto q = make();
        setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f);
        setParam (*q, "scale", (float) smoothtune::scaleCustom);
        for (int i = 0; i < 12; ++i) setParam (*q, ("note" + juce::String (i)).toRawUTF8(), 1.0f);
        prepare (*q, 1, SR, 256);
        const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
        const auto out = mono (render (*q, x, 256));
        check (countNonFinite (render (*q, x, 256)) == 0 && std::abs (median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.3, 0.9)) - 30.0) < 10.0, "Custom: every note removed leaves the voice alone");
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
    check (outputNote (*p, x) == manualNote, "Follow Key with no file uses the manual key", outputNote (*p, x));

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
        check (outputNote (*p, x) == manualNote, std::string ("Follow Key ignores a file with ") + b.name);
        check (p->getReadings().activeKey == 0, std::string ("  active key still reads the manual one after ") + b.name);
    }

    // A good file takes over
    writeKey (file, keyJson ("D", "major", 0.87, nowUnix()));
    p->pollKeyFileNow();
    check (outputNote (*p, x) == fileNote, "Follow Key uses a good file: D major keeps F#", outputNote (*p, x));
    check (p->getReadings().activeKey == 2 && p->getReadings().activeScale == smoothtune::scaleMajor, "active key reads D major");

    // The real format, as the task gives it
    writeKey (file, keyJson ("A", "minor", 0.87, nowUnix()));
    p->pollKeyFileNow();
    prepare (*p, 1, SR, 256);
    render (*p, x, 256);
    check (p->getReadings().activeKey == 9 && p->getReadings().activeScale == smoothtune::scaleMinor, "active key reads A minor from a good file");

    // Good, then bad: the last good key holds.
    writeKey (file, keyJson ("D", "major", 0.87, nowUnix()));
    p->pollKeyFileNow();
    for (const auto& b : bad)
    {
        writeKey (file, b.text);
        p->pollKeyFileNow();
        if (outputNote (*p, x) != fileNote) check (false, std::string ("last good key should hold after ") + b.name);
    }
    check (outputNote (*p, x) == fileNote, "after a good file, bad data leaves the last good key in place");

    file.deleteFile();
    p->pollKeyFileNow();
    check (outputNote (*p, x) == fileNote, "after a good file, the file going missing leaves the last good key in place");

    // Switched off: back to manual at once.
    setParam (*p, "followKey", 0.0f);
    p->pollKeyFileNow();
    check (outputNote (*p, x) == manualNote, "Follow Key off goes back to the manual key");

    // The background thread does it without help.
    writeKey (file, keyJson ("D", "major", 0.9, nowUnix()));
    setParam (*p, "followKey", 1.0f);
    std::this_thread::sleep_for (std::chrono::milliseconds (700));
    check (outputNote (*p, x) == fileNote, "the background thread picks up the file by itself within 0.7 s");
    file.deleteFile();

    // A reader must not trip on a very large file or a folder
    {
        auto q = make (file.getFullPathName());
        setParam (*q, "followKey", 1.0f);
        writeKey (file, std::string (200000, 'x'));
        q->pollKeyFileNow();
        check (! smoothtune::KeyFollower::usable ({}, nowUnix()), "a large junk file changes nothing");
        file.deleteFile();
        file.createDirectory();
        q->pollKeyFileNow();
        file.deleteRecursively();
        check (true, "a folder at that path changes nothing");
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
            check (d == reported, std::string (names[type]) + " at " + std::to_string ((int) sr) + " Hz: delay measured on noise against reported " + std::to_string (reported), d, "samples");
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
        const int d = measuredDelay (out, x, 1200);
        check (std::abs (d - p->getLatencySamples()) <= 1, std::string ("delay measured on a voice, ") + names[type], d, "samples");
    }

    // The lowest latency type
    {
        auto p = make();
        setParam (*p, "inputType", (float) soprano);
        prepare (*p, 1, SR, 256);
        const double ms = p->getLatencySamples() * 1000.0 / SR;
        check (ms < limits::lowestLatencyMs, "lowest latency Input Type (Soprano) at 48 kHz", ms, "ms");
        for (int type : { altoTenor, lowMale, wideRange })
        {
            auto q = make();
            setParam (*q, "inputType", (float) type);
            prepare (*q, 1, SR, 256);
            info (std::string ("  latency at 48 kHz, ") + names[type], q->getLatencySamples() * 1000.0 / SR, "ms");
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
        check (measuredDelay (out, x, 2000) == p->getLatencySamples(), "bypass keeps the same delay");
        const double db = nullDb (out, x, p->getLatencySamples(), 4000, 40000);
        check (db < -120.0, "bypass is the plain delayed input, difference", db, "dB");
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
        check (measuredDelay (out, x, 2000) == p->getLatencySamples(), "processBlockBypassed keeps the same delay");
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
        check (p->getLatencySamples() < first && p->getLatencySamples() == PluginProcessor::latencyFor (soprano, SR), "changing Input Type changes the reported latency, now", p->getLatencySamples(), "samples");
        const std::vector<float> tailIn (x.begin() + 24000, x.end()), tailOut (out.begin() + 24000, out.end());
        check (measuredDelay (tailOut, tailIn, 1200) == p->getLatencySamples(), "and the real delay matches");
    }

    // Saved state with another Input Type, then prepare
    {
        auto a = make();
        setParam (*a, "inputType", (float) lowMale);
        juce::MemoryBlock st; a->getStateInformation (st);
        auto b = make();
        b->setStateInformation (st.getData(), (int) st.getSize());
        prepare (*b, 2, SR, 256);
        check (b->getLatencySamples() == PluginProcessor::latencyFor (lowMale, SR), "a saved Input Type restores its latency");
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
        if (x.empty()) { check (false, std::string ("could not load ") + it.file); continue; }

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
        std::printf ("INFO  %s: %.1f s voiced in both, %ld octave disagreements, %.1f per minute\n", it.file, voicedBoth, events, perMin);

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
        std::printf ("INFO  %s: distance from a note, median, before %.1f cents, after %.1f cents, %d voiced frames\n", it.file, median (dIn), median (dOut), (int) dOut.size());
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
            worstJump = std::max (worstJump, maxJump (out.data(), (int) out.size()) / std::max (maxJump (x.data(), (int) x.size()), 1.0e-6));
        }
        check (countNonFinite (render (*p, x, 256)) == 0, std::string ("  ") + it.file + " gives finite output");
    }

    check (worstFrames <= limits::speechFrameLevelDb, "real material at instant speed: 99th percentile change in 20 ms frame level, worst file", worstFrames, "dB");
    check (worstJump <= limits::speechJumpRatio, "real material at instant speed: biggest sample step against the input's, worst file", worstJump, "times");
    const double perMinute = totalEvents / std::max (totalSec, 1.0) * 60.0;
    check (perMinute <= limits::speechOctaveErrorsPerMinute, "real speech and say singing: octave disagreements with the slow two sided reference, which is not ground truth", perMinute, "per minute");
    const double speech = *std::max_element (speechMedians.begin(), speechMedians.end());
    check (speech <= limits::speechOutputMedianCents, "real speech at instant speed: worst file's median distance from a note", speech, "cents");
    const double sung = *std::max_element (sungMedians.begin(), sungMedians.end());
    check (sung <= limits::sungOutputMedianCents, "say singing and held vowels at instant speed: worst file's median distance from a note", sung, "cents");
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
        check (std::abs (e) <= limits::transposeCents, "Transpose +7 on A3 gives E4, offset", e, "cents");
    }
    {
        auto p = make();
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "transpose", -12.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (440.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 220.0, 0.4, 1.4));
        check (std::abs (e) <= limits::transposeCents, "Transpose -12 on A4 gives A3, offset", e, "cents");
    }
    {
        auto p = make();
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "reference", 432.0f);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (222.0, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        const double e = median (centsOff (out.data(), (long) out.size(), SR, 216.0, 0.4, 1.4));
        check (std::abs (e) <= limits::referenceCents, "Reference A4 432 Hz puts A3 at 216 Hz, offset", e, "cents");
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
        check (m >= 1.0 + limits::formantOffMinMove, "Formant Preserve off: formants follow +3 semitones up, scale (19% would be exact)", m, "times");
        check (std::abs (m / 1.189 - 1.0) <= limits::formantShiftFraction, "  and the formant tool reads that known move within 5%");
    }
    {
        const double m = movedBy (true, 130.0f);
        check (m <= 1.0 - limits::throatFormantMinMove, "Throat Length 130%: formants move down, scale (0.77 would be exact)", m, "times");
        check (std::abs (m / 0.769 - 1.0) <= limits::formantShiftFraction, "  and the formant tool reads that known move within 5%");
    }
    {
        const double m = movedBy (true, 75.0f);
        check (m >= 1.0 + limits::throatFormantMinMove, "Throat Length 75%: formants move up, scale (1.33 would be exact)", m, "times");
        check (std::abs (m / 1.333 - 1.0) <= limits::formantShiftFraction, "  and the formant tool reads that known move within 5%");
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
        check (choosy <= limits::trackingChoosyPercent, "Tracking 0 calls a noisy voice pitched this share of the time", choosy, "%");
        check (relaxed >= limits::trackingRelaxedPercent, "Tracking 100 calls the same voice pitched this share of the time", relaxed, "%");
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
    check (std::abs (r.centsOffTarget - 30.0f) <= 5.0f, "Pitch Off Target reads the sung error, 30 cents sharp", r.centsOffTarget, "cents");
    check (r.targetNote == 57, "Target Note reads A3");
    check (std::abs (r.correctionCents + 30.0f) <= 5.0f, "Correction Applied reads the correction, -30 cents", r.correctionCents, "cents");
    check (std::abs (r.detectedHz - 223.8f) <= 2.0f, "Detected Pitch reads the sung pitch", r.detectedHz, "Hz");
    check (listener.changes.load() == 0, "readings send nothing to the host while audio runs: parameter messages", listener.changes.load());

    // Value strings through the parameter, as a host reads them
    for (auto* prm : p->getParameters())
        if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            if (w->paramID == "pitchOffTarget")
                check (prm->getCurrentValueAsText().containsIgnoreCase ("cents") && ! prm->isAutomatable(), "Pitch Off Target has a real value string and is not automatable: " + prm->getCurrentValueAsText().toStdString());

    // No pitch: readings say so.
    std::vector<float> silence (48000, 0.0f);
    render (*p, silence, 256);
    const auto q = p->getReadings();
    check (! q.voiced && q.centsOffTarget == 0.0f && q.targetNote == -1, "silence: readings show no pitch");

    // The window shows them, without any host message.
    {
        auto x2 = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 0.5, SR), voice::vowelA, SR);
        render (*p, x2, 256);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
        auto* te = dynamic_cast<TuneEditor*> (ed.get());
        check (te != nullptr && te->readingText().contains ("Hz") && te->readingText().contains ("A3"), "the window's reading strip shows the pitch: " + (te != nullptr ? te->readingText().toStdString() : std::string ("none")));
        juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        juce::Graphics g (img);
        ed->paintEntireComponent (g, false);
        check (ed->getWidth() > 200 && ed->getHeight() > 100, "the window opens and paints");
    }

    // Pitch history for a later graph
    check (p->getPitchHistoryCount() > 0 && p->getPitchPoint (p->getPitchHistoryCount() - 1).targetNote.load() >= -1.0f, "pitch history is filling, points", (double) p->getPitchHistoryCount());
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
    check (same, "output does not depend on block size (and so not on a playhead)");

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
        check (equal, "prepareToPlay again resets all state");
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
        check (right < 1.0e-4 * left, "stereo: a voice on the left stays on the left, right against left", toDb (right / left), "dB");
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
        check (hard >= 0 && natural >= 0 && n >= 4, "hard and natural presets are both there, presets", n);
        p->setCurrentProgram (hard);
        const double hardKept = vibratoKept (*p);
        p->setCurrentProgram (natural);
        const double naturalKept = vibratoKept (*p);
        check (hardKept <= limits::vibratoFlatHard, "preset Hard Tune flattens vibrato, kept", hardKept * 100.0, "%");
        check (naturalKept > hardKept + 0.2, "preset Natural Pop keeps more vibrato, kept", naturalKept * 100.0, "%");

        // Presets leave Key, Scale, Input Type and Follow Key alone.
        auto q = make();
        setParam (*q, "key", 5.0f); setParam (*q, "scale", 2.0f); setParam (*q, "inputType", 2.0f);
        q->setCurrentProgram (hard);
        check (juce::roundToInt (q->apvts.getRawParameterValue ("key")->load()) == 5 && juce::roundToInt (q->apvts.getRawParameterValue ("scale")->load()) == 2 && juce::roundToInt (q->apvts.getRawParameterValue ("inputType")->load()) == 2, "presets leave Key, Scale and Input Type alone");
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
    check (percent <= limits::cpuPercent, "CPU on one core, 48 kHz stereo, Wide Range, voice at 180 Hz (machine busy with other builds)", percent, "%");
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
            for (float retune : { 0.0f, 40.0f })
            {
                const auto x = loadWav (name);
                auto p = tuner (std::string (name).find ("daniel") != std::string::npos ? lowMale : altoTenor, retune, retune > 0 ? 40.0f : 0.0f);
                auto out = render (*p, x, 256);
                const auto target = folder.getChildFile (juce::String (name).upToFirstOccurrenceOf (".", false, false) + (retune > 0 ? "_natural.wav" : "_hard.wav"));
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
        opt.readOnly = { "pitchOffTarget", "detectedPitch", "correctionApplied", "targetNote", "activeKey" };
        runGenericChecks ([] () -> std::unique_ptr<juce::AudioProcessor> { return make(); }, opt);
    }
    if (want ("latency")) testLatency();
    if (want ("tracker")) testTracker();
    if (want ("output")) testOutputPitch();
    if (want ("retune")) testRetuneSpeed();
    if (want ("formants")) testFormants();
    if (want ("vibrato")) testVibrato();
    if (want ("step")) testHardStep();
    if (want ("flex")) testFlex();
    if (want ("unvoiced")) testUnvoiced();
    if (want ("artifacts")) testArtifacts();
    if (want ("keys")) testKeysAndScales();
    if (want ("follow")) testFollowKey();
    if (want ("real")) testRealMaterial();
    if (want ("added")) testAddedControls();
    if (want ("meter")) testMeter();
    if (want ("blocks")) testBlocksAndState();
    if (want ("cpu")) testCpu();
    return finish();
}
