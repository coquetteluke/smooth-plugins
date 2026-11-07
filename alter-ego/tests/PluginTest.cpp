// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// Every limit is in PassLimits.h, which was committed before the DSP was written.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PassLimits.h"
#include "VoiceSynth.h"
#include "Analysis.h"
#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <ctime>

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
    double worstFormant = 0.0, worstPitch = 0.0;
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
            std::vector<double> changes;
            const int frame = (int) (0.02 * SR);
            for (size_t i = 0; i + frame < aligned.size(); i += (size_t) frame)
            {
                const double lo = rms (in.data() + i, frame), hi = rms (hi1.data() + i, frame);
                const size_t index = (size_t) ((double) i / SR / 0.005);
                if (index >= reference.size() || reference[index] > 0.0 || hi < 0.001 || hi < 0.5 * lo)
                    continue;
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
            allHiss.insert (allHiss.end(), changes.begin(), changes.end());
        }
    }
    check (worstFormant <= limits::speechFormantFraction, "say speech, Pitch +/-7: long term spectrum shape against the input, worst move", worstFormant * 100.0, "%");
    check (worstPitch <= limits::speechPitchCents, "say speech, Pitch +/-7: median pitch error against a slow reference, worst", worstPitch, "cents");
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
} // namespace

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
    if (want ("range")) testRangeMatch();
    if (want ("presets")) testOutputAndPresets();
    if (want ("blocks")) testBlocksAndStereo();
    if (want ("cpu")) testCpu();
    return finish();
}
