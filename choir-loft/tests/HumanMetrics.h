#pragma once
// How human it measures. Built against 1.0.0 and against the current code, so "before" numbers are real ones from the old source, at its own defaults.
// Include after TestKit.h, VoiceSynth.h, Analysis.h and PluginProcessor.h. Uses only parameters both versions have, except through setIf.
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <numeric>

namespace human
{
using namespace testkit;
using namespace measure;
using namespace smoothharmony;

constexpr double SR = 48000.0;

struct Numbers
{
    std::map<std::string, double> values;
    void put (const std::string& name, double v, const char* unit = "")
    {
        values[name] = v;
        std::printf ("METRIC  %s = %.3f %s\n", name.c_str(), v, unit);
        std::fflush (stdout);
    }
};

inline std::unique_ptr<PluginProcessor> make()
{
    PluginProcessor::Options o;
    o.keyFilePath = "/nonexistent/smooth-harmony-test.json";
    o.hostLatencyUpdates = false;
    return std::make_unique<PluginProcessor> (o);
}

inline void setIf (PluginProcessor& p, const juce::String& id, float value)
{
    if (auto* prm = p.apvts.getParameter (id))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

inline juce::String vid (int voice, const char* what) { return "voice" + juce::String (voice + 1) + what; }

inline std::vector<float> mono (const juce::AudioBuffer<float>& b, int ch = 0)
{
    return std::vector<float> (b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples());
}

inline double noteHz (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }

// The version's own defaults, one voice alone, lead off, in the middle, four voices live.
inline std::unique_ptr<PluginProcessor> defaultsSolo (int voice, std::function<void (PluginProcessor&)> tweak = nullptr)
{
    auto p = make();
    if (tweak) tweak (*p);
    setIf (*p, "voices", 4.0f);
    setIf (*p, "leadLevel", -60.0f);
    for (int v = 0; v < 4; ++v)
    {
        setIf (*p, vid (v, "Level"), v == voice ? 0.0f : -60.0f);
        setIf (*p, vid (v, "Pan"), 0.0f);
    }
    prepare (*p, 1, SR, 256);
    return p;
}

// What a tune-only measurement wants: every singer control at zero. Tone, Breath and Scatter only exist from 1.1.0.
inline void plainShifter (PluginProcessor& p)
{
    setIf (p, "humanize", 0.0f);
    setIf (p, "vibrato", 0.0f);
    setIf (p, "glide", 0.0f);
    setIf (p, "followLead", 0.0f);
    setIf (p, "breath", 0.0f);
    setIf (p, "consonantScatter", 0.0f);
    for (int v = 0; v < 4; ++v)
    {
        setIf (p, vid (v, "Drift"), 0.0f);
        setIf (p, vid (v, "Throat"), 100.0f);
        setIf (p, vid (v, "Tone"), 0.0f);
    }
}

// ------------------------------------------------------------------------------------------------- pitch: scoops, lateness, off target

inline void pitchHumanity (Numbers& out, const std::string& tag, std::function<void (PluginProcessor&)> tweak = nullptr)
{
    const std::vector<int> notes { 57, 59, 60, 62, 64, 62, 60, 57, 60, 64, 62, 59 };
    const double noteSec = 0.6;
    std::vector<double> hz;
    for (int n : notes) hz.push_back (noteHz (n));
    const auto x = voice::synth (voice::phrase (hz, noteSec, SR, 0.0, 0.0), voice::vowelA, SR);
    const auto mask = scaleMask (0, scaleMajor);

    std::array<std::vector<double>, 4> arrival;        // ms after the lead, per voice, per note change
    std::vector<double> offTarget, spreadPerNote;
    std::array<std::vector<double>, 4> bodyByVoice;
    std::vector<double> earlyCents;

    std::array<std::array<double, 16>, 4> arrivalAt {};
    int lat = 0;
    for (int v = 0; v < 4; ++v)
    {
        auto p = defaultsSolo (v, [&] (PluginProcessor& q) { setIf (q, "mode", (float) modeScale); setIf (q, "key", 0.0f); setIf (q, "scale", (float) scaleMajor); if (tweak) tweak (q); });
        const int interval = juce::roundToInt (p->apvts.getRawParameterValue (vid (v, "Interval"))->load());
        const auto o = mono (render (*p, x, 256));
        lat = p->getLatencySamples();
        const double latSec = (double) lat / SR;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const int want = harmonyNote (notes[i], interval, mask);
            const double start = (double) i * noteSec + latSec;
            for (double t = start + 0.25; t < start + noteSec - 0.05; t += 0.01)
            {
                const double f = pitchNear (o.data(), (long) o.size(), (long) (t * SR), noteHz (want), SR, 0.05, 3.0, 0.02);
                if (f > 0.0)
                {
                    const double c = cents (f, noteHz (want));
                    offTarget.push_back (std::abs (c));
                    bodyByVoice[(size_t) v].push_back (c);
                }
            }
            if (i == 0) continue;
            const int was = harmonyNote (notes[i - 1], interval, mask);
            if (want == was) { arrivalAt[(size_t) v][i] = std::nan (""); continue; }
            const double middleHz = noteHz (0.5 * (want + was));
            double found = std::nan ("");
            for (double t = start - 0.03; t < start + 0.25; t += 0.001)
            {
                const double f = pitchNear (o.data(), (long) o.size(), (long) (t * SR), middleHz, SR, 0.2, 3.0);
                if (f > 0.0 && (want > was ? f >= middleHz : f <= middleHz)) { found = t; break; }
            }
            const double ms = 1000.0 * (found - start);
            arrivalAt[(size_t) v][i] = ms;
            if (std::isfinite (ms)) arrival[(size_t) v].push_back (ms);
        }
    }
    for (size_t i = 1; i < notes.size(); ++i)
    {
        double low = 1.0e9, high = -1.0e9;
        int count = 0;
        for (int v = 0; v < 4; ++v)
            if (std::isfinite (arrivalAt[(size_t) v][i])) { low = std::min (low, arrivalAt[(size_t) v][i]); high = std::max (high, arrivalAt[(size_t) v][i]); ++count; }
        if (count >= 2) spreadPerNote.push_back (high - low);
    }
    double meanLag = 0.0; int count = 0;
    for (const auto& a : arrival) for (double d : a) { meanLag += d; ++count; }
    out.put (tag + "note change, how late voices arrive after the lead, average", meanLag / std::max (count, 1), "ms");
    out.put (tag + "note change, spread between earliest and latest voice, median", median (spreadPerNote), "ms");
    out.put (tag + "body of a note, distance from target, median", median (offTarget), "cents");
    out.put (tag + "body of a note, distance from target, 95th percentile", percentile (offTarget, 95.0), "cents");
    std::vector<double> means;
    for (const auto& b : bodyByVoice) if (! b.empty()) means.push_back (median (b));
    std::sort (means.begin(), means.end());
    out.put (tag + "voices settle apart: highest voice average minus lowest", means.back() - means.front(), "cents");
}

// Lead with vibrato: do the voices' pitch moves go together?
inline void lockstep (Numbers& out, const std::string& tag, std::function<void (PluginProcessor&)> tweak = nullptr)
{
    const double base = noteHz (60);
    const auto x = voice::synth (voice::vibrato (base, 8.0, SR, 35.0, 5.3), voice::vowelA, SR);
    std::array<std::vector<double>, 4> traces;
    const std::array<int, 4> intervalsFromC { 4, 7, -5, 12 };
    for (int v = 0; v < 4; ++v)
    {
        auto p = defaultsSolo (v, [&] (PluginProcessor& q) { setIf (q, "mode", (float) modeScale); setIf (q, "key", 0.0f); setIf (q, "scale", (float) scaleMajor); if (tweak) tweak (q); });
        const int interval = juce::roundToInt (p->apvts.getRawParameterValue (vid (v, "Interval"))->load());
        const auto o = mono (render (*p, x, 256));
        const double want = noteHz (harmonyNote (60, interval, scaleMask (0, scaleMajor)));
        for (double t = 0.5; t < 7.8; t += 0.01)
        {
            const double f = pitchNear (o.data(), (long) o.size(), (long) (t * SR), want, SR, 0.06, 3.0);
            traces[(size_t) v].push_back (f > 0.0 ? cents (f, want) : 0.0);
        }
    }
    double worst = -1.0;
    for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 4; ++b)
        {
            double ab = 0, aa = 0, bb = 0, ma = 0, mb = 0;
            const size_t n = traces[(size_t) a].size();
            for (size_t i = 0; i < n; ++i) { ma += traces[(size_t) a][i]; mb += traces[(size_t) b][i]; }
            ma /= (double) n; mb /= (double) n;
            for (size_t i = 0; i < n; ++i)
            {
                const double x1 = traces[(size_t) a][i] - ma, x2 = traces[(size_t) b][i] - mb;
                ab += x1 * x2; aa += x1 * x1; bb += x2 * x2;
            }
            worst = std::max (worst, ab / std::sqrt (aa * bb + 1.0e-12));
        }
    out.put (tag + "lead with vibrato: voices' pitch moves, correlation of the most alike pair", worst);
}

// ---------------------------------------------------------------------------------------------------------- throat and tone

inline std::vector<double> octaveGlide (double centreHz, double seconds)
{
    std::vector<double> f0 ((size_t) (seconds * SR));
    for (size_t i = 0; i < f0.size(); ++i)
    {
        const double u = std::fmod ((double) i / SR / 3.0, 1.0);
        const double tri = u < 0.5 ? 4.0 * u - 1.0 : 3.0 - 4.0 * u;
        f0[i] = centreHz * std::pow (2.0, 0.5 * tri);
    }
    return f0;
}

inline void voiceIdentity (Numbers& out, const std::string& tag)
{
    const auto x = voice::synth (octaveGlide (165.0, 6.0), voice::vowelA, SR);
    const long from = (long) (0.3 * SR), n = (long) x.size() - from - 1000;
    const auto inShape = smoothSpectrumDb (x.data() + from, n, SR, 180.0);
    std::array<double, 4> scale {}, tilt {};
    for (int v = 0; v < 4; ++v)
    {
        auto p = defaultsSolo (v, [] (PluginProcessor& q) { setIf (q, "mode", (float) modeFixed); });
        setIf (*p, vid (v, "Interval"), 0.0f);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, x, 256));
        const auto shape = smoothSpectrumDb (o.data() + from, n, SR, 180.0);
        scale[(size_t) v] = shapeScale (inShape, shape, 500.0, 3800.0);
        const auto bandMean = [] (const std::vector<double>& s, int a, int b) { double m = 0; for (int i = a; i < b; ++i) m += s[(size_t) i]; return m / (b - a); };
        tilt[(size_t) v] = (bandMean (shape, 3000, 6000) - bandMean (shape, 400, 1200)) - (bandMean (inShape, 3000, 6000) - bandMean (inShape, 400, 1200));
    }
    double lowS = 9.0, highS = 0.0, lowT = 99.0, highT = -99.0;
    for (int v = 0; v < 4; ++v)
    {
        out.put (tag + "voice " + std::to_string (v + 1) + " formant scale against the lead", scale[(size_t) v], "times");
        out.put (tag + "voice " + std::to_string (v + 1) + " brightness against the lead", tilt[(size_t) v], "dB");
        lowS = std::min (lowS, scale[(size_t) v]); highS = std::max (highS, scale[(size_t) v]);
        lowT = std::min (lowT, tilt[(size_t) v]); highT = std::max (highT, tilt[(size_t) v]);
    }
    out.put (tag + "formants: widest gap between two voices", 100.0 * (highS / lowS - 1.0), "%");
    out.put (tag + "brightness: widest gap between two voices", highT - lowT, "dB");
}

// ------------------------------------------------------------------------------------------------------------------ consonants

inline std::vector<float> highPassed (const std::vector<float>& v, double hz)
{
    std::vector<float> b (v.size(), 0.0f);
    double lp = 0.0;
    const double k = 1.0 - std::exp (-2.0 * M_PI * hz / SR);
    for (size_t i = 0; i < v.size(); ++i) { lp += k * ((double) v[i] - lp); b[i] = (float) ((double) v[i] - lp); }
    return b;
}

// Three poles, so strong low harmonics do not swamp what is being read up there.
inline std::vector<float> steepHigh (const std::vector<float>& v, double hz)
{
    return highPassed (highPassed (highPassed (v, hz), hz), hz);
}

inline std::vector<float> fricativeLead (double hz, double vowelSec, double sSec, unsigned seed = 5)
{
    auto a = voice::synth (voice::steady (hz, vowelSec, SR), voice::vowelA, SR);
    std::vector<float> x = a;
    const int sLen = (int) (sSec * SR);
    std::vector<float> s ((size_t) sLen);
    noise (s.data(), sLen, 1.0, seed);
    double lp = 0.0;
    const double k = 1.0 - std::exp (-2.0 * M_PI * 3500.0 / SR);
    for (int i = 0; i < sLen; ++i)
    {
        lp += k * ((double) s[(size_t) i] - lp);
        const double fade = std::min ({ 1.0, i / (0.005 * SR), (sLen - i) / (0.005 * SR) });
        s[(size_t) i] = (float) (0.2 * ((double) s[(size_t) i] - lp) * fade);
    }
    x.insert (x.end(), s.begin(), s.end());
    x.insert (x.end(), a.begin(), a.end());
    return x;
}

struct Corr { double zero = 0.0, best = 0.0; };

inline Corr correlateFft (const float* a, const float* b, int n, int reach)
{
    int order = 1;
    while ((1 << order) < 2 * n) ++order;
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    using C = juce::dsp::Complex<float>;
    std::vector<C> ca ((size_t) size), cb ((size_t) size), fa ((size_t) size), fb ((size_t) size), product ((size_t) size), r ((size_t) size);
    double ea = 0, eb = 0;
    for (int i = 0; i < n; ++i)
    {
        ca[(size_t) i] = C (a[i], 0.0f);
        cb[(size_t) i] = C (b[i], 0.0f);
        ea += (double) a[i] * a[i];
        eb += (double) b[i] * b[i];
    }
    fft.perform (ca.data(), fa.data(), false);
    fft.perform (cb.data(), fb.data(), false);
    for (int i = 0; i < size; ++i) product[(size_t) i] = fa[(size_t) i] * std::conj (fb[(size_t) i]);
    fft.perform (product.data(), r.data(), true);
    const double norm = std::sqrt (ea * eb) + 1.0e-20;
    Corr c;
    c.zero = (double) r[0].real() / norm;
    double best = -2.0;
    for (int lag = 0; lag <= reach; ++lag)
    {
        best = std::max (best, (double) r[(size_t) lag].real() / norm);
        best = std::max (best, (double) r[(size_t) ((size - lag) & (size - 1))].real() / norm);
    }
    c.best = best;
    return c;
}

// Where a burst of hiss starts: the first 1 ms frame past halfway, in dB, from the level just before it to its plateau. NaN if the burst does not stand out,
// since a voice's S at low level can sit near the high end of its own vowel.
inline double hissOnset (const std::vector<float>& high, long beforeFrom, long searchFrom, long plateauFrom, long plateauTo)
{
    const int w = (int) (0.001 * SR);
    const double before = rms (high.data() + beforeFrom, (int) (0.1 * SR));
    const double plateau = rms (high.data() + plateauFrom, (int) (plateauTo - plateauFrom));
    if (plateau < 2.0 * before) return std::nan ("");
    const double middle = std::sqrt (before * plateau);
    for (long i = searchFrom; i + w < plateauTo; i += w)
    {
        // Three frames in a row, because noise goes up and down.
        if (rms (high.data() + i, w) >= middle && rms (high.data() + i + w, w) >= middle && rms (high.data() + i + 2 * w, w) >= middle)
            return (double) i / SR;
    }
    return std::nan ("");
}

inline void consonants (Numbers& out, const std::string& tag, std::function<void (PluginProcessor&)> tweak = nullptr)
{
    // Several S sounds, because anything random per consonant needs more than one go.
    std::array<std::vector<double>, 4> onsets;
    std::vector<double> levels, spreads, bestCorr, soft;
    for (unsigned seed : { 5u, 6u, 7u, 8u, 9u, 10u })
    {
        const double vowelSec = 0.4 + 0.03 * seed, sSec = 0.2;
        const auto x = fricativeLead (220.0, vowelSec, sSec, seed);
        std::array<std::vector<float>, 4> highs;
        double lat = 0.0;
        const auto leadHigh = steepHigh (x, 4500.0);
        const double leadLevel = rms (leadHigh.data() + (long) ((vowelSec + 0.04) * SR), (int) (0.12 * SR));
        const double leadAt = hissOnset (leadHigh, (long) ((vowelSec - 0.15) * SR), (long) ((vowelSec - 0.03) * SR), (long) ((vowelSec + 0.04) * SR), (long) ((vowelSec + 0.16) * SR));
        std::array<double, 4> at {};
        for (int v = 0; v < 4; ++v)
        {
            auto p = defaultsSolo (v, [&] (PluginProcessor& q) { setIf (q, "mode", (float) modeFixed); if (tweak) tweak (q); });
            setIf (*p, vid (v, "Interval"), 7.0f);
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, x, 256));
            lat = (double) p->getLatencySamples() / SR;
            highs[(size_t) v] = steepHigh (o, 4500.0);
            // The plateau is judged later for a voice, since it may start well after the lead. The whole S is 200 ms.
            at[(size_t) v] = hissOnset (highs[(size_t) v], (long) ((vowelSec - 0.15 + lat) * SR), (long) ((vowelSec - 0.03 + lat) * SR), (long) ((vowelSec + 0.09 + lat) * SR), (long) ((vowelSec + 0.19 + lat) * SR)) - lat - leadAt;
            onsets[(size_t) v].push_back (at[(size_t) v] * 1000.0);
            levels.push_back (toDb (rms (highs[(size_t) v].data() + (long) ((vowelSec + lat + 0.05) * SR), (int) (0.12 * SR))) - toDb (leadLevel));
        }
        double low = 1.0e9, high = -1.0e9;
        int found = 0;
        for (double d : at) if (std::isfinite (d)) { low = std::min (low, d); high = std::max (high, d); ++found; }
        if (found >= 2) spreads.push_back (1000.0 * (high - low));
        for (int a = 0; a < 4; ++a)
            for (int b = a + 1; b < 4; ++b)
            {
                const long from = (long) ((vowelSec + lat + 0.05) * SR);
                bestCorr.push_back (correlateFft (highs[(size_t) a].data() + from, highs[(size_t) b].data() + from, (int) (0.12 * SR), (int) (0.08 * SR)).best);
            }
    }
    for (int v = 0; v < 4; ++v)
    {
        std::vector<double> finite;
        for (double d : onsets[(size_t) v]) if (std::isfinite (d)) finite.push_back (d);
        out.put (tag + "S sound: voice " + std::to_string (v + 1) + " starts after the lead's, median", finite.empty() ? -1.0 : median (finite), "ms");
    }
    out.put (tag + "S sound: spread of when it starts across voices, median", median (spreads), "ms");
    out.put (tag + "S sound: how much two voices' S look like one another, best lag, median", median (bestCorr));
    out.put (tag + "S sound: voice level against the lead, above 4.5 kHz, median", median (levels), "dB");

    // Together with the lead, at the version's own defaults, on S sounds and on say speech: does the S pile up?
    for (int count : { 2, 4 })
    {
        const auto wav = [&]
        {
            juce::AudioFormatManager m;
            m.registerBasicFormats();
            juce::File f (juce::String (SMOOTH_HARMONY_CORPUS) + "/speech_samantha.wav");
            std::unique_ptr<juce::AudioFormatReader> r (m.createReaderFor (f));
            juce::AudioBuffer<float> b;
            if (r != nullptr) { b.setSize (1, (int) r->lengthInSamples); r->read (&b, 0, (int) r->lengthInSamples, 0, true, false); }
            return b;
        }();
        const std::vector<float> in (wav.getReadPointer (0), wav.getReadPointer (0) + wav.getNumSamples());
        auto p = make();
        setIf (*p, "voices", (float) count);
        if (tweak) tweak (*p);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, in, 256));
        const int lat = p->getLatencySamples();
        std::vector<float> lead (in.size(), 0.0f);
        for (size_t i = (size_t) lat; i < in.size(); ++i) lead[i] = in[i - (size_t) lat];
        const auto leadHigh = highPassed (lead, 4000.0), allHigh = highPassed (o, 4000.0);
        std::vector<double> rises;
        const int w = (int) (0.03 * SR);
        for (long at = w; at + 2 * w < (long) in.size(); at += w)
        {
            const double all = rms (lead.data() + at, w), high = rms (leadHigh.data() + at, w);
            if (all > 0.005 && high > 0.6 * all)
                rises.push_back (toDb (rms (allHigh.data() + at, w)) - toDb (high));
        }
        out.put (tag + "say speech, S and T sounds, lead with " + std::to_string (count) + " voices against the lead alone, median", rises.empty() ? 0.0 : median (rises), "dB");
    }
}

// ------------------------------------------------------------------------------------------------------------- metallic sound

// A lead with a real voice's period to period unevenness, which is what a shifter that repeats periods turns into a pattern.
inline std::vector<float> unevenLead (double hz, double seconds, unsigned seed = 21)
{
    std::mt19937 gen (seed);
    std::normal_distribution<double> gauss (0.0, 1.0);
    std::vector<double> f0 ((size_t) (seconds * SR));
    double wander = 0.0, fast = 0.0;
    for (size_t i = 0; i < f0.size(); ++i)
    {
        wander += 0.0008 * (gauss (gen) - wander);
        fast += 0.03 * (gauss (gen) - fast);
        f0[i] = hz * (1.0 + 0.02 * wander + 0.01 * fast);
    }
    auto x = voice::synth (f0, voice::vowelA, SR, -32.0, -18.0, seed);
    double amp = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        amp += 0.002 * (gauss (gen) - amp);
        x[i] = (float) (x[i] * (1.0 + 1.5 * amp));
    }
    return x;
}

// Energy between the harmonics, against the harmonics, which is where a repeating grain pattern lands. Only sensible on a nearly steady pitch.
// (First written for a third of the output pitch only, which is right at +7 and wrong at -7, where the pattern repeats every two grains.)
inline double thirdSidebandDb (const std::vector<float>& y, long from, double fOut)
{
    constexpr int order = 16, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2, 0.0f);
    for (int i = 0; i < size; ++i)
        buf[(size_t) i] = y[(size_t) (from + i)] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    const double binHz = SR / size;
    double side = 0.0, harm = 0.0;
    for (int k = (int) (80.0 / binHz); k < (int) (6000.0 / binHz); ++k)
    {
        const double phase = (k * binHz) / fOut;
        const double fromWhole = std::abs (phase - std::round (phase));
        const double power = (double) buf[(size_t) k] * buf[(size_t) k];
        if (fromWhole < 0.08) harm += power;
        else if (fromWhole >= 0.12) side += power;
    }
    return 10.0 * std::log10 (side / std::max (harm, 1.0e-30));
}

// How exactly each period above 3 kHz repeats the one before: 1 is a machine.
inline double highBandRepeat (const std::vector<float>& y, long from, double fOut, int seconds = 1)
{
    const auto high = steepHigh (y, 3000.0);
    const double period = SR / fOut;
    const int reach = (int) (period * 0.04) + 2, lag0 = (int) std::lround (period);
    std::vector<double> values;
    const int w = (int) (4.0 * period);
    for (long at = from; at + w + lag0 + reach < from + (long) (seconds * SR) && at + w + lag0 + reach < (long) y.size(); at += (long) (0.02 * SR))
    {
        double best = -1.0, ea = 0.0;
        for (int i = 0; i < w; ++i) ea += (double) high[(size_t) (at + i)] * high[(size_t) (at + i)];
        for (int lag = lag0 - reach; lag <= lag0 + reach; ++lag)
        {
            double c = 0.0, eb = 0.0;
            for (int i = 0; i < w; ++i) { c += (double) high[(size_t) (at + i)] * high[(size_t) (at + i + lag)]; eb += (double) high[(size_t) (at + i + lag)] * high[(size_t) (at + i + lag)]; }
            best = std::max (best, c / std::sqrt (ea * eb + 1.0e-20));
        }
        values.push_back (best);
    }
    return median (values);
}

struct FlatScore { double worst = 0.0; std::vector<double> all; };

inline void flatness (FlatScore& s, double& worstMove, const voice::Vowel& v, double centre, int shift, std::function<void (PluginProcessor&)> tweak)
{
    const auto in = voice::synth (octaveGlide (centre, 6.0), v, SR);
    const auto control = voice::synth (octaveGlide (centre * std::pow (2.0, shift / 12.0), 6.0), v, SR);
    auto p = defaultsSolo (0, [&] (PluginProcessor& q) { setIf (q, "mode", (float) modeFixed); setIf (q, "inputType", 2.0f); plainShifter (q); if (tweak) tweak (q); });
    setIf (*p, vid (0, "Interval"), (float) shift);
    prepare (*p, 1, SR, 256);
    const auto o = mono (render (*p, in, 256));
    const long from = (long) (0.3 * SR), n = (long) in.size() - from - 1000;
    const double smoothing = std::max (centre, centre * std::pow (2.0, shift / 12.0));
    const auto inShape = smoothSpectrumDb (in.data() + from, n, SR, smoothing), gotShape = smoothSpectrumDb (o.data() + from, n, SR, smoothing);
    for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
        worstMove = std::max (worstMove, std::abs (shapeScale (inShape, gotShape, band.first, band.second) - 1.0));
    const auto fine = smoothSpectrumDb (control.data() + from, n, SR, 50.0), fineOut = smoothSpectrumDb (o.data() + from, n, SR, 50.0);
    const size_t f1 = (size_t) v.f[0];
    const double flat = (fine[f1] - fine[f1 / 2]) - (fineOut[f1] - fineOut[f1 / 2]);
    s.worst = std::max (s.worst, flat);
    s.all.push_back (flat);
}

inline void metallic (Numbers& out, const std::string& tag, std::function<void (PluginProcessor&)> tweak = nullptr)
{
    const voice::Vowel* vowels[] = { &voice::vowelA, &voice::vowelE, &voice::vowelO, &voice::vowelAe, &voice::vowelI, &voice::vowelU };
    for (int shift : { 7, -7 })
    {
        FlatScore all, open;
        double worstMove = 0.0;
        for (auto* v : vowels)
            for (double centre : { 110.0, 165.0, 220.0 })
            {
                flatness (all, worstMove, *v, centre, shift, tweak);
                if (v != &voice::vowelI && v != &voice::vowelU) open.all.push_back (all.all.back());
            }
        out.put (tag + "first formant flatter than a real voice, " + std::to_string (shift) + " st, worst of all vowels", all.worst, "dB");
        out.put (tag + "first formant flatter than a real voice, " + std::to_string (shift) + " st, median of all vowels", median (all.all), "dB");
        out.put (tag + "first formant flatter than a real voice, " + std::to_string (shift) + " st, median of open vowels", median (open.all), "dB");
        out.put (tag + "formant position move, " + std::to_string (shift) + " st, worst of these vowels", 100.0 * worstMove, "%");
    }

    // Grain pattern, on a lead with a real voice's unevenness
    const double hz = 220.0;
    const auto x = unevenLead (hz, 3.0);
    for (int shift : { 7, -7, 12 })
    {
        const double fOut = hz * std::pow (2.0, shift / 12.0);
        auto p = defaultsSolo (0, [&] (PluginProcessor& q) { setIf (q, "mode", (float) modeFixed); plainShifter (q); if (tweak) tweak (q); });
        setIf (*p, vid (0, "Interval"), (float) shift);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, x, 256));
        out.put (tag + "grain pattern at " + std::to_string (shift) + " st: sideband energy against the harmonics", thirdSidebandDb (o, 24000, fOut), "dB");
        out.put (tag + "period to period sameness above 3 kHz at " + std::to_string (shift) + " st, 1 is a machine", highBandRepeat (o, 24000, fOut));
    }
    out.put (tag + "for comparison, the same sameness in the lead itself", highBandRepeat (x, 24000, hz));
    out.put (tag + "for comparison, energy between the harmonics in the lead itself", thirdSidebandDb (x, 24000, hz), "dB");

    // At the version's own defaults: how machine like above 3 kHz, on the real thing a listener hears
    {
        auto p = defaultsSolo (0, [] (PluginProcessor& q) { setIf (q, "mode", (float) modeFixed); });
        setIf (*p, vid (0, "Interval"), 7.0f);
        prepare (*p, 1, SR, 256);
        const auto o = mono (render (*p, x, 256));
        out.put (tag + "period to period sameness above 3 kHz at 7 st, this version's defaults", highBandRepeat (o, 24000, hz * std::pow (2.0, 7.0 / 12.0)));
    }
}

// ------------------------------------------------------------------------------------------------------- copies of one another

inline void copies (Numbers& out, const std::string& tag)
{
    const auto steady = voice::synth (voice::steady (220.0, 8.0, SR), voice::vowelA, SR);
    const auto one = [&] (int v)
    {
        auto p = defaultsSolo (v, [] (PluginProcessor& q) { setIf (q, "mode", (float) modeFixed); });
        for (int i = 0; i < 4; ++i) setIf (*p, vid (i, "Interval"), 7.0f);
        prepare (*p, 1, SR, 256);
        return mono (render (*p, steady, 256));
    };
    const auto a = one (0), b = one (1);
    std::vector<double> bests;
    for (long at = 24000; at + 48000 < (long) a.size(); at += 48000)
        bests.push_back (correlateFft (a.data() + at, b.data() + at, 48000, 2400).best);
    out.put (tag + "two voices on one interval, steady lead: correlation at the best lag", median (bests));
}

// -------------------------------------------------------------------------------------------------------------- listening material

// Sung style synthetic phrase with consonants and breaths, so note changes can be heard: vowels on a melody in C major, with scoops,
// vibrato and unevenness, S and T like noise between syllables, and a breath in the gap.
inline std::vector<float> melody()
{
    struct Syl { int note; double seconds; const voice::Vowel* vowel; int before; }; // before: 0 none, 1 S, 2 T, 3 breath
    const std::vector<Syl> line {
        { 60, 0.55, &voice::vowelA, 0 }, { 62, 0.45, &voice::vowelE, 2 }, { 64, 0.9, &voice::vowelA, 1 }, { 64, 0.45, &voice::vowelO, 2 },
        { 62, 0.45, &voice::vowelE, 1 }, { 60, 1.2, &voice::vowelA, 2 },
        { 57, 0.55, &voice::vowelO, 3 }, { 60, 0.45, &voice::vowelA, 1 }, { 62, 0.9, &voice::vowelE, 2 }, { 64, 0.45, &voice::vowelAe, 1 },
        { 65, 0.45, &voice::vowelA, 2 }, { 64, 1.5, &voice::vowelO, 1 },
    };
    std::vector<float> out;
    std::mt19937 gen (41);
    std::normal_distribution<double> gauss (0.0, 1.0);
    unsigned seed = 50;
    const auto addNoise = [&] (double seconds, double hp, double level)
    {
        const int n = (int) (seconds * SR);
        std::vector<float> s ((size_t) n);
        noise (s.data(), n, 1.0, seed++);
        double lp = 0.0;
        const double k = 1.0 - std::exp (-2.0 * M_PI * hp / SR);
        for (int i = 0; i < n; ++i)
        {
            lp += k * ((double) s[(size_t) i] - lp);
            const double fade = std::min ({ 1.0, i / (0.006 * SR), (n - i) / (0.012 * SR) });
            s[(size_t) i] = (float) (level * ((double) s[(size_t) i] - lp) * fade);
        }
        out.insert (out.end(), s.begin(), s.end());
    };
    for (const auto& syl : line)
    {
        if (syl.before == 1) addNoise (0.14, 3500.0, 0.09);
        if (syl.before == 2) addNoise (0.025, 1800.0, 0.12);
        if (syl.before == 3) addNoise (0.45, 800.0, 0.012);
        const size_t len = (size_t) (syl.seconds * SR);
        std::vector<double> f0 (len);
        double wander = 0.0;
        for (size_t i = 0; i < len; ++i)
        {
            const double t = (double) i / SR;
            wander += 0.001 * (gauss (gen) - wander);
            const double scoop = t < 0.07 ? -60.0 * (1.0 - t / 0.07) * (1.0 - t / 0.07) : 0.0;
            const double vib = t > 0.25 ? std::min (1.0, (t - 0.25) / 0.2) * 22.0 * std::sin (2.0 * M_PI * 5.4 * t) : 0.0;
            f0[i] = noteHz (syl.note) * std::pow (2.0, (scoop + vib + 12.0 * wander) / 1200.0);
        }
        auto sung = voice::synth (f0, *syl.vowel, SR, -30.0, -16.0, seed++);
        for (size_t i = 0; i < len; ++i)
        {
            const double t = (double) i / SR;
            sung[i] *= (float) std::min ({ 1.0, t / 0.02, (syl.seconds - t) / 0.04 });
        }
        out.insert (out.end(), sung.begin(), sung.end());
    }
    return out;
}

inline std::vector<float> loadCorpus (const char* name, double maxSeconds)
{
    juce::AudioFormatManager m;
    m.registerBasicFormats();
    juce::File f (juce::String (SMOOTH_HARMONY_CORPUS) + "/" + name + ".wav");
    std::unique_ptr<juce::AudioFormatReader> r (m.createReaderFor (f));
    juce::AudioBuffer<float> b;
    if (r != nullptr)
    {
        const int n = std::min ((int) r->lengthInSamples, (int) (maxSeconds * SR));
        b.setSize (1, n);
        r->read (&b, 0, n, 0, true, false);
    }
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

inline void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();
    juce::WavAudioFormat w;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> writer (w.createWriterFor (os.get(), SR, (unsigned) b.getNumChannels(), 16, {}, 0));
    if (writer != nullptr)
    {
        os.release();
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
}

// The same material and presets for every version. Program 0 is two voices, program 3 is four. Both start from the version's own defaults.
inline void renderDemos (const juce::File& dir, const juce::String& suffix, int extraProgram = -1, const juce::String& extraName = {})
{
    struct Mat { const char* name; std::vector<float> samples; };
    std::vector<Mat> materials;
    materials.push_back ({ "melody", melody() });
    materials.push_back ({ "sung", loadCorpus ("sung_goodnews", 9.0) });
    materials.push_back ({ "speech", loadCorpus ("speech_samantha", 6.0) });
    for (const auto& m : materials)
    {
        const auto one = [&] (int program, const juce::String& label)
        {
            auto p = make();
            p->setCurrentProgram (program);
            setIf (*p, "inputType", 3.0f); // Wide Range, because these voices are not all in one range
            prepare (*p, 2, SR, 256);
            auto b = render (*p, m.samples, 256);
            // Pull down only if the voices would clip the 16 bit file. Same factor everywhere, so before and after compare fairly.
            b.applyGain (0.6f);
            writeWav (dir.getChildFile (juce::String (m.name) + "_" + label + "_" + suffix + ".wav"), b);
        };
        one (0, "two_voices");
        one (3, "four_voices");
        if (extraProgram >= 0) one (extraProgram, extraName);
    }
    // The lead alone, for reference, once
    for (const auto& m : materials)
    {
        const auto f = dir.getChildFile (juce::String (m.name) + "_lead_alone.wav");
        if (! f.existsAsFile())
        {
            juce::AudioBuffer<float> b (1, (int) m.samples.size());
            b.copyFrom (0, 0, m.samples.data(), (int) m.samples.size());
            b.applyGain (0.6f);
            writeWav (f, b);
        }
    }
}

inline void onlyMetallicBlend (Numbers& n)
{
    metallic (n, "with period blending: ", [] (PluginProcessor& p) { setIf (p, "humanize", 0.1f); });
}

inline void runAll (Numbers& n, const std::string& tag = "")
{
    pitchHumanity (n, tag);
    lockstep (n, tag);
    voiceIdentity (n, tag);
    consonants (n, tag);
    metallic (n, tag);
    copies (n, tag);
}
} // namespace human
