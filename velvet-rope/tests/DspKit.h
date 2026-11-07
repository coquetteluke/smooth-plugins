// Shared by the offline DSP tests: reporting, signal generators, running the engine and measuring the result.
#pragma once

#include "ResonanceEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using smoothres::Params;
using smoothres::ResonanceEngine;
using Signal = std::vector<std::vector<float>>;

// ---- reporting ----
static int failures = 0, passes = 0;

static void section (const char* title) { std::printf ("\n== %s\n", title); }

static void check (bool ok, const char* fmt, ...) __attribute__ ((format (printf, 2, 3)));
static void check (bool ok, const char* fmt, ...)
{
    std::printf ("%s  ", ok ? "PASS" : "FAIL");
    va_list args; va_start (args, fmt); std::vprintf (fmt, args); va_end (args);
    std::printf ("\n");
    (ok ? passes : failures)++;
}

static void info (const char* fmt, ...) __attribute__ ((format (printf, 1, 2)));
static void info (const char* fmt, ...)
{
    std::printf ("INFO  ");
    va_list args; va_start (args, fmt); std::vprintf (fmt, args); va_end (args);
    std::printf ("\n");
}

static double toDb (double power) { return 10.0 * std::log10 (std::max (power, 1.0e-30)); }

// ---- signal generators ----
static constexpr double kRate = 48000.0;

static std::vector<float> whiteNoise (double seconds, double rate, double rmsDb, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> dist (0.0f, (float) std::pow (10.0, rmsDb / 20.0));
    std::vector<float> x ((size_t) (seconds * rate));
    for (auto& v : x) v = dist (rng);
    return x;
}

static std::vector<float> pinkNoise (double seconds, double rate, double rmsDb, unsigned seed)
{
    auto w = whiteNoise (seconds, rate, 0.0, seed);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0;
    for (auto& v : w)
    {
        const double x = v;
        b0 = 0.99886 * b0 + x * 0.0555179; b1 = 0.99332 * b1 + x * 0.0750759; b2 = 0.96900 * b2 + x * 0.1538520;
        b3 = 0.86650 * b3 + x * 0.3104856; b4 = 0.55000 * b4 + x * 0.5329522; b5 = -0.7616 * b5 - x * 0.0168980;
        const double y = b0 + b1 + b2 + b3 + b4 + b5 + b6 + x * 0.5362;
        b6 = x * 0.115926;
        v = (float) y;
        sum += y * y;
    }
    const auto scale = (float) (std::pow (10.0, rmsDb / 20.0) / std::sqrt (sum / (double) w.size()));
    for (auto& v : w) v *= scale;
    return w;
}

static std::vector<float> sine (double seconds, double rate, double hz, double peakDb)
{
    const double amp = std::pow (10.0, peakDb / 20.0);
    std::vector<float> x ((size_t) (seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * hz * (double) i / rate));
    return x;
}

static std::vector<float> add (std::vector<float> a, const std::vector<float>& b)
{
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i) a[i] += b[i];
    return a;
}

/** A standard peaking biquad, to add a resonance to a test signal. */
static std::vector<float> peakFilter (std::vector<float> x, double rate, double hz, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0), w = 2.0 * juce::MathConstants<double>::pi * hz / rate;
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

/** A synthetic sung vowel: harmonics with formants, slow vibrato and a little breath noise. Not a real voice. */
static std::vector<float> syntheticVocal (double seconds, double rate, double f0, unsigned seed)
{
    std::vector<float> x ((size_t) (seconds * rate), 0.0f);
    const double formants[][3] = { { 700, 130, 1.0 }, { 1220, 150, 0.6 }, { 2600, 200, 0.25 }, { 3500, 300, 0.1 } };
    const int harmonics = (int) std::floor (16000.0 / f0);
    std::vector<double> amps ((size_t) harmonics + 1), phases ((size_t) harmonics + 1, 0.0);
    for (int h = 1; h <= harmonics; ++h)
    {
        double a = 0.02;
        for (auto& f : formants)
        {
            const double d = (h * f0 - f[0]) / f[1];
            a += f[2] / (1.0 + d * d);
        }
        amps[(size_t) h] = a / std::pow ((double) h, 0.7);
    }
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;
    double peak = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / rate;
        const double f = f0 * (1.0 + 0.012 * std::sin (twoPi * 5.5 * t));
        double y = 0.0;
        for (int h = 1; h <= harmonics; ++h)
        {
            phases[(size_t) h] += twoPi * h * f / rate;
            y += amps[(size_t) h] * std::sin (phases[(size_t) h]);
        }
        x[i] = (float) y;
        peak = std::max (peak, std::abs (y));
    }
    const auto scale = (float) (0.3 / peak);
    for (auto& v : x) v *= scale;
    return add (x, whiteNoise (seconds, rate, -66.0, seed));
}

// ---- running the engine ----
/** Output with the latency removed, so index i of the output lines up with index i of the input. */
static Signal render (const Signal& in, const Params& p, double rate = kRate, int block = 512)
{
    ResonanceEngine engine;
    engine.prepare (rate, (int) in.size());
    engine.setParams (p);
    const int latency = engine.getLatencySamples();
    const size_t total = in[0].size() + (size_t) latency;
    Signal buf (in.size());
    std::vector<float*> ptrs (in.size());
    for (size_t c = 0; c < in.size(); ++c)
    {
        buf[c] = in[c];
        buf[c].resize (total, 0.0f);
    }
    for (size_t pos = 0; pos < total; pos += (size_t) block)
    {
        for (size_t c = 0; c < in.size(); ++c) ptrs[c] = buf[c].data() + pos;
        engine.process (ptrs.data(), (int) in.size(), (int) std::min ((size_t) block, total - pos));
    }
    for (auto& ch : buf) ch.erase (ch.begin(), ch.begin() + latency);
    return buf;
}

static Signal mono (std::vector<float> x)   { return { std::move (x) }; }
static Signal dual (const std::vector<float>& x) { return { x, x }; }

// ---- measuring ----
/** Averaged power spectrum, skipping the settling time at the start. */
struct Spectrum
{
    std::vector<double> power;
    double binHz = 0.0;

    double band (double lowHz, double highHz) const
    {
        double sum = 0.0;
        const int first = std::max (1, (int) std::ceil (lowHz / binHz));
        const int last = std::min ((int) power.size() - 1, (int) std::floor (highHz / binHz));
        for (int k = first; k <= last; ++k) sum += power[(size_t) k];
        return toDb (sum);
    }
};

static Spectrum spectrumOf (const std::vector<float>& x, double rate = kRate, double skipSeconds = 1.0)
{
    constexpr int order = 13, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) n), buf ((size_t) n * 2);
    for (int i = 0; i < n; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / n);
    Spectrum s;
    s.binHz = rate / n;
    s.power.assign ((size_t) n / 2 + 1, 0.0);
    int frames = 0;
    for (size_t pos = (size_t) (skipSeconds * rate); pos + (size_t) n <= x.size(); pos += (size_t) n / 2)
    {
        for (int i = 0; i < n; ++i) buf[(size_t) i] = x[pos + (size_t) i] * win[(size_t) i];
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k <= n / 2; ++k)
            s.power[(size_t) k] += (double) (buf[(size_t) k * 2] * buf[(size_t) k * 2] + buf[(size_t) k * 2 + 1] * buf[(size_t) k * 2 + 1]);
        ++frames;
    }
    for (auto& v : s.power) v /= std::max (1, frames);
    return s;
}

static double rmsDb (const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0 || to > x.size()) to = x.size();
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) sum += (double) x[i] * (double) x[i];
    return toDb (sum / std::max<size_t> (1, to - from));
}

static double peakDifferenceDb (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0)
{
    double worst = 0.0;
    for (size_t i = from; i < std::min (a.size(), b.size()); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));
    return 20.0 * std::log10 (std::max (worst, 1.0e-15));
}

/** How much a band changed. Negative means it got quieter. */
static double bandChange (const Spectrum& before, const Spectrum& after, double low, double high)
{
    return after.band (low, high) - before.band (low, high);
}

/** Largest change in any third-octave band between two frequencies. */
static double worstThirdOctave (const Spectrum& before, const Spectrum& after, double low, double high)
{
    double worst = 0.0;
    for (double f = low; f * 1.26 <= high; f *= 1.26)
    {
        const double change = bandChange (before, after, f, f * 1.26);
        if (std::abs (change) > std::abs (worst)) worst = change;
    }
    return worst;
}

/** Runs a signal and calls back once per frame with the live engine, for looking at the gain curve. */
static void watchFrames (const Signal& in, const Params& p, const std::function<void (const ResonanceEngine&, double)>& onFrame, double rate = kRate)
{
    ResonanceEngine engine;
    engine.prepare (rate, (int) in.size());
    engine.setParams (p);
    const int hop = engine.getHopSize();
    Signal buf = in;
    std::vector<float*> ptrs (in.size());
    for (size_t pos = 0; pos + (size_t) hop <= in[0].size(); pos += (size_t) hop)
    {
        for (size_t c = 0; c < in.size(); ++c) ptrs[c] = buf[c].data() + pos;
        engine.process (ptrs.data(), (int) in.size(), hop);
        onFrame (engine, (double) (pos + (size_t) hop) / rate);
    }
}


static int finishAndReturn()
{
    std::printf ("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
