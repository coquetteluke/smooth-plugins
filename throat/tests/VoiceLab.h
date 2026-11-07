#pragma once
// Test tools: a synthetic voice (a glottal pulse through formant filters), real speech loading, formant and pitch measuring, and an exact
// tube calculation that shares no code with the engine. Nothing here ships.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace lab
{
constexpr double twoPi = 6.283185307179586;

struct Vowel { std::vector<double> formants; std::vector<double> bandwidths; };

// A real tract keeps resonating above the formants that get names, so these go on up to about 8 kHz.
inline Vowel schwa()   { return { { 500, 1500, 2500, 3500, 4500, 5500, 6500, 7500 }, { 60, 90, 120, 150, 200, 250, 300, 350 } }; }
inline Vowel vowelA()  { return { { 730, 1090, 2440, 3400, 4500, 5500, 6500, 7500 }, { 80, 90, 120, 150, 200, 250, 300, 350 } }; }
inline Vowel vowelI()  { return { { 280, 2250, 3010, 3700, 4500, 5500, 6500, 7500 }, { 50, 100, 120, 150, 200, 250, 300, 350 } }; }
inline Vowel femaleA() { return { { 850, 1220, 2810, 3800, 4950, 6000, 7000, 8000 }, { 90, 100, 130, 170, 220, 260, 300, 350 } }; }

// One cycle of a glottal pulse as radiated from the lips (the Liljencrants and Fant model of the flow's slope), band limited to the
// harmonics that fit below Nyquist at the highest pitch used. Its harmonics fall smoothly at about 6 dB per octave.
// The first version used a Rosenberg pulse, whose hard corners make harmonic levels jump about from one harmonic number to the next. That
// pattern belongs to the formula and not to voices, and it swamped the readings for the first formant.
inline std::vector<float> glottalTable (double sr, double highestHz)
{
    const int size = 8192;
    const double te = 0.6, tp = te * 2.0 / 3.0, ta = 0.05, growth = 3.0 / te;
    const double wg = juce::MathConstants<double>::pi / tp, fallAtEnd = -std::exp (growth * te) * std::sin (wg * te);
    std::vector<double> raw ((size_t) size);
    double mean = 0.0;
    for (int i = 0; i < size; ++i)
    {
        const double t = (double) i / size;
        raw[(size_t) i] = t < te ? std::exp (growth * t) * std::sin (wg * t)
                                 : fallAtEnd * (std::exp (-(t - te) / ta) - std::exp (-(1.0 - te) / ta));
        mean += raw[(size_t) i] / size;
    }
    const int harmonics = std::min ((int) (0.48 * sr / highestHz), size / 2 - 1);
    std::vector<double> built ((size_t) size, 0.0);
    for (int h = 1; h <= harmonics; ++h)
    {
        double re = 0, im = 0;
        for (int i = 0; i < size; ++i)
        {
            re += (raw[(size_t) i] - mean) * std::cos (twoPi * h * i / size);
            im += (raw[(size_t) i] - mean) * std::sin (twoPi * h * i / size);
        }
        for (int i = 0; i < size; ++i)
            built[(size_t) i] += re * std::cos (twoPi * h * i / size) + im * std::sin (twoPi * h * i / size);
    }
    double biggest = 1.0e-9;
    for (auto v : built) biggest = std::max (biggest, std::abs (v));
    std::vector<float> table ((size_t) size);
    for (int i = 0; i < size; ++i) table[(size_t) i] = (float) (built[(size_t) i] / biggest);
    return table;
}

// A glottal source gliding from startHz to endHz, through the formant filters of the vowel. Peak about 0.25.
inline std::vector<float> voice (const Vowel& v, double sr, double seconds, double startHz, double endHz, double level = 0.25)
{
    const auto table = glottalTable (sr, std::max (startHz, endHz));
    const int n = (int) (seconds * sr);
    std::vector<double> y ((size_t) n);
    double phase = 0.0;
    const int size = (int) table.size();
    for (int i = 0; i < n; ++i)
    {
        const double hz = startHz + (endHz - startHz) * i / n;
        const double at = phase * size;
        const int a = (int) at;
        const double t = at - a;
        y[(size_t) i] = table[(size_t) (a % size)] + t * (table[(size_t) ((a + 1) % size)] - table[(size_t) (a % size)]);
        phase += hz / sr;
        if (phase >= 1.0) phase -= 1.0;
    }
    for (size_t f = 0; f < v.formants.size(); ++f)
    {
        const double r = std::exp (-juce::MathConstants<double>::pi * v.bandwidths[f] / sr);
        const double c = 2.0 * r * std::cos (twoPi * v.formants[f] / sr), d = -r * r, g = 1.0 - c - d;
        double y1 = 0, y2 = 0;
        for (auto& s : y)
        {
            const double o = g * s + c * y1 + d * y2;
            y2 = y1; y1 = o; s = o;
        }
    }
    double biggest = 1.0e-9;
    for (int i = n / 10; i < n; ++i) biggest = std::max (biggest, std::abs (y[(size_t) i]));
    std::vector<float> out ((size_t) n);
    for (int i = 0; i < n; ++i) out[(size_t) i] = (float) (y[(size_t) i] * level / biggest);
    return out;
}

inline std::vector<float> loadWav (const juce::File& f)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (f.createInputStream().release(), true));
    if (! reader) return {};
    std::vector<float> out ((size_t) reader->lengthInSamples);
    float* dest[] = { out.data() };
    reader->read (dest, 1, 0, (int) reader->lengthInSamples);
    return out;
}

// Average power spectrum over a stretch. A voice whose pitch glides sweeps its harmonics over the formants, so the average shows the
// resonances themselves.
struct Spectrum
{
    double binHz = 1.0;
    std::vector<double> db;
    double at (double hz) const
    {
        const int k = juce::jlimit (0, (int) db.size() - 2, (int) (hz / binHz));
        const double t = hz / binHz - k;
        return db[(size_t) k] + t * (db[(size_t) k + 1] - db[(size_t) k]);
    }
};

inline Spectrum averageSpectrum (const float* x, int n, double sr, double smoothHz = 40.0, int order = 13)
{
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) size * 2);
    std::vector<double> sum ((size_t) size / 2 + 1, 0.0);
    int frames = 0;
    for (int start = 0; start + size <= n; start += size / 4, ++frames)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            work[(size_t) i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft.performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= size / 2; ++k)
            sum[(size_t) k] += (double) work[(size_t) (2 * k)] * work[(size_t) (2 * k)] + (double) work[(size_t) (2 * k + 1)] * work[(size_t) (2 * k + 1)];
    }
    Spectrum s;
    s.binHz = sr / size;
    s.db.resize (sum.size());
    const int reach = std::max (1, (int) (smoothHz / s.binHz));
    for (int k = 0; k <= size / 2; ++k)
    {
        double total = 0, weightSum = 0;
        for (int j = std::max (0, k - reach); j <= std::min (size / 2, k + reach); ++j)
        {
            const double w = 1.0 - std::abs (j - k) / (reach + 1.0);
            total += w * sum[(size_t) j];
            weightSum += w;
        }
        s.db[(size_t) k] = 10.0 * std::log10 (total / weightSum / std::max (frames, 1) + 1.0e-20);
    }
    return s;
}

// The loudest each frequency ever gets over a stretch. A voice whose pitch glides sweeps its harmonics across every frequency, so this
// traces the resonances themselves. Unlike an average it does not care how long a harmonic lingered anywhere.
inline Spectrum holdSpectrum (const float* x, int n, double sr, double smoothHz = 30.0, int order = 12)
{
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) size * 2);
    std::vector<double> held ((size_t) size / 2 + 1, 1.0e-20);
    for (int start = 0; start + size <= n; start += size / 4)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            work[(size_t) i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft.performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= size / 2; ++k)
            held[(size_t) k] = std::max (held[(size_t) k], (double) work[(size_t) (2 * k)] * work[(size_t) (2 * k)] + (double) work[(size_t) (2 * k + 1)] * work[(size_t) (2 * k + 1)]);
    }
    Spectrum s;
    s.binHz = sr / size;
    s.db.resize (held.size());
    const int reach = std::max (1, (int) (smoothHz / s.binHz));
    for (int k = 0; k <= size / 2; ++k)
    {
        double total = 0, weightSum = 0;
        for (int j = std::max (0, k - reach); j <= std::min (size / 2, k + reach); ++j)
        {
            const double w = 1.0 - std::abs (j - k) / (reach + 1.0);
            total += w * 10.0 * std::log10 (held[(size_t) j]);
            weightSum += w;
        }
        s.db[(size_t) k] = total / weightSum;
    }
    return s;
}

// Level in dB of each harmonic of the glottal pulse the test voices use.
inline std::vector<double> sourceHarmonicsDb (double sr, double highestHz)
{
    const auto table = glottalTable (sr, highestHz);
    const int size = (int) table.size(), count = (int) (0.48 * sr / highestHz);
    std::vector<double> out ((size_t) count + 1, -200.0);
    for (int h = 1; h <= count; ++h)
    {
        double re = 0, im = 0;
        for (int i = 0; i < size; ++i)
        {
            re += table[(size_t) i] * std::cos (twoPi * h * i / size);
            im += table[(size_t) i] * std::sin (twoPi * h * i / size);
        }
        out[(size_t) h] = 10.0 * std::log10 (re * re + im * im + 1.0e-20);
    }
    return out;
}

// Reads the tract's response from a voice whose glide is known: at each moment the level of every harmonic, with the glottal pulse's own
// level for that harmonic taken back off. What is left, gathered over the glide, is the resonances alone, with no pulse shape to mistake
// for a formant and no dependence on how long a harmonic lingered anywhere.
inline Spectrum tractResponse (const float* x, int n, double sr, double startHz, double endHz, double seconds, double smoothHz = 20.0)
{
    const auto source = sourceHarmonicsDb (sr, std::max (startHz, endHz));
    const int order = sr > 60000.0 ? 13 : 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) size * 2);
    const double slotHz = 5.0;
    const int slots = (int) (0.5 * sr / slotHz) + 2;
    std::vector<double> sum ((size_t) slots, 0.0), count ((size_t) slots, 0.0);
    const double binHz = sr / size;
    for (int start = (int) (0.2 * sr); start + size <= n; start += size / 4)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i) work[(size_t) i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft.performRealOnlyForwardTransform (work.data());
        const double t = (start + 0.5 * size) / sr;
        const double f0 = startHz + (endHz - startHz) * t / seconds;
        for (int h = 1; h < (int) source.size() && h * f0 < 0.48 * sr; ++h)
        {
            const double hz = h * f0;
            const int centre = (int) std::round (hz / binHz);
            double best = 1.0e-20;
            for (int k = std::max (0, centre - 2); k <= std::min (size / 2, centre + 2); ++k)
                best = std::max (best, (double) work[(size_t) (2 * k)] * work[(size_t) (2 * k)] + (double) work[(size_t) (2 * k + 1)] * work[(size_t) (2 * k + 1)]);
            const int slot = (int) (hz / slotHz);
            sum[(size_t) slot] += 10.0 * std::log10 (best) - source[(size_t) h];
            count[(size_t) slot] += 1.0;
        }
    }
    Spectrum s;
    s.binHz = slotHz;
    s.db.resize ((size_t) slots);
    const int reach = std::max (1, (int) (smoothHz / slotHz));
    for (int k = 0; k < slots; ++k)
    {
        double total = 0, weightSum = 0;
        for (int j = std::max (0, k - reach); j <= std::min (slots - 1, k + reach); ++j)
            if (count[(size_t) j] > 0.0)
            {
                const double w = (1.0 - std::abs (j - k) / (reach + 1.0)) * count[(size_t) j];
                total += w * sum[(size_t) j] / count[(size_t) j];
                weightSum += w;
            }
        s.db[(size_t) k] = weightSum > 0.0 ? total / weightSum : -200.0;
    }
    return s;
}

// Highest peak between two frequencies, refined with a parabola.
inline double peakBetween (const Spectrum& s, double lowHz, double highHz)
{
    const int a = std::max (1, (int) (lowHz / s.binHz)), b = std::min ((int) s.db.size() - 2, (int) (highHz / s.binHz));
    int best = -1;
    double bestDb = -1.0e9;
    for (int k = a; k <= b; ++k)
        if (s.db[(size_t) k] >= s.db[(size_t) k - 1] && s.db[(size_t) k] >= s.db[(size_t) k + 1] && s.db[(size_t) k] > bestDb)
        {
            best = k;
            bestDb = s.db[(size_t) k];
        }
    if (best < 0) return 0.0;
    // The middle of the top 2 dB. Each slot here comes from whichever harmonics passed it, so the very top is rough, and on a broad peak the
    // highest point can sit anywhere along it.
    int lo = best, hi = best;
    while (lo > a && s.db[(size_t) lo - 1] >= bestDb - 2.0) --lo;
    while (hi < b && s.db[(size_t) hi + 1] >= bestDb - 2.0) ++hi;
    return 0.5 * (lo + hi) * s.binHz;
}

// Frequency of the strongest partial, from how its phase advances between frames. Far finer than a bin.
inline double partialHz (const float* x, int n, double sr, double nearHz, double reachHz)
{
    const int size = 1 << 14, hop = 1 << 12;
    juce::dsp::FFT fft (14);
    std::vector<float> a ((size_t) size * 2), b ((size_t) size * 2);
    const auto spectrum = [&] (std::vector<float>& w, int start)
    {
        std::fill (w.begin(), w.end(), 0.0f);
        for (int i = 0; i < size; ++i) w[(size_t) i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft.performRealOnlyForwardTransform (w.data());
    };
    const double binHz = sr / size;
    double total = 0.0;
    int count = 0;
    for (int start = 0; start + size + hop <= n; start += size)
    {
        spectrum (a, start);
        spectrum (b, start + hop);
        int best = (int) ((nearHz - reachHz) / binHz);
        double bestPower = -1.0;
        for (int k = best; k <= (int) ((nearHz + reachHz) / binHz); ++k)
        {
            const double p = (double) a[(size_t) (2 * k)] * a[(size_t) (2 * k)] + (double) a[(size_t) (2 * k + 1)] * a[(size_t) (2 * k + 1)];
            if (p > bestPower) { bestPower = p; best = k; }
        }
        const double pa = std::atan2 ((double) a[(size_t) (2 * best + 1)], (double) a[(size_t) (2 * best)]);
        const double pb = std::atan2 ((double) b[(size_t) (2 * best + 1)], (double) b[(size_t) (2 * best)]);
        const double expected = twoPi * best * hop / size;
        double diff = pb - pa - expected;
        diff -= twoPi * std::round (diff / twoPi);
        total += (best + diff * size / (twoPi * hop)) * binHz;
        ++count;
    }
    return count > 0 ? total / count : 0.0;
}

// Harmonics to noise ratio of a voice with a constant pitch, between two frequencies: power at the harmonics over power between them.
inline double harmonicsToNoiseDb (const float* x, int n, double sr, double f0, double lowHz, double highHz)
{
    const int order = 17, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) size * 2);
    std::vector<double> sum ((size_t) size / 2 + 1, 0.0);
    int frames = 0;
    for (int start = 0; start + size <= n; start += size / 2, ++frames)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i) work[(size_t) i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft.performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= size / 2; ++k)
            sum[(size_t) k] += (double) work[(size_t) (2 * k)] * work[(size_t) (2 * k)] + (double) work[(size_t) (2 * k + 1)] * work[(size_t) (2 * k + 1)];
    }
    const double binHz = sr / size;
    double harmonic = 1.0e-30, between = 1.0e-30;
    for (int k = (int) (lowHz / binHz); k <= (int) (highHz / binHz); ++k)
    {
        const double hz = k * binHz;
        const double off = std::abs (hz - f0 * std::round (hz / f0));
        if (off <= 3.0 * binHz) harmonic += sum[(size_t) k];
        else between += sum[(size_t) k];
    }
    // Noise under the harmonics counts too: scale up for the bins the harmonic slots took.
    const double slotShare = std::min (0.9, (6.0 + 1.0) * binHz / f0);
    const double noise = between / (1.0 - slotShare);
    const double pure = std::max (harmonic - noise * slotShare, 1.0e-30);
    return 10.0 * std::log10 (pure / noise);
}

// ---- A lossless tube, solved exactly: one chain matrix per section, closed at the cords, open at the lips.
// Resonances are where the pressure at the open end is zero for a closed start. Shares nothing with the engine.
inline std::vector<double> tubeResonances (const std::vector<double>& area, double lengthCm, int wanted, double soundCmPerSec = 34000.0)
{
    const double step = lengthCm / (double) area.size();
    const auto pressureAtLips = [&] (double hz)
    {
        const double k = twoPi * hz / soundCmPerSec, c = std::cos (k * step), s = std::sin (k * step);
        // With U = 0 at the start: state is pressure p (real) and volume velocity u (imaginary part), which stay in quadrature.
        double p = 1.0, u = 0.0;
        for (double a : area)
        {
            const double z = 1.0 / a;
            const double pNext = c * p - z * s * u;
            const double uNext = (s / z) * p + c * u;
            p = pNext; u = uNext;
        }
        return p;
    };
    std::vector<double> found;
    double previous = pressureAtLips (20.0);
    for (double hz = 25.0; hz < 8000.0 && (int) found.size() < wanted; hz += 5.0)
    {
        const double now = pressureAtLips (hz);
        if (now * previous < 0.0)
        {
            double lo = hz - 5.0, hi = hz, flo = previous;
            for (int i = 0; i < 40; ++i)
            {
                const double mid = 0.5 * (lo + hi), fm = pressureAtLips (mid);
                if (fm * flo <= 0.0) hi = mid;
                else { lo = mid; flo = fm; }
            }
            found.push_back (0.5 * (lo + hi));
        }
        previous = now;
    }
    return found;
}

// Area along a tube from five points (place 0 to 1, width), the same straight lines in log width that the controls describe.
inline std::vector<double> pointsToArea (const std::array<double, 5>& place, const std::array<double, 5>& width, const std::vector<double>& base, int sections = 128)
{
    std::vector<double> area ((size_t) sections);
    for (int i = 0; i < sections; ++i)
    {
        const double x = (i + 0.5) / sections;
        int a = 0;
        while (a < 3 && x > place[(size_t) a + 1]) ++a;
        const double t = juce::jlimit (0.0, 1.0, (x - place[(size_t) a]) / std::max (place[(size_t) a + 1] - place[(size_t) a], 1.0e-6));
        const double lw = std::log (width[(size_t) a]) + t * (std::log (width[(size_t) a + 1]) - std::log (width[(size_t) a]));
        const double b = base.empty() ? 1.0 : base[(size_t) (x * (double) base.size())];
        area[(size_t) i] = b * std::exp (2.0 * lw);
    }
    return area;
}

// Short-time level in dB, frame by frame. Used to catch warble and pumping.
inline std::vector<double> levelTrack (const float* x, int n, int frame)
{
    std::vector<double> out;
    for (int s = 0; s + frame <= n; s += frame)
    {
        double e = 1.0e-14;
        for (int i = 0; i < frame; ++i) e += (double) x[s + i] * x[s + i];
        out.push_back (10.0 * std::log10 (e / frame));
    }
    return out;
}

// Simple second order band filter, in place, for looking at one band of a signal.
inline void bandPass (std::vector<float>& x, double sr, double lowHz, double highHz)
{
    const auto section = [&] (double hz, bool high)
    {
        const double w = twoPi * hz / sr, alpha = std::sin (w) / (2.0 * 0.7071), cs = std::cos (w);
        const double b0 = high ? (1 + cs) / 2 : (1 - cs) / 2, b1 = high ? -(1 + cs) : 1 - cs, b2 = b0;
        const double a0 = 1 + alpha, a1 = -2 * cs, a2 = 1 - alpha;
        double z1 = 0, z2 = 0;
        for (auto& s : x)
        {
            const double in = s, o = (b0 / a0) * in + z1;
            z1 = (b1 / a0) * in - (a1 / a0) * o + z2;
            z2 = (b2 / a0) * in - (a2 / a0) * o;
            s = (float) o;
        }
    };
    section (lowHz, true); section (lowHz, true);
    section (highHz, false); section (highHz, false);
}

// How far one partial's frequency differs between two versions of a signal, frame by frame, from its phase advance. The partial is the
// strongest near the given pitch. Works on gliding pitch because both versions are read at the same moment in the same bin.
inline double partialMoveCents (const float* a, const float* b, int start, double sr, double nearHz)
{
    const int order = 11, size = 1 << order, hop = size / 8;
    static thread_local std::unique_ptr<juce::dsp::FFT> fft;
    static thread_local std::vector<float> w0, w1, w2, w3;
    if (fft == nullptr) { fft = std::make_unique<juce::dsp::FFT> (order); w0.resize ((size_t) size * 2); w1 = w0; w2 = w0; w3 = w0; }
    const auto take = [&] (const float* x, int from, std::vector<float>& w)
    {
        std::fill (w.begin(), w.end(), 0.0f);
        for (int i = 0; i < size; ++i) w[(size_t) i] = x[from + i] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft->performRealOnlyForwardTransform (w.data());
    };
    take (a, start, w0); take (a, start + hop, w1); take (b, start, w2); take (b, start + hop, w3);
    const double binHz = sr / size;
    int best = std::max (1, (int) ((nearHz * 0.75) / binHz));
    double bestPower = -1.0;
    for (int k = best; k <= (int) (nearHz * 1.25 / binHz); ++k)
    {
        const double p = (double) w0[(size_t) (2 * k)] * w0[(size_t) (2 * k)] + (double) w0[(size_t) (2 * k + 1)] * w0[(size_t) (2 * k + 1)];
        if (p > bestPower) { bestPower = p; best = k; }
    }
    const auto advance = [&] (const std::vector<float>& u, const std::vector<float>& v)
    {
        const double pu = std::atan2 ((double) u[(size_t) (2 * best + 1)], (double) u[(size_t) (2 * best)]);
        const double pv = std::atan2 ((double) v[(size_t) (2 * best + 1)], (double) v[(size_t) (2 * best)]);
        const double expected = twoPi * best * hop / size;
        double d = pv - pu - expected;
        d -= twoPi * std::round (d / twoPi);
        return (best + d * size / (twoPi * hop)) * binHz;
    };
    return 1200.0 * std::log2 (advance (w2, w3) / advance (w0, w1));
}

// Short-time pitch by autocorrelation with parabolic refinement: 0 when the frame is not clearly voiced.
inline std::vector<double> pitchTrack (const float* x, int n, double sr)
{
    const int frame = (int) (0.04 * sr), step = (int) (0.01 * sr);
    const int low = (int) (sr / 400.0), high = (int) (sr / 70.0);
    std::vector<double> out;
    for (int s = 0; s + frame + high < n; s += step)
    {
        double e0 = 1.0e-12;
        for (int i = 0; i < frame; ++i) e0 += (double) x[s + i] * x[s + i];
        double best = 0.0;
        int bestLag = -1;
        std::vector<double> r ((size_t) high + 2, 0.0);
        for (int lag = low - 1; lag <= high + 1; ++lag)
        {
            double c = 0.0, e1 = 1.0e-12;
            for (int i = 0; i < frame; ++i) { c += (double) x[s + i] * x[s + i + lag]; e1 += (double) x[s + i + lag] * x[s + i + lag]; }
            r[(size_t) lag] = c / std::sqrt (e0 * e1);
        }
        for (int lag = low; lag <= high; ++lag)
            if (r[(size_t) lag] > best && r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1])
            {
                best = r[(size_t) lag];
                bestLag = lag;
            }
        if (bestLag < 0 || best < 0.8 || e0 / frame < 1.0e-5)
        {
            out.push_back (0.0);
            continue;
        }
        const double y0 = r[(size_t) bestLag - 1], y1 = r[(size_t) bestLag], y2 = r[(size_t) bestLag + 1];
        const double bend = y0 - 2.0 * y1 + y2;
        out.push_back (sr / (bestLag + (std::abs (bend) > 1.0e-9 ? 0.5 * (y0 - y2) / bend : 0.0)));
    }
    return out;
}

// Finds the ratio a long-term spectrum moved by, by sliding it along a log frequency axis.
inline double spectrumShiftRatio (const Spectrum& input, const Spectrum& output, double lowHz, double highHz)
{
    const int stepsPerOctave = 96;
    const int points = (int) (std::log2 (highHz / lowHz) * stepsPerOctave);
    const auto hzOf = [&] (int i) { return lowHz * std::pow (2.0, (double) i / stepsPerOctave); };
    double bestScore = -2.0, bestRatio = 1.0;
    for (int shift = -stepsPerOctave; shift <= stepsPerOctave; ++shift)
    {
        const double ratio = std::pow (2.0, (double) shift / stepsPerOctave);
        double ma = 0, mb = 0;
        for (int i = 0; i < points; ++i) { ma += input.at (hzOf (i)); mb += output.at (hzOf (i) * ratio); }
        ma /= points; mb /= points;
        double ab = 0, aa = 0, bb = 0;
        for (int i = 0; i < points; ++i)
        {
            const double a = input.at (hzOf (i)) - ma, b = output.at (hzOf (i) * ratio) - mb;
            ab += a * b; aa += a * a; bb += b * b;
        }
        const double score = ab / std::sqrt (aa * bb + 1.0e-12);
        if (score > bestScore) { bestScore = score; bestRatio = ratio; }
    }
    return bestRatio;
}
} // namespace lab
