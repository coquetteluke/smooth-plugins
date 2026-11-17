// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// Every limit is in PassLimits.h, which was committed before the DSP was written.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PassLimits.h"
#include "VoiceSynth.h"
#include "Analysis.h"
#include "PluginProcessor.h"
#include "SmoothLink.h"
#include "MorphEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <ctime>
#include <thread>

using namespace testkit;
using namespace measure;

namespace
{
constexpr double SR = 48000.0;

void setParam (juce::AudioProcessor& p, const char* id, float value)
{
    auto* holder = dynamic_cast<PluginProcessor&> (p).apvts.getParameter (id);
    holder->setValueNotifyingHost (holder->convertTo0to1 (value));
}

std::unique_ptr<PluginProcessor> make() { return std::make_unique<PluginProcessor>(); }

std::vector<float> mono (const juce::AudioBuffer<float>& b)
{
    std::vector<float> v ((size_t) b.getNumSamples());
    std::copy_n (b.getReadPointer (0), b.getNumSamples(), v.data());
    return v;
}

std::vector<float> run (PluginProcessor& p, const std::vector<float>& in, double sr = SR, int channels = 1, int block = 256)
{
    prepare (p, channels, sr, block);
    return mono (render (p, in, block));
}

std::vector<float> loadWav (const char* name)
{
    const auto file = juce::File (SMOOTH_MORPH_CORPUS).getChildFile (name);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));
    if (reader == nullptr) return {};
    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, (int) reader->lengthInSamples, 0, true, false);
    return mono (b);
}

// Pitch against a known value. The worst 5% of frames are left out, and low coverage counts as failure.
double pitchError (const std::vector<float>& x, double hz, double fromSec, double toSec, double sr = SR, double search = 0.06)
{
    const auto off = centsOff (x.data(), (long) x.size(), sr, hz, fromSec, toSec, 0.005, search);
    const double wantedFrames = (toSec - fromSec) / 0.005;
    if ((double) off.size() < 0.9 * wantedFrames) return 999.0;
    return percentile (absolute (off), 95.0);
}

// A voice that glides over one octave, so harmonics sweep over the whole shape of the vocal tract.
std::vector<float> glideVoice (const voice::Vowel& v, double centreHz, double sr = SR, double seconds = 6.0)
{
    std::vector<double> f0 ((size_t) (seconds * sr));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        const double u = std::fmod ((double) i / sr / 3.0, 1.0);
        const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u;
        f0[i] = centreHz * std::pow (2.0, 0.5 * tri);
    }
    return voice::synth (f0, v, sr);
}

struct Bands { double low1, high1, low2, high2; };
Bands bandsOf (const voice::Vowel& v) { return { 0.7 * v.f[0], 1.3 * v.f[1], 0.8 * v.f[1], 1.2 * v.f[3] }; }

// Worst difference from an expected scale, over the two bands, of the spectrum shape.
double formantMove (const std::vector<float>& in, const std::vector<float>& out, const voice::Vowel& v, double smoothing, double expected = 1.0, double sr = SR)
{
    const long from = (long) (0.3 * sr);
    const long n = (long) std::min (in.size(), out.size()) - from - 2000;
    const auto a = smoothSpectrumDb (in.data() + from, n, sr, smoothing), b = smoothSpectrumDb (out.data() + from, n, sr, smoothing);
    const auto bands = bandsOf (v);
    const double lowest = expected * 0.6, highest = expected * 1.6;
    const double s1 = shapeScale (a, b, bands.low1, bands.high1, lowest, highest);
    const double s2 = shapeScale (a, b, bands.low2, bands.high2, lowest, highest);
    return std::max (std::abs (s1 / expected - 1.0), std::abs (s2 / expected - 1.0));
}

double levelChange (const std::vector<float>& in, const std::vector<float>& out, long from)
{
    const int n = (int) std::min (in.size(), out.size()) - (int) from - 2000;
    return toDb (rms (out.data() + from, n)) - toDb (rms (in.data() + from, n));
}

double nullAgainstDelayed (const std::vector<float>& in, const std::vector<float>& out, int delay, long from)
{
    double e = 0.0, s = 0.0;
    for (long i = from; i < (long) out.size(); ++i)
    {
        const double d = (double) out[(size_t) i] - in[(size_t) (i - delay)];
        e += d * d;
        s += (double) in[(size_t) (i - delay)] * in[(size_t) (i - delay)];
    }
    return 10.0 * std::log10 (std::max (e, 1.0e-30) / std::max (s, 1.0e-30));
}

// ------------------------------------------------------------------------------------------------- latency and neutral

void testNeutralAndLatency()
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        auto p = make();
        const auto in = voice::synth (voice::vibrato (180.0, 2.0, sr, 30.0, 5.5), voice::vowelA, sr, -35.0);
        const auto out = run (*p, in, sr);
        const int latency = p->getLatencySamples();
        const double ms = 1000.0 * latency / sr;
        check (ms <= limits::latencyMaxMs, "latency at " + std::to_string ((int) sr) + " Hz", ms, "ms");
        info ("  in samples", latency);
        const double n = nullAgainstDelayed (in, out, latency, (long) (0.2 * sr));
        check (n <= limits::neutralNullDb, "default settings null against the input delayed by the reported latency, " + std::to_string ((int) sr) + " Hz", n, "dB");

        // Find the delay by correlation, with nothing assumed.
        int best = 0; double bestC = -1.0;
        for (int d = std::max (0, latency - 400); d <= latency + 400; ++d)
        {
            double c = 0.0;
            for (long i = (long) (0.5 * sr); i < (long) (0.5 * sr) + 8000; ++i) c += (double) out[(size_t) i] * in[(size_t) (i - d)];
            if (c > bestC) { bestC = c; best = d; }
        }
        check (std::abs (best - latency) <= limits::latencyErrorSamples, "delay measured by correlation against the reported one, samples off", best - latency);

        auto bypassed = make();
        prepare (*bypassed, 1, sr, 256);
        check (bypassed->getLatencySamples() == latency, "latency reports the same with a fresh prepare");
    }

    // The dry path is lined up on a loud, hard sound too. Noise is what a misaligned dry path shows worst on.
    auto p = make();
    setParam (*p, "pitch", 7.0f);
    setParam (*p, "mix", 0.0f);
    std::vector<float> in ((size_t) SR);
    noise (in.data(), (int) SR, 0.2);
    const auto out = run (*p, in);
    const double n = nullAgainstDelayed (in, out, p->getLatencySamples(), (long) (0.3 * SR));
    check (n <= limits::mixNullDb, "Mix 0 with Pitch +7 is the input delayed, on noise", n, "dB");

    // Bypass holds the same delay
    auto b = make();
    setParam (*b, "pitch", 5.0f);
    prepare (*b, 1, SR, 256);
    {
        juce::AudioBuffer<float> buf (1, 256);
        juce::MidiBuffer midi;
        std::vector<float> o;
        const auto v = voice::synth (voice::steady (200.0, 1.0, SR), voice::vowelA, SR);
        for (size_t at = 0; at + 256 <= v.size(); at += 256)
        {
            std::copy_n (v.data() + at, 256, buf.getWritePointer (0));
            b->processBlockBypassed (buf, midi);
            o.insert (o.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 256);
        }
        const double m = nullAgainstDelayed (v, o, b->getLatencySamples(), (long) (0.3 * SR));
        check (m <= limits::mixNullDb, "processBlockBypassed is the input at the same delay", m, "dB");
    }
}

// ------------------------------------------------------------------------------------------------- Pitch

void testPitch()
{
    double worstCents = 0.0, worstLevel = 0.0;
    for (double hz : { 98.0, 147.0, 196.0, 262.0, 349.0 })
        for (double shift : { -12.0, -7.0, -3.0, -1.0, 1.0, 3.0, 7.0, 12.0 })
            for (auto* v : { &voice::vowelA, &voice::vowelI })
            {
                auto p = make();
                setParam (*p, "pitch", (float) shift);
                const auto in = voice::synth (voice::steady (hz, 2.0, SR), *v, SR, -40.0);
                const auto out = run (*p, in);
                const double want = hz * std::pow (2.0, shift / 12.0);
                const double e = pitchError (out, want, 0.5, 1.8);
                const double dl = levelChange (in, out, (long) (0.5 * SR));
                if (e > limits::pitchCents || std::abs (dl) > limits::pitchLevelDb)
                    std::printf ("  note %.0f, shift %+.0f, vowel %s: %.2f cents at the 95th percentile, level %+.1f dB\n", hz, shift, v->name, e, dl);
                worstCents = std::max (worstCents, e);
                worstLevel = std::max (worstLevel, std::abs (levelChange (in, out, (long) (0.5 * SR))));
            }
    check (worstCents <= limits::pitchCents, "Pitch +/-1 to +/-12, notes 98 to 349 Hz, worst 95th percentile", worstCents, "cents");
    check (worstLevel <= limits::pitchLevelDb, "level stays put over those, worst change", worstLevel, "dB");

    // On a moving voice: the vibrato keeps its shape and the centre moves by the shift
    {
        auto p = make();
        setParam (*p, "pitch", -7.0f);
        const auto in = voice::synth (voice::vibrato (220.0, 3.0, SR, 40.0, 5.5), voice::vowelE, SR, -35.0);
        const auto out = run (*p, in);
        const double want = 220.0 * std::pow (2.0, -7.0 / 12.0);
        const auto trace = pitchTrace (out.data(), (long) out.size(), SR, want, 0.5, 2.8, 0.005);
        std::vector<double> cents_;
        for (double hz : trace) cents_.push_back (hz > 0.0 ? cents (hz, want) : std::nan (""));
        const double depth = sineDepth (cents_, 0.005, 5.5);
        check (std::abs (depth / 40.0 - 1.0) <= 0.15, "a voice's own vibrato of 40 cents at Pitch -7 reads", depth, "cents");
    }

    // Other rates
    for (double sr : { 44100.0, 96000.0 })
    {
        double worst = 0.0;
        for (double shift : { -12.0, 12.0, 5.0 })
        {
            auto p = make();
            setParam (*p, "pitch", (float) shift);
            const auto in = voice::synth (voice::steady (196.0, 2.0, sr), voice::vowelA, sr, -40.0);
            const auto out = run (*p, in, sr);
            worst = std::max (worst, pitchError (out, 196.0 * std::pow (2.0, shift / 12.0), 0.5, 1.8, sr));
        }
        check (worst <= limits::pitchCents, "Pitch at " + std::to_string ((int) sr) + " Hz, worst", worst, "cents");
    }
}

void testPitchFormants()
{
    double worst = 0.0, worstReal = 0.0, worstFloor = 0.0, best = 0.0, worstUnreadable = 0.0;
    int unreadable = 0;
    const auto one = [&] (const voice::Vowel& v, double centre, double shift)
    {
        auto p = make();
        setParam (*p, "pitch", (float) shift);
        const auto in = glideVoice (v, centre);
        const auto out = run (*p, in);
        const double moved = centre * std::pow (2.0, shift / 12.0);
        const auto control = glideVoice (v, moved);
        const double smoothing = std::max (centre, moved);
        const double m = formantMove (in, out, v, smoothing);
        const double r = formantMove (control, out, v, smoothing);
        const double f = formantMove (control, in, v, smoothing);
        worst = std::max (worst, m);
        worstReal = std::max (worstReal, r);
        worstFloor = std::max (worstFloor, f);
        // The same two readings as for Formant Shift. See there. Here the real voice has the same tract at the new pitch.
        // If the input against that real voice, which share a vocal tract, already reads over the limit, the tool cannot judge this
        // case at all: harmonics are too far apart for the shape to show. Those are counted and reported, not passed or failed.
        if (f > limits::pitchFormantFraction)
        {
            ++unreadable;
            worstUnreadable = std::max (worstUnreadable, r);
            return;
        }
        best = std::max (best, std::min (m, r));
        if (std::min (m, r) > limits::pitchFormantFraction || std::getenv ("MORPH_DUMP") != nullptr)
            std::printf ("  vowel %s around %.0f Hz, shift %+.0f: against input %.1f%%, against a real voice there %.1f%%, input against that voice %.1f%%\n", v.name, centre, shift, 100 * m, 100 * r, 100 * f);
    };
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU })
        for (double centre : { 110.0, 165.0, 247.0 })
            for (double shift : { -12.0, -7.0, -3.0, 3.0, 7.0, 12.0 })
                one (*v, centre, shift);
    check (best <= limits::pitchFormantFraction, "formants across Pitch +/-3 to +/-12, better reading, worst move", best * 100.0, "%");
    info ("  output against input on its own, worst", worst * 100.0, "%");
    info ("  output against a real voice at the new pitch on its own, worst", worstReal * 100.0, "%");
    info ("  and the tool's floor: the input against that real voice, worst", worstFloor * 100.0, "%");
    info ("  cases the tool cannot judge, because its floor is over the limit (high voice, wide harmonics)", unreadable);
    info ("  those cases against a real voice, worst, for what it is worth", worstUnreadable * 100.0, "%");

    best = 0.0;
    for (auto* v : { &voice::vowelO, &voice::vowelAe })
        for (double centre : { 130.0, 196.0 })
            for (double shift : { -12.0, -5.0, 5.0, 12.0 })
                one (*v, centre, shift);
    check (best <= limits::pitchFormantFraction, "formants on vowels and pitches kept back from any tuning, better reading, worst move", best * 100.0, "%");
}

// ------------------------------------------------------------------------------------------------- Formant Shift

void testFormantShift()
{
    double worstRatio = 0.0, worstPitch = 0.0, worstDirect = 0.0, worstReal = 0.0;
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI })
        for (double centre : { 110.0, 196.0 })
            for (double st : { -7.0, -4.0, -2.0, 2.0, 4.0, 7.0 })
            {
                auto p = make();
                setParam (*p, "formant", (float) st);
                const auto in = glideVoice (*v, centre);
                const auto out = run (*p, in);
                // Two readings. The tool can read a true move wrongly when formants a set distance apart line up with their own
                // neighbours (a real voice with moved formants can read 3% off, and e at 196 Hz -7 read 12.6% against 0.7% from
                // that real voice), so a case passes on the better of: the scale against the input, and the scale against a real
                // voice at this pitch with its formants really moved. Both worst cases are reported.
                voice::Vowel moved = *v;
                const double ratio = std::pow (2.0, st / 12.0);
                for (auto& f : moved.f) f *= ratio;
                const auto real = glideVoice (moved, centre);
                const double direct = formantMove (in, out, *v, centre, ratio), againstReal = formantMove (real, out, moved, centre);
                const double m = std::min (direct, againstReal);
                if (m > limits::formantRatioFraction || std::getenv ("MORPH_DUMP") != nullptr)
                    std::printf ("  vowel %s around %.0f Hz, formant %+.0f: ratio reading off by %.1f%%, against a real moved voice %.1f%%\n", v->name, centre, st, 100 * direct, 100 * againstReal);
                worstRatio = std::max (worstRatio, m);
                worstDirect = std::max (worstDirect, direct);
                worstReal = std::max (worstReal, againstReal);
            }
    check (worstRatio <= limits::formantRatioFraction, "Formant Shift +/-2 to +/-7 semitones, measured ratio against 2^(st/12), better reading, worst", worstRatio * 100.0, "%");
    info ("  worst ratio reading on its own", worstDirect * 100.0, "%");
    info ("  worst reading against a real voice with moved formants on its own", worstReal * 100.0, "%");

    for (double sr : { 44100.0, 96000.0 })
    {
        double worst = 0.0;
        for (double st : { -4.0, 4.0 })
        {
            auto p = make();
            setParam (*p, "formant", (float) st);
            const auto in = glideVoice (voice::vowelA, 130.0, sr);
            const auto out = run (*p, in, sr);
            worst = std::max (worst, formantMove (in, out, voice::vowelA, 130.0, std::pow (2.0, st / 12.0), sr));
        }
        check (worst <= limits::formantRatioFraction, "Formant Shift +/-4 at " + std::to_string ((int) sr) + " Hz, ratio reading, worst", worst * 100.0, "%");
    }

    for (double st : { -12.0, -7.0, -3.0, 3.0, 7.0, 12.0 })
        for (double hz : { 110.0, 196.0, 330.0 })
        {
            auto p = make();
            setParam (*p, "formant", (float) st);
            const auto in = voice::synth (voice::steady (hz, 2.0, SR), voice::vowelA, SR, -40.0);
            const auto out = run (*p, in);
            const double e = pitchError (out, hz, 0.5, 1.8);
            if (e > limits::formantPitchCents) std::printf ("  formant %+.0f at %.0f Hz: pitch %.2f cents\n", st, hz, e);
            worstPitch = std::max (worstPitch, e);
        }
    check (worstPitch <= limits::formantPitchCents, "pitch while Formant Shift goes to +/-12, worst 95th percentile", worstPitch, "cents");
}

// ------------------------------------------------------------------------------------------------- characters

double spectralAt (const std::vector<double>& shape, double hz) { return shape[(size_t) hz]; }

struct Pulses { double jitterPercent = 0.0, shimmerDb = 0.0, meanPeriod = 0.0; };

// Cycle to cycle changes, from the biggest peak in each cycle of a lightly smoothed rectified signal.
Pulses pulsesOf (const std::vector<float>& x, double hz, double fromSec, double toSec, double sr = SR)
{
    std::vector<double> marks, levels;
    const long from = (long) (fromSec * sr), to = (long) (toSec * sr);
    const long reach = (long) (0.45 * sr / hz);
    double smooth = 0.0;
    std::vector<double> env ((size_t) to, 0.0);
    const double k = 1.0 - std::exp (-2.0 * M_PI * 1500.0 / sr);
    for (long i = 1; i < to; ++i) { smooth += k * (std::abs ((double) x[(size_t) i]) - smooth); env[(size_t) i] = smooth; }
    for (long i = from + reach; i < to - reach; ++i)
    {
        bool top = env[(size_t) i] > 0.0;
        for (long j = i - reach; top && j <= i + reach; ++j)
            if (env[(size_t) j] > env[(size_t) i]) top = false;
        if (top) { marks.push_back ((double) i); levels.push_back (env[(size_t) i]); i += reach; }
    }
    Pulses r;
    if (marks.size() < 4) return r;
    double dt = 0.0, da = 0.0, total = 0.0;
    int count = 0;
    for (size_t i = 2; i < marks.size(); ++i)
    {
        const double a = marks[i - 1] - marks[i - 2], b = marks[i] - marks[i - 1];
        dt += std::abs (a - b);
        total += a;
        da += std::abs (20.0 * std::log10 (levels[i] / levels[i - 1]));
        ++count;
    }
    r.meanPeriod = total / count;
    r.jitterPercent = 100.0 * (dt / count) / r.meanPeriod;
    r.shimmerDb = da / count;
    return r;
}

void testCharacters()
{
    enum { natural, deep, child, robot, monster };
    struct Case { int id; const char* name; double pitch, formant; };
    for (const Case& c : { Case { deep, "Deep", -5.0, -3.0 }, Case { child, "Child", 7.0, 4.0 } })
    {
        double worstPitch = 0.0, worstFormant = 0.0;
        for (double hz : { 130.0, 196.0, 262.0 })
        {
            auto p = make();
            setParam (*p, "character", (float) c.id);
            const auto in = voice::synth (voice::steady (hz, 2.0, SR), voice::vowelA, SR, -40.0);
            const auto out = run (*p, in);
            worstPitch = std::max (worstPitch, pitchError (out, hz * std::pow (2.0, c.pitch / 12.0), 0.5, 1.8));
        }
        for (auto* v : { &voice::vowelA, &voice::vowelE })
            for (double centre : { 130.0, 196.0 })
            {
                auto p = make();
                setParam (*p, "character", (float) c.id);
                const auto in = glideVoice (*v, centre);
                const auto out = run (*p, in);
                // Better of two readings, as for Formant Shift: against the input, and against a real voice with this tract and pitch.
                const double smoothing = std::max (centre, centre * std::pow (2.0, c.pitch / 12.0));
                voice::Vowel moved = *v;
                for (auto& f : moved.f) f *= std::pow (2.0, c.formant / 12.0);
                const auto real = glideVoice (moved, centre * std::pow (2.0, c.pitch / 12.0));
                const double m = std::min (formantMove (in, out, *v, smoothing, std::pow (2.0, c.formant / 12.0)), formantMove (real, out, moved, smoothing));
                worstFormant = std::max (worstFormant, m);
            }
        check (worstPitch <= limits::characterPitchCents, std::string (c.name) + ": pitch moves as stated, worst", worstPitch, "cents");
        check (worstFormant <= limits::characterFormantFraction, std::string (c.name) + ": formants move as stated, worst", worstFormant * 100.0, "%");

        // Character Amount scales it
        auto p = make();
        setParam (*p, "character", (float) c.id);
        setParam (*p, "characterAmount", 50.0f);
        const auto in = voice::synth (voice::steady (196.0, 2.0, SR), voice::vowelA, SR, -40.0);
        const auto out = run (*p, in);
        const double e = pitchError (out, 196.0 * std::pow (2.0, 0.5 * c.pitch / 12.0), 0.5, 1.8);
        check (e <= limits::characterPitchCents, std::string (c.name) + " at Character Amount 50: half the pitch move", e, "cents");
    }

    // Robot: a voice gliding a fifth with vibrato comes out at one note. Robot Note A2 is 110 Hz.
    {
        double worst = 0.0;
        for (double note : { 45.0, 52.0, 60.0 })
        {
            std::vector<double> f0 ((size_t) (4.0 * SR));
            for (size_t i = 0; i < f0.size(); ++i)
            {
                const double t = (double) i / SR;
                f0[i] = 150.0 * std::pow (2.0, (7.0 / 12.0) * (0.5 + 0.5 * std::sin (2.0 * M_PI * 0.4 * t)) + (40.0 / 1200.0) * std::sin (2.0 * M_PI * 5.5 * t));
            }
            auto p = make();
            setParam (*p, "character", (float) robot);
            setParam (*p, "robotNote", (float) note);
            const auto in = voice::synth (f0, voice::vowelA, SR, -40.0);
            const auto out = run (*p, in);
            const double hz = 440.0 * std::pow (2.0, (note - 69.0) / 12.0);
            const double e = pitchError (out, hz, 0.5, 3.8);
            std::printf ("  Robot Note at %.1f Hz: %.2f cents\n", hz, e);
            worst = std::max (worst, e);
        }
        check (worst <= limits::robotPitchCents, "Robot: a voice gliding a fifth with vibrato stays on the note, worst 95th percentile", worst, "cents");

        // Character Amount 50: half way between the voice and the note, in pitch
        {
            auto p = make();
            setParam (*p, "character", (float) robot);
            setParam (*p, "characterAmount", 50.0f);
            const auto in = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR, -40.0);
            const auto out = run (*p, in);
            const double e = pitchError (out, 155.56, 0.5, 1.8);
            check (e <= limits::robotPitchCents, "Robot at Character Amount 50 on a 220 Hz voice lands half way to 110 Hz", e, "cents");
        }

        // Robot keeps formants in place and makes them sharper.
        const auto in = glideVoice (voice::vowelE, 165.0);
        auto p = make();
        setParam (*p, "character", (float) robot);
        const auto out = run (*p, in);
        const double m = formantMove (in, out, voice::vowelE, 165.0);
        check (m <= limits::characterFormantFraction, "Robot: formants stay in place", m * 100.0, "%");
        const long from = (long) (0.3 * SR), n = (long) in.size() - from - 2000;
        const auto a = smoothSpectrumDb (in.data() + from, n, SR, 165.0), b = smoothSpectrumDb (out.data() + from, n, SR, 165.0);
        const double before = spectralAt (a, voice::vowelE.f[1]) - spectralAt (a, 1150.0);
        const double after = spectralAt (b, voice::vowelE.f[1]) - spectralAt (b, 1150.0);
        check (after - before >= limits::robotPeakGainDb, "Robot: second formant stands taller over the valley, by", after - before, "dB");
    }

    // Monster
    {
        const double hz = 220.0, down = hz * std::pow (2.0, -8.0 / 12.0);
        const auto in = voice::synth (voice::steady (hz, 3.0, SR), voice::vowelA, SR, -40.0);
        auto p = make();
        setParam (*p, "character", (float) monster);
        const auto out = run (*p, in);
        auto q = make();
        setParam (*q, "pitch", -8.0f);
        setParam (*q, "formant", -5.0f);
        const auto plain = run (*q, in);

        // Its cycle is two grains long, so the pitch reads cleanly from twice the period.
        const auto twice = centsOff (out.data(), (long) out.size(), SR, down * 0.5, 0.5, 2.8, 0.01, 0.1);
        const double centre = std::abs (median (twice));
        check (centre <= limits::monsterPitchCents, "Monster: median pitch is 8 semitones down, off by", centre, "cents");
        const int span = (int) (2.3 * SR), start = (int) (0.5 * SR);
        const double subOut = toneDb (out.data() + start, span, down * 0.5, SR), subPlain = toneDb (plain.data() + start, span, down * 0.5, SR);
        check (subOut - subPlain >= limits::monsterSubharmonicRiseDb, "Monster: energy at half the pitch, rise against the same move without growl", subOut - subPlain, "dB");
        info ("  that subharmonic against the fundamental", subOut - toneDb (out.data() + start, span, down, SR), "dB");
        const auto rough = pulsesOf (out, down, 0.5, 2.8), smooth = pulsesOf (plain, down, 0.5, 2.8);
        check (rough.jitterPercent >= limits::monsterJitterPercent, "Monster: cycle to cycle period change", rough.jitterPercent, "%");
        check (rough.shimmerDb >= limits::monsterShimmerDb, "Monster: cycle to cycle level change", rough.shimmerDb, "dB");
        info ("  without growl: period change", smooth.jitterPercent, "%");
        info ("  without growl: level change", smooth.shimmerDb, "dB");

        const auto g = glideVoice (voice::vowelA, 196.0);
        auto r = make();
        setParam (*r, "character", (float) monster);
        const auto gOut = run (*r, g);
        const double m = formantMove (g, gOut, voice::vowelA, 196.0, std::pow (2.0, -5.0 / 12.0));
        check (m <= limits::characterFormantFraction, "Monster: formants move as stated, off by", m * 100.0, "%");
    }

    // Natural at Character Amount anything is the same as neutral.
    {
        auto p = make();
        setParam (*p, "character", (float) natural);
        setParam (*p, "characterAmount", 30.0f);
        const auto in = voice::synth (voice::steady (196.0, 1.0, SR), voice::vowelA, SR, -40.0);
        const auto out = run (*p, in);
        const double n = nullAgainstDelayed (in, out, p->getLatencySamples(), (long) (0.2 * SR));
        check (n <= limits::neutralNullDb, "Natural is unchanged at any Character Amount", n, "dB");
    }
}

// ------------------------------------------------------------------------------------------------- Vibrato

void testVibrato()
{
    double worstRate = 0.0, worstDepth = 0.0;
    for (double rate : { 2.0, 5.5, 9.0 })
        for (double depth : { 20.0, 50.0, 100.0 })
            for (double hz : { 147.0, 262.0 })
            {
                auto p = make();
                setParam (*p, "vibratoRate", (float) rate);
                setParam (*p, "vibratoDepth", (float) depth);
                const auto in = voice::synth (voice::steady (hz, 5.0, SR), voice::vowelA, SR, -40.0);
                const auto out = run (*p, in);
                const auto trace = pitchTrace (out.data(), (long) out.size(), SR, hz, 0.5, 4.8, 0.004, 0.15);
                std::vector<double> c;
                for (double h : trace) c.push_back (h > 0.0 ? cents (h, hz) : std::nan (""));
                double bestRate = 0.0, bestSize = -1.0;
                for (double r = rate * 0.7; r <= rate * 1.3; r += rate * 0.005)
                {
                    const double s = sineDepth (c, 0.004, r);
                    if (s > bestSize) { bestSize = s; bestRate = r; }
                }
                const double size = sineDepth (c, 0.004, bestRate);
                const double rateOff = std::abs (bestRate / rate - 1.0), depthOff = std::abs (size / depth - 1.0);
                if (rateOff > limits::vibratoFraction || depthOff > limits::vibratoFraction)
                    std::printf ("  rate %.1f depth %.0f at %.0f Hz: read rate %.2f, depth %.1f\n", rate, depth, hz, bestRate, size);
                worstRate = std::max (worstRate, rateOff);
                worstDepth = std::max (worstDepth, depthOff);
            }
    check (worstRate <= limits::vibratoFraction, "Vibrato Rate 2 to 9 Hz, worst error", worstRate * 100.0, "%");
    check (worstDepth <= limits::vibratoFraction, "Vibrato Depth 20 to 100 cents, worst error", worstDepth * 100.0, "%");

    // Depth 0 means none, exactly
    auto p = make();
    setParam (*p, "vibratoRate", 9.0f);
    const auto in = voice::synth (voice::steady (196.0, 1.0, SR), voice::vowelA, SR, -40.0);
    const auto out = run (*p, in);
    const double n = nullAgainstDelayed (in, out, p->getLatencySamples(), (long) (0.2 * SR));
    check (n <= limits::neutralNullDb, "Vibrato Depth 0 changes nothing", n, "dB");
}

// ------------------------------------------------------------------------------------------------- Whisper

// Energy on the harmonics of a known pitch against energy half way between them, in dB. 0 means no harmonics at all.
double combDb (const std::vector<float>& x, double hz, double fromSec, double toSec, double sr = SR)
{
    constexpr int order = 15, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> power (size / 2 + 1, 0.0);
    std::vector<float> buf ((size_t) size * 2);
    for (long at = (long) (fromSec * sr); at + size <= (long) (toSec * sr); at += size / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[(size_t) (at + i)] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k <= size / 2; ++k) power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    // Harmonic by harmonic against the two gaps either side, so that the slope of a formant doesn't read as harmonics.
    const double bin = sr / size;
    const auto around = [&] (double hzAt)
    {
        double e = 0.0;
        for (double d = -8.0; d <= 8.0; d += bin) e += power[(size_t) std::lround ((hzAt + d) / bin)];
        return e;
    };
    double total = 0.0; int count = 0;
    for (int h = 1; h * hz < 5000.0; ++h)
    {
        total += 10.0 * std::log10 (std::max (around (h * hz), 1.0e-20) / std::max (0.5 * (around ((h - 0.5) * hz) + around ((h + 0.5) * hz)), 1.0e-20));
        ++count;
    }
    return total / count;
}

// Normalised autocorrelation at the pitch lag after taking the spectrum shape out, so a formant's own ringing doesn't read as pitch.
double periodicity (const std::vector<float>& x, double hz, double fromSec, double toSec, double sr = SR)
{
    constexpr int order = 40;
    const long from = (long) (fromSec * sr), to = (long) (toSec * sr);
    std::vector<double> r ((size_t) order + 1, 0.0);
    for (int lag = 0; lag <= order; ++lag)
        for (long i = from; i < to; ++i) r[(size_t) lag] += (double) x[(size_t) i] * x[(size_t) (i - lag)];
    r[0] *= 1.0001;
    std::vector<double> a ((size_t) order + 1, 0.0), tmp;
    a[0] = 1.0;
    double err = r[0];
    for (int i = 1; i <= order; ++i)
    {
        double acc = r[(size_t) i];
        for (int j = 1; j < i; ++j) acc += a[(size_t) j] * r[(size_t) (i - j)];
        const double k = -acc / err;
        tmp = a;
        for (int j = 1; j < i; ++j) a[(size_t) j] = tmp[(size_t) j] + k * tmp[(size_t) (i - j)];
        a[(size_t) i] = k;
        err *= 1.0 - k * k;
    }
    std::vector<double> e ((size_t) (to - from), 0.0);
    for (long i = from; i < to; ++i)
    {
        double s = 0.0;
        for (int k = 0; k <= order; ++k) s += a[(size_t) k] * x[(size_t) (i - k)];
        e[(size_t) (i - from)] = s;
    }
    const int T = (int) std::lround (sr / hz);
    double best = 0.0;
    for (int lag = T - 2; lag <= T + 2; ++lag)
    {
        double c = 0.0, p = 0.0, q = 0.0;
        for (size_t i = (size_t) lag; i < e.size(); ++i) { c += e[i] * e[i - (size_t) lag]; p += e[i] * e[i]; q += e[i - (size_t) lag] * e[i - (size_t) lag]; }
        best = std::max (best, c / std::sqrt (p * q + 1.0e-30));
    }
    return best;
}

void testWhisper()
{
    double worstComb = 0.0, worstPeriodic = 0.0, worstLevel = 0.0, inputComb = 1.0e9, inputPeriodic = 1.0e9;
    for (double hz : { 110.0, 196.0, 330.0 })
        for (auto* v : { &voice::vowelA, &voice::vowelE })
        {
            auto p = make();
            setParam (*p, "whisper", 100.0f);
            const auto in = voice::synth (voice::steady (hz, 3.0, SR), *v, SR, -40.0);
            const auto out = run (*p, in);
            const double c = combDb (out, hz, 0.5, 2.9);
            inputComb = std::min (inputComb, combDb (in, hz, 0.5, 2.9));
            worstComb = std::max (worstComb, std::abs (c));
            worstPeriodic = std::max (worstPeriodic, periodicity (out, hz, 0.5, 2.0));
            inputPeriodic = std::min (inputPeriodic, periodicity (in, hz, 0.5, 2.0));
            worstLevel = std::max (worstLevel, std::abs (levelChange (in, out, (long) (0.5 * SR))));
        }
    check (inputComb >= 20.0, "the test voice has harmonics going in, on against between them", inputComb, "dB");
    check (worstComb <= limits::whisperCombDb, "Whisper 100: harmonics to noise, on against between them, worst (0 is none left)", worstComb, "dB");
    info ("  going in, the whitened voice's periodicity at the pitch lag, least", inputPeriodic);
    check (worstPeriodic <= limits::whisperPeriodicity, "Whisper 100: whitened output's periodicity at the pitch lag, worst", worstPeriodic);
    check (worstLevel <= limits::whisperLevelDb, "Whisper 100: level against the input, worst change", worstLevel, "dB");

    double worstMove = 0.0, worstShape = 0.0;
    for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU })
        for (double centre : { 130.0, 220.0 })
        {
            auto p = make();
            setParam (*p, "whisper", 100.0f);
            const auto in = glideVoice (*v, centre);
            const auto out = run (*p, in);
            worstMove = std::max (worstMove, formantMove (in, out, *v, 300.0));
            const long from = (long) (0.3 * SR), n = (long) in.size() - from - 2000;
            const auto a = smoothSpectrumDb (in.data() + from, n, SR, 300.0), b = smoothSpectrumDb (out.data() + from, n, SR, 300.0);
            double mean = 0.0; int count = 0;
            for (int hz = 200; hz < 5000; hz += 10) { mean += b[(size_t) hz] - a[(size_t) hz]; ++count; }
            mean /= count;
            double e = 0.0;
            for (int hz = 200; hz < 5000; hz += 10) { const double d = b[(size_t) hz] - a[(size_t) hz] - mean; e += d * d; }
            worstShape = std::max (worstShape, std::sqrt (e / count));
        }
    check (worstMove <= limits::whisperFormantFraction, "Whisper 100: formants against the voice, worst move", worstMove * 100.0, "%");
    check (worstShape <= limits::whisperShapeDb, "Whisper 100: envelope shape against the voice, RMS difference 200 Hz to 5 kHz, worst", worstShape, "dB");

    // Half way is half way: harmonics are still there, but well down from where they were.
    auto p = make();
    setParam (*p, "whisper", 50.0f);
    const auto in = voice::synth (voice::steady (196.0, 3.0, SR), voice::vowelA, SR, -40.0);
    const auto out = run (*p, in);
    const double c = combDb (out, 196.0, 0.5, 2.9), full = combDb (in, 196.0, 0.5, 2.9);
    check (c > limits::whisperCombDb && c < full - 6.0, "Whisper 50 sits between: on against between the harmonics", c, "dB");

    // The breath is the same noise whenever Whisper was switched on (1.2.1): switched on at 0.5 s, the output from 1.5 s on is the
    // output of Whisper 100 from the start. Before, the noise went on from wherever it was and the two never met again.
    {
        const auto v = voice::synth (voice::steady (196.0, 3.0, SR), voice::vowelA, SR, -40.0);
        auto a = make();
        setParam (*a, "whisper", 100.0f);
        const auto outA = run (*a, v);
        auto b = make();
        prepare (*b, 1, SR, 256);
        const int at = (int) (0.5 * SR / 256);
        const auto outB = mono (render (*b, v, 256, nullptr, [&] (int idx) { if (idx == at) setParam (*b, "whisper", 100.0f); }));
        double e = 0.0, s = 0.0;
        for (long i = (long) (1.5 * SR); i < (long) v.size(); ++i)
        {
            const double d = (double) outB[(size_t) i] - outA[(size_t) i];
            e += d * d; s += (double) outA[(size_t) i] * outA[(size_t) i];
        }
        const double nb = 10.0 * std::log10 (std::max (e, 1.0e-30) / std::max (s, 1.0e-30));
        check (nb <= limits::whisperRepeatNullDb, "Whisper 100 switched on at 0.5 s: from 1.5 s on, against Whisper 100 from the start", nb, "dB");
    }
}

// ------------------------------------------------------------------------------------------------- Mix

void testMix()
{
    const auto in = voice::synth (voice::vibrato (196.0, 2.0, SR, 25.0, 5.5), voice::vowelE, SR, -35.0);
    {
        auto p = make();
        setParam (*p, "pitch", 5.0f);
        setParam (*p, "formant", 2.0f);
        setParam (*p, "mix", 0.0f);
        const double n = nullAgainstDelayed (in, run (*p, in), p->getLatencySamples(), (long) (0.3 * SR));
        check (n <= limits::mixNullDb, "Mix 0 is the delayed input, with everything else turned up", n, "dB");
    }
    {
        auto p = make();
        setParam (*p, "mix", 50.0f);
        const double n = nullAgainstDelayed (in, run (*p, in), p->getLatencySamples(), (long) (0.3 * SR));
        check (n <= limits::neutralNullDb, "Mix 50 with nothing else changed is still the delayed input: no comb from the two paths", n, "dB");
    }
    {
        // Mix 50 with a clean shift: both pitches present and each about 6 dB down.
        const double hz = 220.0, up = 220.0 * std::pow (2.0, 7.0 / 12.0);
        const auto tone = voice::synth (voice::steady (hz, 2.0, SR), voice::vowelA, SR, -40.0);
        auto full = make(); setParam (*full, "pitch", 7.0f);
        const auto wet = run (*full, tone);
        auto half = make(); setParam (*half, "pitch", 7.0f); setParam (*half, "mix", 50.0f);
        const auto both = run (*half, tone);
        const int n = (int) (1.2 * SR), at = (int) (0.6 * SR);
        const double dryPart = toneDb (both.data() + at, n, hz, SR) - toneDb (tone.data() + at - half->getLatencySamples(), n, hz, SR);
        const double wetPart = toneDb (both.data() + at, n, up, SR) - toneDb (wet.data() + at, n, up, SR);
        check (std::abs (dryPart + 6.02) <= limits::mixBlendDb, "Mix 50: dry part of a shifted voice", dryPart, "dB");
        check (std::abs (wetPart + 6.02) <= limits::mixBlendDb, "Mix 50: wet part of a shifted voice", wetPart, "dB");
    }
}

// ------------------------------------------------------------------------------------------------- clicks

double highBandTop (const std::vector<float>& x, long from, long to)
{
    // Two cascaded high pass sections at 8 kHz, then the loudest 5 ms frame.
    juce::dsp::IIR::Coefficients<float>::Ptr c = juce::dsp::IIR::Coefficients<float>::makeHighPass (SR, 8000.0f, 0.7f);
    juce::dsp::IIR::Filter<float> a (c), b (c);
    const int frame = (int) (0.005 * SR);
    double top = 1.0e-20, sum = 0.0;
    int k = 0;
    for (long i = 0; i < to; ++i)
    {
        const float y = b.processSample (a.processSample (x[(size_t) i]));
        if (i < from) continue;
        sum += (double) y * y;
        if (++k == frame) { top = std::max (top, sum / frame); sum = 0.0; k = 0; }
    }
    return 10.0 * std::log10 (top);
}

void testClicks()
{
    const auto voiceIn = voice::synth (voice::vibrato (196.0, 8.0, SR, 15.0, 5.0), voice::vowelA, SR, -40.0);
    const int block = 256;
    const long from = (long) (0.5 * SR);
    const long to = (long) voiceIn.size() - 2000;
    const auto measureRun = [&] (const char* id, const std::vector<float>& values, const char* what)
    {
        double steadyStep = 0.0, steadyHigh = -400.0;
        for (float v : values)
        {
            auto p = make();
            setParam (*p, id, v);
            const auto out = run (*p, voiceIn, SR, 1, block);
            steadyStep = std::max (steadyStep, maxJump (out.data() + from, (int) (to - from)));
            steadyHigh = std::max (steadyHigh, highBandTop (out, from, to));
        }
        auto p = make();
        setParam (*p, id, values[0]);
        prepare (*p, 1, SR, block);
        const int blocksPerChange = (int) (0.4 * SR / block);
        size_t which = 0;
        const auto out = mono (render (*p, voiceIn, block, nullptr, [&] (int idx)
        {
            if (idx > 0 && idx % blocksPerChange == 0)
            {
                which = (which + 1) % values.size();
                setParam (*p, id, values[which]);
            }
        }));
        const double step = maxJump (out.data() + from, (int) (to - from));
        const double high = highBandTop (out, from, to);
        check (step <= limits::clickStepRatio * steadyStep, std::string ("no click changing ") + what + ": biggest step against steady", step / steadyStep, "times");
        check (high - steadyHigh <= limits::clickHighBandRiseDb, std::string ("no click changing ") + what + ": above 8 kHz, loudest 5 ms against steady", high - steadyHigh, "dB");
    };
    measureRun ("character", { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 0.0f, 3.0f, 2.0f, 4.0f, 1.0f }, "Character");
    measureRun ("pitch", { 0.0f, 12.0f, -12.0f, 3.0f, -5.0f, 7.0f }, "Pitch");
    measureRun ("formant", { 0.0f, 7.0f, -7.0f, 3.0f, -3.0f }, "Formant Shift");
    measureRun ("whisper", { 0.0f, 100.0f, 30.0f, 100.0f }, "Whisper");
    measureRun ("mix", { 100.0f, 0.0f, 50.0f, 100.0f }, "Mix");
}

// ------------------------------------------------------------------------------------------------- consonants

// A vowel, an S and a vowel, with the S known exactly.
struct Vcv { std::vector<float> x; long sFrom, sTo; };
Vcv vowelConsonantVowel (double hz, double sr = SR)
{
    Vcv r;
    auto a = voice::synth (voice::steady (hz, 2.0, sr), voice::vowelA, sr, -40.0);
    std::vector<float> s ((size_t) (0.25 * sr));
    noise (s.data(), (int) s.size(), 0.5, 11);
    auto hp = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 3500.0f, 0.7f);
    juce::dsp::IIR::Filter<float> f (hp);
    for (auto& v : s) v = f.processSample (v) * 0.25f;
    const long at = (long) (1.0 * sr), fade = (long) (0.005 * sr);
    r.x = a;
    for (long i = 0; i < (long) s.size(); ++i)
    {
        const double u = std::min ({ 1.0, (double) i / fade, (double) ((long) s.size() - 1 - i) / fade });
        r.x[(size_t) (at + i)] = (float) ((1.0 - u) * a[(size_t) (at + i)] + u * s[(size_t) i]);
    }
    r.sFrom = at;
    r.sTo = at + (long) s.size();
    return r;
}

double flatnessOf (const float* x, int n, double sr)
{
    constexpr int order = 11, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> power (size / 2 + 1, 0.0);
    std::vector<float> buf ((size_t) size * 2);
    for (int at = 0; at + size <= n; at += size / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = x[at + i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k <= size / 2; ++k) power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    const double bin = sr / size;
    double logSum = 0.0, sum = 0.0; int c = 0;
    for (int k = (int) (4000.0 / bin); k < (int) (15000.0 / bin); ++k)
    {
        logSum += std::log (power[(size_t) k] + 1.0e-20);
        sum += power[(size_t) k];
        ++c;
    }
    return std::exp (logSum / c) / (sum / c);
}

void testConsonants()
{
    const auto vcv = vowelConsonantVowel (196.0);
    const long margin = (long) (0.05 * SR);

    double worstDiff = -400.0;
    for (float shift : { -12.0f, -7.0f, 7.0f, 12.0f })
    {
        auto p = make();
        setParam (*p, "pitch", shift);
        const auto out = run (*p, vcv.x);
        const int L = p->getLatencySamples();
        double e = 0.0, s = 0.0;
        for (long i = vcv.sFrom + margin; i < vcv.sTo - margin; ++i)
        {
            const double d = (double) out[(size_t) (i + L)] - vcv.x[(size_t) i];
            e += d * d;
            s += (double) vcv.x[(size_t) i] * vcv.x[(size_t) i];
        }
        worstDiff = std::max (worstDiff, 10.0 * std::log10 (std::max (e, 1.0e-30) / s));
    }
    check (worstDiff <= limits::fricativeDifferenceDb, "middle of an S between shifted vowels, Pitch +/-7 and +/-12, against the delayed input, worst", worstDiff, "dB");

    // The same S through everything that changes it. It should stay noise, not grow tones.
    const int count = (int) (vcv.sTo - vcv.sFrom - 2 * margin);
    const double flatIn = flatnessOf (vcv.x.data() + vcv.sFrom + margin, count, SR);
    info ("  spectral flatness of the S going in", flatIn);
    struct Setting { const char* name; const char* id; float v; };
    double worstFlat = 1.0e9;
    for (const Setting& s : { Setting { "Deep", "character", 1.0f }, Setting { "Child", "character", 2.0f }, Setting { "Robot", "character", 3.0f }, Setting { "Monster", "character", 4.0f },
                              Setting { "Formant Shift +4", "formant", 4.0f }, Setting { "Formant Shift -4", "formant", -4.0f }, Setting { "Whisper 100", "whisper", 100.0f } })
    {
        auto p = make();
        setParam (*p, s.id, s.v);
        const auto out = run (*p, vcv.x);
        const double flat = flatnessOf (out.data() + vcv.sFrom + margin + p->getLatencySamples(), count, SR);
        std::printf ("  S through %s: flatness %.3f\n", s.name, flat);
        worstFlat = std::min (worstFlat, flat / flatIn);
    }
    check (worstFlat >= limits::fricativeFlatnessRatio, "S stays noise through every Character, Formant Shift and Whisper, worst flatness against the input", worstFlat, "times");
}

// ------------------------------------------------------------------------------------------------- real material

void testRealMaterial()
{
    double worstFormant = 0.0, worstPitch = 0.0, worstHissFirst = 0.0, worstHissAway = 0.0;
    std::vector<double> allHiss;
    for (const char* name : { "speech_samantha.wav", "speech_daniel.wav", "speech_kathy.wav" })
    {
        const auto in = loadWav (name);
        check (! in.empty(), std::string ("found ") + name);
        if (in.empty()) continue;
        const auto reference = referenceTrack (in, SR);
        for (double shift : { -7.0, 7.0 })
        {
            auto p = make();
            setParam (*p, "pitch", (float) shift);
            const auto out = run (*p, in);
            const int L = p->getLatencySamples();
            std::vector<float> aligned (out.begin() + L, out.end());

            // Long term spectrum shape in the bands speech has most in
            const long n = (long) aligned.size() - 4096;
            const auto a = smoothSpectrumDb (in.data(), n, SR, 300.0), b = smoothSpectrumDb (aligned.data(), n, SR, 300.0);
            const double m = std::abs (shapeScale (a, b, 300.0, 3500.0) - 1.0);
            worstFormant = std::max (worstFormant, m);

            // Pitch: the two sided reference on both, median of voiced frames.
            const auto track = referenceTrack (aligned, SR);
            std::vector<double> off;
            for (size_t f = 0; f < std::min (track.size(), reference.size()); ++f)
                if (track[f] > 0.0 && reference[f] > 0.0)
                    off.push_back (std::abs (cents (track[f], reference[f]) - shift * 100.0));
            const double e = median (off);
            worstPitch = std::max (worstPitch, e);
            std::printf ("  %s Pitch %+.0f: formant shape %.1f%%, pitch median %.1f cents\n", name, shift, 100.0 * m, e);

            // Hiss band level in the frames that have no pitch and do have hiss.
            juce::dsp::IIR::Coefficients<float>::Ptr c = juce::dsp::IIR::Coefficients<float>::makeHighPass (SR, 4000.0f, 0.7f);
            juce::dsp::IIR::Filter<float> f1 (c), f2 (c);
            std::vector<float> hi1 (in.size()), hi2 (aligned.size());
            for (size_t i = 0; i < in.size(); ++i) hi1[i] = f1.processSample (in[i]);
            for (size_t i = 0; i < aligned.size(); ++i) hi2[i] = f2.processSample (aligned[i]);
            std::vector<double> changes, changesFirst;
            const int frame = (int) (0.02 * SR);
            for (size_t i = 0; i + frame < aligned.size(); i += (size_t) frame)
            {
                const double lo = rms (in.data() + i, frame), hi = rms (hi1.data() + i, frame);
                const size_t index = (size_t) ((double) i / SR / 0.005);
                if (index >= reference.size() || reference[index] > 0.0 || hi < 0.001 || hi < 0.5 * lo)
                    continue;
                // Before commit 8e9df0d every such frame counted. Both forms are checks again since 1.2.0, see below.
                changesFirst.push_back (std::abs (toDb (rms (hi2.data() + i, frame)) - toDb (hi)));
                if (std::getenv ("MORPH_HISS") != nullptr && changesFirst.back() > 2.0)
                    std::printf ("  first form, frame at %.3f s: %+.1f dB, level %.1f dB, hiss %.1f dB, pitched 20 ms before %d, 20 ms after %d, 40 ms after %d\n", (double) i / SR, toDb (rms (hi2.data() + i, frame)) - toDb (hi), toDb (lo), toDb (hi), index >= 4 && reference[index - 4] > 0.0, index + 4 < reference.size() && reference[index + 4] > 0.0, index + 8 < reference.size() && reference[index + 8] > 0.0);
                // A frame that also holds the end of a vowel is not a consonant: its vowel part is shifted on purpose. So nothing
                // voiced within a frame and a bit either way. Changed after the first run, which caught exactly those frames.
                bool nearVowel = false;
                for (size_t k = (size_t) std::max (0, (int) index - 6); k <= std::min (reference.size() - 1, index + 10); ++k)
                    if (reference[k] > 0.0) nearVowel = true;
                if (nearVowel)
                    continue;
                changes.push_back (std::abs (toDb (rms (hi2.data() + i, frame)) - toDb (hi)));
                if (std::getenv ("MORPH_HISS") != nullptr && changes.back() > 3.0)
                    std::printf ("  hiss frame at %.3f s: %+.1f dB, level %.1f dB, hiss %.1f dB, neighbours voiced %d %d\n", (double) i / SR, toDb (rms (hi2.data() + i, frame)) - toDb (hi), toDb (lo), toDb (hi), index >= 4 && reference[index - 4] > 0.0, index + 4 < reference.size() && reference[index + 4] > 0.0);
            }
            std::printf ("    hiss frames %d, median change %.2f dB, 90th %.2f, worst %.2f\n", (int) changes.size(), percentile (changes, 50.0), percentile (changes, 90.0), percentile (changes, 100.0));
            std::printf ("    as first written: unvoiced frames %d, median change %.2f dB, 95th %.2f, worst %.2f; with the frames next to a vowel left out, 95th %.2f\n", (int) changesFirst.size(), percentile (changesFirst, 50.0), percentile (changesFirst, 95.0), percentile (changesFirst, 100.0), percentile (changes, 95.0));
            worstHissFirst = std::max (worstHissFirst, percentile (changesFirst, 95.0));
            worstHissAway = std::max (worstHissAway, percentile (changes, 95.0));
            allHiss.insert (allHiss.end(), changes.begin(), changes.end());
        }
    }
    check (worstFormant <= limits::speechFormantFraction, "say speech, Pitch +/-7: long term spectrum shape against the input, worst move", worstFormant * 100.0, "%");
    check (worstPitch <= limits::speechPitchCents, "say speech, Pitch +/-7: median pitch error against a slow reference, worst", worstPitch, "cents");
    // The test as it failed in the first full results (10.470 dB, commit ad7815d): each voice at each shift on its own, the unvoiced
    // frames with hiss and no vowel next to them, the worst of the six 95th percentiles. With 7 to 16 frames a set that is the worst
    // single frame. Commit ed6bd07 pooled all six sets into one percentile and renamed it, which hid 4.324 dB (1.1.1) behind 1.997.
    // Restored in 1.2.0 with its name and its limit, and it passes because the plug-in changed: 1.955 dB. The pooled reading stays
    // below as a third check.
    check (worstHissAway <= limits::speechHissLevelDb, "say speech, Pitch +/-7: hiss level in unvoiced frames, 95th percentile change, worst", worstHissAway, "dB");
    // And as it stood before commit 8e9df0d took the frames next to a vowel out: every unvoiced frame with hiss, same limit.
    check (worstHissFirst <= limits::speechHissLevelDb, "say speech, Pitch +/-7: the same with the frames next to a vowel counted, worst", worstHissFirst, "dB");
    // Frames from all voices and both shifts together. Taken per voice, as first written, there were 7 to 16 frames, so the 95th percentile
    // was simply the worst single frame, which read 4.3 dB on a frame that is part voice. That frame is reported below as well.
    check (percentile (allHiss, 95.0) <= limits::speechHissLevelDb, "say speech, Pitch +/-7: hiss level in consonant frames, all voices, 95th percentile change", percentile (allHiss, 95.0), "dB");
    info ("  median change in those frames", percentile (allHiss, 50.0), "dB");
    info ("  worst single frame", percentile (allHiss, 100.0), "dB");
    info ("  frames counted", (double) allHiss.size());

    // No character makes a mess of real speech: finite, about the same level.
    double worstLevel = 0.0;
    const auto in = loadWav ("speech_kathy.wav");
    for (int c = 0; c < 5; ++c)
    {
        auto p = make();
        setParam (*p, "character", (float) c);
        const auto out = run (*p, in);
        worstLevel = std::max (worstLevel, std::abs (levelChange (in, out, 0)));
    }
    check (worstLevel <= 3.0, "say speech through every Character, level against the input, worst change", worstLevel, "dB");
}

// ------------------------------------------------------------------------------------------------- Peak Guard (1.2.0)

void testPeakGuard()
{
    // A hot voice: its highest peak is just under full scale.
    auto in = voice::synth (voice::vibrato (147.0, 3.0, SR, 40.0, 5.0), voice::vowelA, SR, -40.0);
    {
        const double pk = peak (in.data(), (int) in.size());
        for (auto& v : in) v = (float) (v * 0.99 / pk);
    }
    const double inPeak = peak (in.data(), (int) in.size());
    const long from = (long) (0.3 * SR);
    {
        auto p = make();
        check (p->apvts.getRawParameterValue ("peakGuard")->load() < 0.5f, "Peak Guard is off by default");
        setParam (*p, "peakGuard", 1.0f);
        for (int i = 0; i < p->getNumPrograms(); ++i)
            p->setCurrentProgram (i);
        check (p->apvts.getRawParameterValue ("peakGuard")->load() >= 0.5f, "no preset changes Peak Guard");
    }
    {
        auto p = make();
        setParam (*p, "peakGuard", 1.0f);
        const auto out = run (*p, in);
        const double d = nullAgainstDelayed (in, out, p->getLatencySamples(), from + p->getLatencySamples());
        check (d <= limits::guardNullDb, "Peak Guard on at the default settings: output against the delayed input", d, "dB");
    }
    double worstOver = -100.0, worstPast = -100.0;
    long samplesOver = 0;
    for (int preset : { 11, 5, 3, 1 }) // Giant, Monster Growl, Robot Announcer, Deep Voice
    {
        auto off = make(), on = make();
        off->setCurrentProgram (preset);
        on->setCurrentProgram (preset);
        setParam (*on, "peakGuard", 1.0f);
        const auto a = run (*off, in), b = run (*on, in);
        const double peakOff = peak (a.data(), (int) a.size()), peakOn = peak (b.data(), (int) b.size());
        long over = 0;
        for (float v : b) if (std::abs (v) > (float) inPeak) ++over;
        samplesOver += over;
        const double took = std::max (0.0, toDb (peakOff) - toDb (inPeak));
        const double fell = toDb (rms (a.data() + from, (int) a.size() - (int) from)) - toDb (rms (b.data() + from, (int) b.size() - (int) from));
        std::printf ("  %s on a voice that peaks at %.2f dBFS: peak %+.2f dBFS with the guard off, %+.2f on, level down %.2f dB\n", off->getProgramName (preset).toRawUTF8(), toDb (inPeak), toDb (peakOff), toDb (peakOn), fell);
        worstOver = std::max (worstOver, toDb (peakOff) - toDb (inPeak));
        worstPast = std::max (worstPast, fell - took);
    }
    info ("  with Peak Guard off the output's peak passes the input's by, worst preset", worstOver, "dB");
    check (samplesOver <= limits::guardSamplesOver, "Peak Guard on, four presets on a voice just under full scale: samples over the input's highest peak", (double) samplesOver);
    {
        // Every preset, Output included (1.2.3: Deep Double's Output +2.5 dB went straight past the ceiling, over full scale on a hot take).
        long over = 0;
        auto first = make();
        for (int preset = 0; preset < first->getNumPrograms(); ++preset)
        {
            auto on = make();
            on->setCurrentProgram (preset);
            setParam (*on, "peakGuard", 1.0f);
            const auto b = run (*on, in);
            long n = 0;
            for (float v : b) if (std::abs (v) > (float) inPeak) ++n;
            if (n > 0) std::printf ("  %s with Peak Guard on: %ld samples over the input's highest peak, peak %+.2f dBFS\n", on->getProgramName (preset).toRawUTF8(), n, toDb (peak (b.data(), (int) b.size())));
            over += n;
        }
        check (over <= limits::guardSamplesOver, "Peak Guard on, every preset, Output included, on a voice just under full scale: samples over the input's highest peak", (double) over);
    }
    check (worstPast <= limits::guardLevelPastPeakDb, "Peak Guard on: the level falls by no more than the peak it took off plus, worst", worstPast, "dB");
    {
        // Switched off again it lets go, and then the output is the same samples as if it had never been on.
        auto never = make(), was = make();
        never->setCurrentProgram (11);
        was->setCurrentProgram (11);
        const auto a = run (*never, in);
        prepare (*was, 1, SR, 256);
        setParam (*was, "peakGuard", 1.0f);
        std::vector<float> first (in.begin(), in.begin() + (long) SR), rest (in.begin() + (long) SR, in.end());
        auto b = mono (render (*was, first, 256));
        setParam (*was, "peakGuard", 0.0f);
        const auto c = mono (render (*was, rest, 256));
        b.insert (b.end(), c.begin(), c.end());
        long differ = 0;
        for (size_t i = (size_t) (2.5 * SR); i < std::min (a.size(), b.size()); ++i)
            if (a[i] != b[i]) ++differ;
        check (differ == 0, "Peak Guard switched off again: from 1.5 s later the output is the same samples as if it had never been on", (double) differ);
    }
}

// A soft note with two strong harmonics and a formant between them (found on a real take, 1.2.0). The fit to the harmonics is free
// between them, and it drew a peak 21 dB over both, so with Pitch +7 the moved fundamental landed on it and came out that much louder.
void testSoftNoteBurst()
{
    const long n = (long) (1.5 * SR);
    std::vector<float> in ((size_t) n);
    double phase = 0.0;
    for (long i = 0; i < n; ++i)
    {
        const double t = (double) i / SR;
        const double hz = 205.0 * std::pow (2.0, 0.25 * std::min (1.0, t / 0.3)); // glides up a quarter octave, then holds
        phase += 2.0 * M_PI * hz / SR;
        const double fade = std::min (1.0, t / 0.02) * std::min (1.0, (1.5 - t) / 0.05);
        in[(size_t) i] = (float) (fade * 0.03 * (std::sin (phase) + 0.55 * std::sin (2.0 * phase + 0.4) + 0.045 * std::sin (3.0 * phase + 1.1) + 0.02 * std::sin (8.0 * phase)));
    }
    double worst = -100.0;
    for (double shift : { 7.0, 5.0, -7.0 })
    {
        auto p = make();
        setParam (*p, "pitch", (float) shift);
        const auto out = run (*p, in);
        const int L = p->getLatencySamples();
        const int frame = 480;
        double w = -100.0, at = 0.0;
        for (long i = (long) (0.05 * SR); i + frame + L < (long) out.size() && i + frame < n - (long) (0.1 * SR); i += frame)
        {
            // against the loudest input frame within 20 ms, so a note that only starts a little early or late is not counted
            double loudest = 0.0;
            for (int k = -2; k <= 2; ++k)
                loudest = std::max (loudest, rms (in.data() + std::max (0L, i + k * frame), frame));
            const double over = toDb (rms (out.data() + i + L, frame)) - toDb (loudest);
            if (over > w) { w = over; at = (double) i / SR; }
            if (std::getenv ("MORPH_BURST") != nullptr && over > 3.0)
                std::printf ("    %.2f s: %+.2f dB\n", (double) i / SR, over);
        }
        std::printf ("  soft note with two strong harmonics, Pitch %+.0f: loudest 10 ms frame %+.2f dB over the input's, at %.2f s\n", shift, w, at);
        worst = std::max (worst, w);
    }
    check (worst <= limits::guardBurstDb, "a soft note with two strong harmonics, Pitch +7, +5 and -7: loudest 10 ms frame over the input's, worst", worst, "dB");
}

// ------------------------------------------------------------------------------------------------- input level

// Nothing in the plug-in should depend on how loud the voice was recorded (1.2.1, from the real vocal re-test, where Pitch +7 on a
// take times 4 read -2.9 dB against the plain render). A loud phrase and a quiet one (the quiet one under the old -60 dBFS gate,
// over it times 4), Pitch +7, the input times exactly 4 and times exactly 0.25 (no rounding), scaled back: each phrase against the
// render at the plain level.
void testInputLevel()
{
    auto loud = voice::synth (voice::steady (190.0, 1.0, SR), voice::vowelA, SR, -60.0, -18.0, 5);
    const auto quiet = voice::synth (voice::steady (150.0, 1.0, SR), voice::vowelE, SR, -60.0, -66.0, 6);
    const long split = (long) loud.size();
    std::vector<float> in = loud;
    in.insert (in.end(), quiet.begin(), quiet.end());
    in.resize (in.size() + (size_t) (0.2 * SR), 0.0f);
    const auto render1 = [&] (float scale)
    {
        auto p = make();
        setParam (*p, "pitch", 7.0f);
        std::vector<float> x = in;
        for (auto& v : x) v *= scale;
        auto o = run (*p, x);
        for (auto& v : o) v /= scale;
        return o;
    };
    const auto plain = render1 (1.0f);
    double worst = -400.0;
    for (float scale : { 4.0f, 0.25f })
    {
        const auto o = render1 (scale);
        for (int part = 0; part < 2; ++part)
        {
            const long a = part == 0 ? 0 : split, b = part == 0 ? split : (long) plain.size();
            double e = 0.0, s = 0.0;
            for (long i = a; i < b; ++i) { const double d = (double) o[(size_t) i] - plain[(size_t) i]; e += d * d; s += (double) plain[(size_t) i] * plain[(size_t) i]; }
            const double nb = 10.0 * std::log10 (std::max (e, 1.0e-30) / std::max (s, 1.0e-30));
            std::printf ("  input times %.2f, %s phrase: %.1f dB against the plain level\n", scale, part == 0 ? "loud" : "quiet", nb);
            worst = std::max (worst, nb);
        }
    }
    check (worst <= limits::inputLevelNullDb, "Pitch +7 with the input times 4 and times 0.25, scaled back, against the plain level, worst phrase", worst, "dB");
}

// ------------------------------------------------------------------------------------------------- phrase starts

// The level of a phrase's first 30 ms, output against input (1.2.2, from the real vocal re-test, where every phrase start at Robot came
// out 3.7 dB quiet, median). Phrases of a made up voice, each after a gap long enough to be a consonant or a breath (the level match lets
// go there), at the settings that read worst on the real takes. The real vocal check's limits, set before this test ran: median -1 dB or
// more, 5th percentile -3 dB or more.
void testPhraseStarts()
{
    const int gapLen = (int) (0.12 * SR);
    std::vector<float> in;
    std::vector<long> starts;
    const double notes[] = { 147.0, 165.0, 131.0, 175.0, 156.0, 139.0, 196.0, 123.0 };
    int k = 0;
    for (double hz : notes)
    {
        in.resize (in.size() + (size_t) gapLen, 0.0f);
        starts.push_back ((long) in.size());
        const auto ph = voice::synth (voice::vibrato (hz, 0.45, SR, 10.0, 5.0), k % 2 == 0 ? voice::vowelA : voice::vowelE, SR, -50.0, -20.0, 7 + (unsigned) k);
        in.insert (in.end(), ph.begin(), ph.end());
        ++k;
    }
    in.resize (in.size() + (size_t) gapLen, 0.0f);
    struct Setting { const char* name; std::vector<std::pair<const char*, float>> set; };
    const std::vector<Setting> settings = { { "Robot", { { "character", 3.0f } } }, { "Giant", { { "pitch", -12.0f }, { "formant", -6.0f } } },
                                            { "Chipmunk", { { "pitch", 12.0f }, { "formant", 8.0f } } }, { "Monster", { { "character", 4.0f } } },
                                            { "Child", { { "character", 2.0f } } } };
    double worstMedian = 100.0, worstP5 = 100.0;
    for (const auto& st : settings)
    {
        auto p = make();
        for (const auto& [id, v] : st.set) setParam (*p, id, v);
        const auto out = run (*p, in);
        const int L = p->getLatencySamples(), n30 = (int) (0.030 * SR);
        std::vector<double> d;
        // the first start comes before the level match has heard anything: left out, as the real check has no start at sample 0 either
        for (size_t i = 1; i < starts.size(); ++i)
            d.push_back (toDb (rms (out.data() + starts[i] + L, n30)) - toDb (rms (in.data() + starts[i], n30)));
        const double med = median (d), p5 = percentile (d, 5.0);
        std::printf ("  %-8s first 30 ms of a phrase against the input: median %+.2f dB, 5th percentile %+.2f dB\n", st.name, med, p5);
        worstMedian = std::min (worstMedian, med);
        worstP5 = std::min (worstP5, p5);
    }
    check (worstMedian >= limits::startLevelMedianDb, "phrase starts, first 30 ms against the input, median, worst setting", worstMedian, "dB");
    check (worstP5 >= limits::startLevelP5Db, "phrase starts, first 30 ms against the input, 5th percentile, worst setting", worstP5, "dB");
}

// ------------------------------------------------------------------------------------------------- Range Match

void testRangeMatch()
{
    const double centre = 220.0;
    double worst = 0.0;
    for (double hz : { 110.0, 156.0, 196.0, 277.0, 392.0, 440.0 })
    {
        auto p = make();
        setParam (*p, "rangeMatch", 1.0f);
        setParam (*p, "rangeCentre", 57.0f);
        const auto in = voice::synth (voice::steady (hz, 3.0, SR), voice::vowelA, SR, -40.0);
        const auto out = run (*p, in);
        const auto off = centsOff (out.data(), (long) out.size(), SR, centre, 1.5, 2.9, 0.01, 0.1);
        const double e = off.empty() ? 9999.0 : std::abs (median (off)) / 100.0;
        std::printf ("  Range Match from %.0f Hz: output off by %.2f st, offset reading %.0f st\n", hz, e, p->getEngine().rangeOffset());
        worst = std::max (worst, e);
    }
    check (worst <= limits::rangeMatchSemitones, "Range Match: a voice anywhere over two octaves lands on A3, worst", worst, "st");

    // Switched off, the voice is back to itself as fast as a Pitch move (added 1.2.1, from the real vocal re-test: it took 1.4 s).
    {
        auto q = make();
        setParam (*q, "rangeMatch", 1.0f);
        const auto v = voice::synth (voice::steady (110.0, 5.0, SR), voice::vowelA, SR, -40.0);
        const int block = 256;
        prepare (*q, 1, SR, block);
        const int offAt = (int) (2.5 * SR / block);
        const auto o = mono (render (*q, v, block, nullptr, [&] (int idx) { if (idx == offAt) setParam (*q, "rangeMatch", 0.0f); }));
        const int L = q->getLatencySamples();
        const long off = (long) offAt * block;
        double e = 0.0, s = 0.0;
        for (long i = off + (long) (0.5 * SR); i < off + (long) (1.5 * SR); ++i)
        {
            const double d = (double) o[(size_t) i] - v[(size_t) (i - L)];
            e += d * d; s += (double) v[(size_t) (i - L)] * v[(size_t) (i - L)];
        }
        const double nb = 10.0 * std::log10 (std::max (e, 1.0e-30) / std::max (s, 1.0e-30));
        check (nb <= limits::rangeMatchBackNullDb, "Range Match switched off at +12 st: 0.5 to 1.5 s later, against the delayed input", nb, "dB");
    }

    // A preset chosen, then the default chosen again: once it has settled the voice passes straight through (1.2.3, from the real
    // vocal re-test: from 1.2.2 the level match's memory of the gain other settings needed kept every later phrase start a hair off,
    // -108 to -118 dB on real takes where the check's limit is -120). Real speech, so there are many starts after the move back.
    {
        std::vector<float> v;
        for (const char* name : { "speech_samantha.wav", "speech_daniel.wav", "speech_kathy.wav" })
        {
            const auto x = loadWav (name);
            v.insert (v.end(), x.begin(), x.end());
        }
        check (v.size() > (size_t) (8.0 * SR), "found the three speech files for the move back test");
        double worstBack = -400.0;
        for (int preset : { 11, 5, 6, 10 }) // Giant, Monster Growl, Ghost Whisper, Chipmunk
        {
            auto q = make();
            const int block = 256;
            prepare (*q, 1, SR, block);
            const int toAt = (int) (0.3 * SR / block), backAt = (int) (3.0 * SR / block);
            const auto o = mono (render (*q, v, block, nullptr, [&] (int idx) { if (idx == toAt) q->setCurrentProgram (preset); if (idx == backAt) q->setCurrentProgram (0); }));
            const int L = q->getLatencySamples();
            double e = 0.0, s2 = 0.0;
            for (long i = (long) backAt * block + (long) (3.0 * SR); i < (long) o.size(); ++i)
            {
                const double d = (double) o[(size_t) i] - v[(size_t) (i - L)];
                e += d * d; s2 += (double) v[(size_t) (i - L)] * v[(size_t) (i - L)];
            }
            const double nb = 10.0 * std::log10 (std::max (e, 1.0e-30) / std::max (s2, 1.0e-30));
            std::printf ("  %s, then Clean Slate: from 3 s after, %.1f dB against the delayed input\n", q->getProgramName (preset).toRawUTF8(), nb);
            worstBack = std::max (worstBack, nb);
        }
        check (worstBack <= limits::backToDefaultNullDb, "a preset and back to Clean Slate: from 3 s after, against the delayed input, worst", worstBack, "dB");
    }

    // Off does nothing
    auto p = make();
    setParam (*p, "rangeCentre", 40.0f);
    const auto in = voice::synth (voice::steady (196.0, 1.0, SR), voice::vowelA, SR, -40.0);
    const auto out = run (*p, in);
    const double n = nullAgainstDelayed (in, out, p->getLatencySamples(), (long) (0.2 * SR));
    check (n <= limits::neutralNullDb, "Range Centre does nothing while Range Match is off", n, "dB");
}

// ------------------------------------------------------------------------------------------------- output, presets, blocks, stereo

void testOutputAndPresets()
{
    const auto in = voice::synth (voice::steady (196.0, 1.5, SR), voice::vowelA, SR, -40.0);
    {
        auto p = make();
        setParam (*p, "output", -6.0f);
        const double d = levelChange (in, run (*p, in), (long) (0.3 * SR));
        check (std::abs (d + 6.0) <= 0.1, "Output -6 dB measures", d, "dB");
    }
    auto q = make();
    std::vector<std::vector<float>> outs;
    for (int i = 0; i < q->getNumPrograms(); ++i)
    {
        q->setCurrentProgram (i);
        const auto out = run (*q, in);
        const double d = levelChange (in, out, (long) (0.3 * SR));
        std::printf ("  preset %s: level %+.1f dB\n", q->getProgramName (i).toRawUTF8(), d);
        check (std::abs (d) <= 6.0, std::string ("preset ") + q->getProgramName (i).toStdString() + " sits near the input level", d, "dB");
        // (1.2.2) the real vocal check's level rule for every setting, set before this test ran: Deep Double read -3.0 here and on real takes
        check (std::abs (d) <= limits::presetLevelDb, std::string ("preset ") + q->getProgramName (i).toStdString() + " level within the real vocal check's limit", d, "dB");
        outs.push_back (out);
    }
    bool differ = true;
    for (size_t a = 0; a < outs.size(); ++a)
        for (size_t b = a + 1; b < outs.size(); ++b)
        {
            double e = 0.0;
            for (size_t i = (size_t) (0.3 * SR); i < outs[a].size(); ++i) { const double d = outs[a][i] - outs[b][i]; e += d * d; }
            if (e < 1.0e-3) differ = false;
        }
    check (differ, "every preset sounds different from every other");
}

void testBlocksAndStereo()
{
    const auto in = voice::synth (voice::vibrato (196.0, 2.0, SR, 25.0, 5.5), voice::vowelE, SR, -35.0);
    auto setUp = [] (PluginProcessor& p)
    {
        setParam (p, "pitch", 4.0f);
        setParam (p, "formant", -2.0f);
        setParam (p, "whisper", 20.0f);
    };
    auto a = make(); setUp (*a);
    const auto reference = run (*a, in, SR, 1, 512);
    double worst = 0.0;
    for (int block : { 1, 32, 64, 441, 4096 })
    {
        auto b = make(); setUp (*b);
        const auto out = run (*b, in, SR, 1, block);
        double e = 0.0;
        for (size_t i = 0; i < out.size(); ++i) e = std::max (e, (double) std::abs (out[i] - reference[i]));
        worst = std::max (worst, e);
    }
    check (worst <= 1.0e-5, "any block size gives the same output, biggest difference", worst);

    // prepareToPlay again gives the same result from the start
    const auto again = run (*a, in, SR, 1, 512);
    double diff = 0.0;
    for (size_t i = 0; i < again.size(); ++i) diff = std::max (diff, (double) std::abs (again[i] - reference[i]));
    check (diff <= 1.0e-5, "preparing again starts clean, biggest difference", diff);

    // Stereo: same grains and same filter both sides, so a level difference stays what it was
    auto s = make();
    setParam (*s, "pitch", 5.0f);
    prepare (*s, 2, SR, 256);
    juce::AudioBuffer<float> buf (2, 256), all (2, (int) in.size());
    juce::MidiBuffer midi;
    for (int at = 0; at + 256 <= (int) in.size(); at += 256)
    {
        std::copy_n (in.data() + at, 256, buf.getWritePointer (0));
        for (int i = 0; i < 256; ++i) buf.setSample (1, i, 0.5f * in[(size_t) (at + i)]);
        s->processBlock (buf, midi);
        all.copyFrom (0, at, buf, 0, 0, 256);
        all.copyFrom (1, at, buf, 1, 0, 256);
    }
    const double balance = rmsDb (all, 1, (int) (0.3 * SR)) - rmsDb (all, 0, (int) (0.3 * SR));
    check (std::abs (balance + 6.02) <= 0.2, "stereo balance holds: right was 6 dB down, reads", balance, "dB");
}

// ---- the window ----------------------------------------------------------------------------------------------------

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

// Runs a voice through with a window open, a frame at a time, and leaves the mirror showing its end.
void playWith (PluginProcessor& p, MorphEditor& w, const std::vector<float>& x, std::function<void()> eachFrame = nullptr)
{
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    for (int pos = 0; pos + 256 <= (int) x.size(); pos += 256)
    {
        for (int ch = 0; ch < 2; ++ch)
            buffer.copyFrom (ch, 0, x.data() + pos, 256);
        p.processBlock (buffer, midi);
        if (pos % 1600 < 256)
        {
            w.tick();
            if (eachFrame != nullptr)
                eachFrame();
        }
    }
}

double semitonesOfPoint (int point) { return 12.0 * point * std::log2 (Mirror::highestHz / Mirror::lowestHz) / (Mirror::numPoints - 1); }

int pointOfHz (double hz) { return juce::roundToInt (std::log2 (hz / Mirror::lowestHz) / std::log2 (Mirror::highestHz / Mirror::lowestHz) * (Mirror::numPoints - 1)); }

// The first real tooth up from the bottom.
int lowestTooth (const std::vector<float>& shape)
{
    for (int i = 2; i + 2 < (int) shape.size(); ++i)
        if (shape[(size_t) i] > 0.35f && shape[(size_t) i] >= shape[(size_t) i - 1] && shape[(size_t) i] >= shape[(size_t) i + 1]
            && shape[(size_t) i] - shape[(size_t) juce::jmax (0, i - 6)] > 0.08f)
            return i;
    return -1;
}

std::vector<float> smoothed (const std::vector<float>& shape, int reach)
{
    std::vector<float> out (shape.size());
    for (int i = 0; i < (int) shape.size(); ++i)
    {
        float sum = 0.0f;
        int n = 0;
        for (int k = juce::jmax (0, i - reach); k <= juce::jmin ((int) shape.size() - 1, i + reach); ++k) { sum += shape[(size_t) k]; ++n; }
        out[(size_t) i] = sum / (float) n;
    }
    return out;
}

void shapesOf (Mirror& m, std::vector<float>& you, std::vector<float>& ego)
{
    you.resize (Mirror::numPoints);
    ego.resize (Mirror::numPoints);
    for (int i = 0; i < Mirror::numPoints; ++i) { you[(size_t) i] = m.youAt (i); ego[(size_t) i] = m.egoAt (i); }
}

void testWindow()
{
    using ui::DotSlider;
    auto holder = make();
    auto& p = *holder;
    prepare (p, 2, SR, 256);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    auto* w = dynamic_cast<MorphEditor*> (ed.get());
    check (w != nullptr, "the window is the custom one");
    if (w == nullptr)
        return;
    w->setAdvancedOpen (true);

    // Nothing lost: every parameter a person can change has a control in the window, under its own ID, and keeps its name.
    {
        int missing = 0, controls = 0;
        for (auto* prm : p.getParameters())
        {
            const auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm);
            if (withId == nullptr || dynamic_cast<ReadingParameter*> (prm) != nullptr)
                continue;
            ++controls;
            juce::String id = withId->paramID;
            // Pitch and Formant Shift live on the pad and in the two big values.
            if (id == "pitch" || id == "formant")
                id += "Value";
            auto* c = ed->findChildWithID (id);
            if (c == nullptr || c->getWidth() < 20 || ! c->isVisible())
            {
                ++missing;
                std::printf ("  no control for %s\n", withId->paramID.toRawUTF8());
            }
        }
        // 14 since 1.2.0 added Peak Guard (13 before). The rule is the same: none without a control.
        check (missing == 0 && controls == 14, "every one of the 14 parameters has a visible control in the window", (double) controls);
        check (ed->findChildWithID ("pad") != nullptr, "the pad is there");

        // The 13 controls of 1.1.1 in their old order, then Peak Guard (new in 1.2.0, added after them), then the three readings.
        const juce::StringArray names { "Pitch", "Formant Shift", "Character", "Character Amount", "Robot Note", "Vibrato Rate", "Vibrato Depth", "Whisper", "Range Match", "Range Centre", "Mix", "Output", "Bypass", "Peak Guard", "Detected Pitch", "Output Pitch", "Range Offset" };
        bool same = names.size() == p.getParameters().size();
        for (int i = 0; same && i < names.size(); ++i)
            same = p.getParameters()[i]->getName (64) == names[i];
        check (same, "parameter names and order are unchanged, so automation and old sessions still find them");
    }

    // The hint line is one line, at the smallest window.
    {
        const float room = (float) (PluginProcessor::minWindowWidth - 40 - 12 - 260) * 0.6f;
        float widest = 0.0f;
        const auto hintFont = ui::font (ui::Weight::regular, 13.0f);
        for (const auto& hint : MorphEditor::allHints (*w))
        {
            widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (hintFont, hint));
            check (! hint.containsChar ((juce::juce_wchar) 0x2014), "no em dash in a hint");
        }
        check (widest > 100.0f && widest <= room, "every hint fits on the hint line at the smallest window, widest in points", (double) widest);
    }

    // The pad: one gesture a drag, on both parameters, and the dot follows automation.
    {
        auto& pad = w->getPad();
        ParamWatch watch (p);
        const auto start = pad.pointFor (6.0, 3.0);
        pad.mouseDown (mouseAt (pad, start));
        check (std::abs (realValue (p, "pitch") - 6.0f) < 0.05f && std::abs (realValue (p, "formant") - 3.0f) < 0.05f, "a click on the pad sets Pitch and Formant Shift, Pitch", (double) realValue (p, "pitch"));
        check (watch.begins == 2 && watch.ends == 0, "and opens one gesture on each", (double) watch.begins.load());
        const auto next = pad.pointFor (-9.0, -4.0);
        pad.mouseDrag (mouseAt (pad, next, 1, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), start));
        check (std::abs (realValue (p, "pitch") + 9.0f) < 0.05f && std::abs (realValue (p, "formant") + 4.0f) < 0.05f, "dragging moves both", (double) realValue (p, "formant"));
        check (watch.begins == 2, "with no new gesture while dragging");
        pad.mouseUp (mouseAt (pad, next));
        check (watch.begins == 2 && watch.ends == 2, "letting go closes both", (double) watch.ends.load());

        setParam (p, "pitch", 12.0f);
        setParam (p, "formant", 6.0f);
        check (pad.voiceAt().getDistanceFrom (pad.pointFor (12.0, 6.0)) < 0.5f, "automation moves the dot");

        // The dot is the voice, so a Character's part shows in it, and comes off again when dragging.
        setParam (p, "pitch", 0.0f);
        setParam (p, "formant", 0.0f);
        setParam (p, "character", 1.0f);
        check (pad.voiceAt().getDistanceFrom (pad.pointFor (-5.0, -3.0)) < 0.5f, "Deep moves the dot by the Character's own amounts");
        pad.mouseDown (mouseAt (pad, pad.pointFor (-5.0, -3.0)));
        pad.mouseDrag (mouseAt (pad, pad.pointFor (-2.0, -1.0), 1, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), pad.pointFor (-5.0, -3.0)));
        pad.mouseUp (mouseAt (pad, pad.pointFor (-2.0, -1.0)));
        check (std::abs (realValue (p, "pitch") - 3.0f) < 0.1f && std::abs (realValue (p, "formant") - 2.0f) < 0.1f, "dragging with a Character sets what is left after it", (double) realValue (p, "pitch"));

        // Shift snaps the voice to whole semitones.
        const auto shift = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
        pad.mouseDown (mouseAt (pad, pad.pointFor (3.3, 2.4), 1, shift));
        pad.mouseUp (mouseAt (pad, pad.pointFor (3.3, 2.4)));
        check (std::abs (realValue (p, "pitch") - 8.0f) < 0.02f && std::abs (realValue (p, "formant") - 5.0f) < 0.02f, "holding Shift lands on whole semitones", (double) realValue (p, "pitch"));

        setParam (p, "character", 0.0f);
        pad.mouseDoubleClick (mouseAt (pad, pad.pointFor (3.0, 3.0), 2));
        check (std::abs (realValue (p, "pitch")) < 0.005f && std::abs (realValue (p, "formant")) < 0.005f, "double-click on the pad resets the dot", (double) realValue (p, "pitch"));
    }

    // The big values type and reset, and take the sign they show.
    {
        auto* value = dynamic_cast<ui::ValueField*> (ed->findChildWithID ("pitchValue"));
        check (value != nullptr, "the Pitch value is there");
        value->applyTyped ("7.5");
        check (std::abs (realValue (p, "pitch") - 7.5f) < 0.01f, "typing a pitch sets it");
        value->applyTyped (juce::String::fromUTF8 ("\xe2\x88\x92" "3"));
        check (std::abs (realValue (p, "pitch") + 3.0f) < 0.01f, "the minus sign the window shows can be typed back", (double) realValue (p, "pitch"));
        value->applyTyped ("hello");
        check (std::abs (realValue (p, "pitch") + 3.0f) < 0.01f, "a word that is not a number changes nothing");
        value->applyTyped ("999");
        check (std::abs (realValue (p, "pitch") - 24.0f) < 0.005f, "too much stops at the end of the range", (double) realValue (p, "pitch"));
        value->mouseDoubleClick (mouseAt (*value, { 5.0f, 5.0f }, 2));
        check (std::abs (realValue (p, "pitch")) < 0.005f, "double-click on the value resets it", (double) realValue (p, "pitch"));
    }

    // Sliders: drag gestures come from the attachment, and typing and double-click work.
    {
        auto* whisper = dynamic_cast<DotSlider*> (ed->findChildWithID ("whisper"));
        auto* note = dynamic_cast<DotSlider*> (ed->findChildWithID ("robotNote"));
        check (whisper != nullptr && note != nullptr, "the sliders are there");
        ParamWatch watch (p);
        whisper->applyTyped ("40");
        check (std::abs (realValue (p, "whisper") - 40.0f) < 0.1f && watch.begins == 1 && watch.ends == 1, "typing a value sets it, as one gesture", (double) realValue (p, "whisper"));
        setParam (p, "whisper", 70.0f);
        check (std::abs (whisper->getValue() - 70.0) < 0.1, "automation moves the slider");
        check (whisper->valueText() == "70%", "and it reads what it says");
        whisper->mouseDoubleClick (mouseAt (*whisper, { 40.0f, 35.0f }, 2));
        check (realValue (p, "whisper") == 0.0f, "double-click on a slider resets it", (double) realValue (p, "whisper"));
        note->applyTyped ("C4");
        check (juce::roundToInt (realValue (p, "robotNote")) == 60, "a note can be typed by name", (double) realValue (p, "robotNote"));
        check (note->valueText() == "C4", "and is shown by name");
        setParam (p, "robotNote", 45.0f);
    }

    // Chips.
    {
        auto& chips = w->getChips();
        ParamWatch watch (p);
        const auto robot = chips.chipBounds (3).getCentre();
        chips.mouseDown (mouseAt (chips, robot));
        check (juce::roundToInt (realValue (p, "character")) == 3 && watch.begins == 1 && watch.ends == 1, "clicking a chip picks that Character, as one gesture");
        setParam (p, "character", 2.0f);
        check (chips.getSelected() == 2, "automation moves the chips");
        chips.mouseDoubleClick (mouseAt (chips, robot, 2));
        check (juce::roundToInt (realValue (p, "character")) == 0, "double-click goes back to Natural");
    }

    // Presets, and the window's sentence.
    {
        setParam (p, "character", 0.0f);
        p.setCurrentProgram (4);
        w->tick();
        check (! p.isCurrentProgramEdited(), "a preset just loaded is not edited");
        setParam (p, "whisper", 30.0f);
        check (p.isCurrentProgramEdited(), "moving a control marks it edited");
        p.setCurrentProgram (0);
        for (int i = 0; i < 8; ++i) w->tick();
        check (w->statusText() == "Your voice, untouched.", "Clean Slate says the voice is untouched: " + w->statusText().toStdString());
        setParam (p, "character", 4.0f);
        for (int i = 0; i < 8; ++i) w->tick();
        check (w->statusText().contains ("Monster") && w->statusText().contains ("growl"), "and says what the Character adds: " + w->statusText().replace ("\n", " ").toStdString());
        check (! w->statusText().containsChar ((juce::juce_wchar) 0x2014), "no em dash in it");
        setParam (p, "character", 0.0f);
    }

    // Size and Advanced are saved, nothing else is added, and damage goes back in range.
    {
        w->setAdvancedOpen (true);
        p.windowWidth = 1000;
        p.windowHeight = 800;
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        auto other = make();
        other->setStateInformation (saved.getData(), (int) saved.getSize());
        check (other->windowWidth.load() == 1000 && other->windowHeight.load() == 800 && other->advancedOpen.load(), "the size and Advanced come back with the session", (double) other->windowWidth.load());
        check (! other->apvts.state.hasProperty ("windowWidth") && ! other->apvts.state.hasProperty ("windowHeight") && ! other->apvts.state.hasProperty ("advancedOpen"), "they stay out of the parameter tree");

        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        const int savedAttributes = xml->getNumAttributes();
        check (savedAttributes == 3, "only the size and Advanced were added to saved state", (double) savedAttributes);
        xml->setAttribute ("windowWidth", 99999);
        xml->setAttribute ("windowHeight", "nonsense");
        juce::MemoryBlock damaged;
        juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
        other->setStateInformation (damaged.getData(), (int) damaged.getSize());
        check (other->windowWidth.load() == PluginProcessor::maxWindowWidth && other->windowHeight.load() == PluginProcessor::minWindowHeight, "a damaged size goes back in range", (double) other->windowWidth.load());

        xml->removeAttribute ("windowWidth");
        xml->removeAttribute ("windowHeight");
        xml->removeAttribute ("advancedOpen");
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        auto fresh = make();
        fresh->setStateInformation (old.getData(), (int) old.getSize());
        check (fresh->windowWidth.load() == PluginProcessor::defaultWindowWidth && ! fresh->advancedOpen.load(), "a session from before the window opens at the default size");

        std::unique_ptr<juce::AudioProcessorEditor> reopened (other->createEditor());
        check (reopened->getWidth() == PluginProcessor::maxWindowWidth && reopened->getHeight() >= PluginProcessor::minWindowHeight, "a window opens within limits from a damaged size", (double) reopened->getHeight());
    }

    ed.reset();

    // The read: off with no window, exact with one, and it never allocates.
    {
        const auto x = voice::synth (voice::steady (196.0, 1.0, SR), voice::vowelA, SR);
        auto q = make();
        prepare (*q, 2, SR, 256);
        render (*q, x, 256);
        check (q->getTap().position() == 0, "with no window open, the read is not even written");

        std::unique_ptr<juce::AudioProcessorEditor> open (q->createEditor());
        check (q->getTap().isWanted(), "opening a window turns it on");
        const auto before = allocs.load();
        long during = 0;
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        std::vector<float> outputs;
        for (int pos = 0; pos + 256 <= (int) x.size(); pos += 256)
        {
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, x.data() + pos, 256);
            const auto a = allocs.load();
            q->processBlock (buffer, midi);
            during += allocs.load() - a;
            outputs.insert (outputs.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 256);
        }
        juce::ignoreUnused (before);
        check (during == 0, "no allocation on the audio thread with the read on", (double) during);

        const int blocks = (int) x.size() / 256;
        const auto end = q->getTap().position();
        check (end == (long long) blocks * 256, "the read counts every sample", (double) end);
        std::vector<float> seen (4096);
        q->getTap().readOutput (end, seen.data(), 4096);
        double worst = 0.0;
        for (int i = 0; i < 4096; ++i)
            worst = juce::jmax (worst, (double) std::abs (seen[(size_t) i] - outputs[outputs.size() - 4096 + (size_t) i]));
        check (worst == 0.0, "the output side of the read is the output, sample for sample", worst);
        q->getTap().readInput (end, seen.data(), 4096);
        worst = 0.0;
        const auto latency = q->getLatencySamples();
        for (int i = 0; i < 4096; ++i)
            worst = juce::jmax (worst, (double) std::abs (seen[(size_t) i] - x[(size_t) blocks * 256 - 4096 + (size_t) i - (size_t) latency]));
        check (worst < 1.0e-6, "the input side is the input, one plug-in delay earlier, so both sides show the same moment", worst);
        open.reset();
        check (! q->getTap().isWanted(), "closing the window turns it off");
    }

    // The mirror really shows the transformation.
    {
        const auto x = voice::synth (voice::steady (196.0, 1.5, SR), voice::vowelA, SR);
        std::vector<float> you, ego;

        const auto look = [&] (std::function<void (PluginProcessor&)> setUp, std::vector<float>& a, std::vector<float>& b) -> std::unique_ptr<PluginProcessor>
        {
            auto q = make();
            prepare (*q, 2, SR, 256);
            setUp (*q);
            std::unique_ptr<juce::AudioProcessorEditor> open (q->createEditor());
            auto* me = dynamic_cast<MorphEditor*> (open.get());
            playWith (*q, *me, x);
            shapesOf (me->getMirror(), a, b);
            return q;
        };

        look ([] (PluginProcessor&) {}, you, ego);
        double worst = 0.0;
        for (int i = 0; i < Mirror::numPoints; ++i)
            worst = juce::jmax (worst, (double) std::abs (you[(size_t) i] - ego[(size_t) i]));
        check (worst <= limits::mirrorNaturalShape, "Natural: the two sides of the mirror match", worst);
        double loudest = 0.0;
        for (auto v : you) loudest = juce::jmax (loudest, (double) v);
        check (loudest > 0.5, "and they are really showing a voice", loudest);

        look ([] (PluginProcessor& q) { setParam (q, "pitch", 12.0f); }, you, ego);
        const int y = lowestTooth (you), e = lowestTooth (ego);
        const double moved = y >= 0 && e >= 0 ? semitonesOfPoint (e) - semitonesOfPoint (y) : 99.0;
        check (std::abs (moved - 12.0) <= limits::mirrorPitchSemitones, "Pitch +12: the lowest tooth moves up, semitones", moved);

        look ([] (PluginProcessor& q) { setParam (q, "formant", 7.0f); }, you, ego);
        const auto ys = smoothed (you, 7), es = smoothed (ego, 7);
        const int from = pointOfHz (500.0), to = pointOfHz (4500.0);
        int bestLag = 0;
        double bestScore = 1.0e30;
        for (int lag = -6; lag <= 30; ++lag)
        {
            double sum = 0.0;
            for (int i = from; i < to && i + lag < Mirror::numPoints; ++i)
                sum += std::abs (ys[(size_t) i] - es[(size_t) (i + lag)]);
            if (sum < bestScore) { bestScore = sum; bestLag = lag; }
        }
        const double humps = semitonesOfPoint (bestLag);
        check (std::abs (humps - 7.0) <= limits::mirrorFormantSemitones, "Formant Shift +7: the humps move up, semitones", humps);

        look ([] (PluginProcessor& q) { setParam (q, "whisper", 100.0f); }, you, ego);
        const auto roughness = [] (const std::vector<float>& s)
        {
            const auto calm = smoothed (s, 5);
            double sum = 0.0;
            for (int i = pointOfHz (300.0); i < pointOfHz (2000.0); ++i)
                sum += std::abs (s[(size_t) i] - calm[(size_t) i]);
            return sum;
        };
        check (roughness (ego) <= limits::mirrorWhisperRoughness * roughness (you), "Whisper 100: the teeth melt, what is left against your side", roughness (ego) / roughness (you));

        {
            auto q = make();
            prepare (*q, 2, SR, 256);
            setParam (*q, "character", 3.0f);
            setParam (*q, "robotNote", 57.0f);
            std::unique_ptr<juce::AudioProcessorEditor> open (q->createEditor());
            auto* me = dynamic_cast<MorphEditor*> (open.get());
            playWith (*q, *me, x);
            check (std::abs (me->getMirror().getEgoHz() / 220.0 - 1.0) <= limits::mirrorRobotFraction, "Robot: the note shown is the note set, Hz", me->getMirror().getEgoHz());
            check (me->readingText().contains ("220 Hz") && me->readingText().contains ("196 Hz"), "and the window names both: " + me->readingText().toStdString());
        }

        {
            auto q = make();
            prepare (*q, 2, SR, 256);
            setParam (*q, "vibratoDepth", 100.0f);
            std::unique_ptr<juce::AudioProcessorEditor> open (q->createEditor());
            auto* me = dynamic_cast<MorphEditor*> (open.get());
            float low = 1.0e9f, high = -1.0e9f;
            int frame = 0;
            playWith (*q, *me, x, [&]
            {
                if (++frame > 10)
                {
                    low = juce::jmin (low, me->getPad().liveX());
                    high = juce::jmax (high, me->getPad().liveX());
                }
            });
            const float perSemitone = me->getPad().pointFor (1.0, 0.0).x - me->getPad().pointFor (0.0, 0.0).x;
            check ((double) (high - low) / perSemitone >= limits::padVibratoSemitones, "Vibrato: the disc on the pad swings with the voice, semitones end to end", (double) (high - low) / perSemitone);
            check (me->getPad().discSize() > 12.0f, "and it has a size while the voice sounds", (double) me->getPad().discSize());
        }
    }

    // The window only reads: frames and paints, with a voice going through, move no parameter and start no gesture.
    {
        const auto x = voice::synth (voice::steady (220.0, 1.0, SR), voice::vowelA, SR);
        auto q = make();
        prepare (*q, 2, SR, 256);
        std::unique_ptr<juce::AudioProcessorEditor> open (q->createEditor());
        auto* me = dynamic_cast<MorphEditor*> (open.get());
        ParamWatch watch (*q);
        bool moved = false;
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        for (int pos = 0; pos + 256 <= (int) x.size(); pos += 256)
        {
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, x.data() + pos, 256);
            q->processBlock (buffer, midi);
            if (pos % 1600 < 256)
            {
                moved = me->tick() || moved;
                paintAt (*open, pos % 3200 < 1600 ? 1.0f : 2.0f);
            }
        }
        check (moved, "with a voice going through, the display reports it is moving");
        check (watch.changes == 0 && watch.begins == 0 && watch.ends == 0, "the window reading and painting writes no parameter and no automation", (double) watch.changes.load());

        // With no more audio, the display settles and says so, so the timer can slow down and nothing repaints.
        bool still = true;
        for (int i = 0; i < 200; ++i)
            still = me->tick();
        check (! still, "with the host stopped, the display stops moving");

        // And it stays that way after a Character moves the dot with no sound going: only the picture changes, not the timer.
        setParam (*q, "character", 2.0f);
        for (int i = 0; i < 40; ++i)
            still = me->tick();
        check (! still, "a Character chosen in silence leaves nothing moving");
    }

    // Opening and closing, over and over, with audio running on another thread and with none.
    {
        auto q = make();
        prepare (*q, 2, SR, 256);
        std::atomic<bool> running { true };
        const auto x = voice::synth (voice::steady (196.0, 1.0, SR), voice::vowelA, SR);
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
            auto* me = dynamic_cast<MorphEditor*> (e.get());
            me->tick();
            paintAt (*e, i % 2 == 0 ? 2.0f : 1.0f);
            me->setAdvancedOpen (i % 3 == 0);
            me->tick();
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
        check (! q->getTap().isWanted(), "and the read is off again after the last one");

        auto unprepared = make();
        std::unique_ptr<juce::AudioProcessorEditor> before (unprepared->createEditor());
        paintAt (*before, 2.0f);
        check (before->getWidth() > 0, "a window opens and paints before the host has prepared the plug-in");

        // Every sample rate the plug-in takes, including one changing under an open window.
        auto r = make();
        prepare (*r, 2, 44100.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> e (r->createEditor());
        auto* me = dynamic_cast<MorphEditor*> (e.get());
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        bool finite = true;
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            r->prepareToPlay (rate, 256);
            for (int i = 0; i < 40; ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                    buffer.copyFrom (ch, 0, x.data() + i * 256, 256);
                r->processBlock (buffer, midi);
                me->tick();
            }
            paintAt (*e, 2.0f);
            for (int i = 0; i < Mirror::numPoints; ++i)
                finite = finite && std::isfinite (me->getMirror().egoAt (i));
        }
        check (finite, "the mirror follows a sample rate change under an open window");
    }

    // A double-click on the pad really does put the voice back: the output is the delayed input again.
    {
        const auto x = loadWav ("speech_samantha.wav");
        auto q = make();
        prepare (*q, 2, SR, 256);
        std::unique_ptr<juce::AudioProcessorEditor> open (q->createEditor());
        auto* me = dynamic_cast<MorphEditor*> (open.get());
        auto& pad = me->getPad();
        const auto at = pad.pointFor (7.0, 4.0);
        pad.mouseDown (mouseAt (pad, at));
        pad.mouseUp (mouseAt (pad, at));
        pad.mouseDoubleClick (mouseAt (pad, at, 2));
        q->reset();
        prepare (*q, 2, SR, 256);
        const auto out = render (*q, x, 256);
        const int delay = q->getLatencySamples();
        double error = 0.0, signal = 0.0;
        for (int i = delay + 4800; i < out.getNumSamples(); ++i)
        {
            const double d = (double) out.getSample (0, i) - (double) x[(size_t) (i - delay)];
            error += d * d;
            signal += (double) x[(size_t) (i - delay)] * x[(size_t) (i - delay)];
        }
        check (10.0 * std::log10 ((error + 1.0e-30) / signal) <= limits::padResetNullDb, "after a double-click on the pad the voice nulls against the delayed input, dB", 10.0 * std::log10 ((error + 1.0e-30) / signal));
    }

    // The null test: the same audio with the window open, ticking and painting is identical to the audio with it closed.
    {
        const auto x = loadWav ("speech_samantha.wav");
        for (int setting = 0; setting < 3; ++setting)
        {
            const auto setUp = [setting] (PluginProcessor& q)
            {
                prepare (q, 2, SR, 256);
                if (setting == 1) { setParam (q, "character", 2.0f); setParam (q, "whisper", 30.0f); }
                if (setting == 2) { setParam (q, "character", 4.0f); setParam (q, "vibratoDepth", 60.0f); }
            };
            auto a = make();
            auto b = make();
            setUp (*a);
            setUp (*b);
            std::unique_ptr<juce::AudioProcessorEditor> open (b->createEditor());
            auto* me = dynamic_cast<MorphEditor*> (open.get());
            const auto first = render (*a, x, 256);
            int block = 0;
            const auto second = render (*b, x, 256, nullptr, [&] (int)
            {
                me->tick();
                if (++block % 16 == 0)
                    paintAt (*open, 2.0f);
            });
            double worst = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < first.getNumSamples(); ++i)
                    worst = juce::jmax (worst, (double) std::abs (first.getSample (ch, i) - second.getSample (ch, i)));
            check (worst == 0.0, std::string ("audio with the window open and painting nulls to exact zero against closed, ") + (setting == 0 ? "Natural" : (setting == 1 ? "Child with Whisper" : "Monster with Vibrato")), worst);
        }
    }
}

void testCpu()
{
    for (double sr : { 48000.0, 96000.0 })
    {
        const auto x = voice::synth (voice::vibrato (180.0, 5.0, sr, 30.0, 5.5), voice::vowelA, sr, -30.0);
        auto p = make();
        setParam (*p, "pitch", -5.0f);
        setParam (*p, "formant", 3.0f);
        setParam (*p, "whisper", 20.0f);
        setParam (*p, "vibratoDepth", 30.0f);
        prepare (*p, 2, sr, 256);
        const auto start = std::clock();
        render (*p, x, 256);
        const double percent = (double) (std::clock() - start) / CLOCKS_PER_SEC / 5.0 * 100.0;
        const double limit = sr > 50000.0 ? limits::cpuPercent96 : limits::cpuPercent;
        check (percent <= limit, "CPU on one core, stereo, everything working, " + std::to_string ((int) sr) + " Hz, machine busy with other builds", percent, "%");
    }
}

// Writes a listening set to build/demo, for ears, outside any host.
void writeDemos()
{
    const auto folder = juce::File (SMOOTH_MORPH_CORPUS).getParentDirectory().getParentDirectory().getChildFile ("build/demo");
    folder.createDirectory();
    juce::WavAudioFormat wav;
    auto temp = make();
    for (const char* name : { "speech_samantha.wav", "speech_daniel.wav", "sung_cellos.wav" })
        for (int program = 0; program < temp->getNumPrograms(); ++program)
        {
            const auto x = loadWav (name);
            auto p = make();
            p->setCurrentProgram (program);
            prepare (*p, 1, SR, 256);
            auto out = render (*p, x, 256);
            const auto target = folder.getChildFile (juce::String (name).upToFirstOccurrenceOf (".", false, false) + " " + p->getProgramName (program) + ".wav");
            target.deleteFile();
            if (auto stream = target.createOutputStream())
                if (std::unique_ptr<juce::AudioFormatWriter> writer { wav.createWriterFor (stream.get(), SR, 1, 24, juce::StringPairArray(), 0) })
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
                }
        }
}
// The link (1.3.0, Switchboard): with SMOOTH_LINK_DIR on a fresh scratch folder in the build, never the real one,
// the card lists every parameter by its ID, and a text command through the inbox moves Pitch and is confirmed in done.
void testLink()
{
    const char* had = std::getenv ("SMOOTH_LINK_DIR");
    const std::string saved = had != nullptr ? had : "";
    const auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile ("link-test");
    dir.deleteRecursively();
    setenv ("SMOOTH_LINK_DIR", dir.getFullPathName().toRawUTF8(), 1);
    {
        auto p = make();
        const auto readCard = [&p]
        {
            std::string bytes;
            juce::var v;
            if (smooth::linkdetail::readFile (p->link.card().getFullPathName().toStdString(), bytes))
                juce::JSON::parse (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), v);
            return v;
        };
        // The readings are added in the constructor's body, after the link is built: its polls list them on the next write, at most a
        // quarter second on in a host. The test moves the link's clock on by that instead of waiting.
        const double start = p->link.clock();
        p->link.clock = [start] { return start + 0.3; };
        p->link.pollNow();
        auto card = readCard();
        const auto& params = p->getParameters();
        auto* listed = card["params"].getArray();
        bool ok = p->link.enabled() && p->link.card().getParentDirectory() == dir && listed != nullptr && listed->size() == params.size();
        juce::StringArray mains;
        for (int i = 0; ok && i < params.size(); ++i)
        {
            auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (params[i]);
            ok = hosted != nullptr && listed->getReference (i)["id"].toString() == hosted->getParameterID();
            if ((bool) listed->getReference (i)["main"])
                mains.add (listed->getReference (i)["id"].toString());
        }
        const juce::StringArray wanted { "pitch", "formant", "character", "characterAmount", "vibratoDepth", "vibratoRate" };
        ok = ok && mains.size() == wanted.size();
        for (const auto& id : wanted)
            ok = ok && mains.contains (id);
        check (ok, "link: the card in a scratch folder lists every parameter by its ID, in order, with the 6 main controls marked", (double) params.size());

        const juce::String name = "1794430230000-test-a1b2c3";
        const auto tmp = p->link.inbox().getChildFile ("." + name + ".tmp");
        tmp.replaceWithText (R"({ "param": "pitch", "text": "5 st" })");
        tmp.moveFileTo (p->link.inbox().getChildFile (name + ".json"));
        const float before = p->apvts.getRawParameterValue ("pitch")->load();
        p->link.pollNow();
        card = readCard();
        juce::var done;
        if (auto* d = card["done"].getArray())
            for (auto& e : *d)
                if (e["cmd"].toString() == name)
                    done = e;
        const float after = p->apvts.getRawParameterValue ("pitch")->load();
        check (std::abs (before - 5.0f) > 0.5f && std::abs (after - 5.0f) < 0.005f && done.isObject() && (bool) done["ok"] && done["param"].toString() == "pitch",
               "link: a text command \"5 st\" on Pitch through the inbox moves it and is confirmed in done", after, "st");
    }
    check (dir.isDirectory() && dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0, "link: closing the plug-in removes its card and inbox");
    if (had != nullptr)
        setenv ("SMOOTH_LINK_DIR", saved.c_str(), 1);
    else
        unsetenv ("SMOOTH_LINK_DIR");
    dir.deleteRecursively();
}
} // namespace

namespace
{
#include "Phase2Tests.h"
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const std::string only = argc > 1 ? argv[1] : "";
    const auto want = [&only] (const char* name) { return only.empty() || only == name; };
    if (only == "demo") { writeDemos(); return 0; }

    if (want ("generic"))
    {
        GenericOptions opt;
        opt.minPresets = 4;
        opt.readOnly = { "detectedPitch", "outputPitch", "rangeOffset" };
        runGenericChecks ([] () -> std::unique_ptr<juce::AudioProcessor> { return make(); }, opt);
    }
    if (want ("latency")) testNeutralAndLatency();
    if (want ("pitch")) testPitch();
    if (want ("pitchformants")) testPitchFormants();
    if (want ("formant")) testFormantShift();
    if (want ("characters")) testCharacters();
    if (want ("vibrato")) testVibrato();
    if (want ("whisper")) testWhisper();
    if (want ("mix")) testMix();
    if (want ("clicks")) testClicks();
    if (want ("consonants")) testConsonants();
    if (want ("real")) testRealMaterial();
    if (want ("guard")) testPeakGuard();
    if (want ("burst")) testSoftNoteBurst();
    if (want ("level")) testInputLevel();
    if (want ("starts")) testPhraseStarts();
    if (want ("range")) testRangeMatch();
    if (want ("presets")) testOutputAndPresets();
    if (want ("blocks")) testBlocksAndStereo();
    if (want ("window")) testWindow();
    if (want ("phase2")) phase2::run();
    if (want ("link")) testLink();
    if (want ("cpu")) testCpu();
    return finish();
}
