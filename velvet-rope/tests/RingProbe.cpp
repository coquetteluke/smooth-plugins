// Not a test: a look inside. Shows what the engine sees and does at one spot of a recording, and how deeply a list of rings is cut.
//
//   RingProbe frames <wav> <from sec> <to sec> <hz> [name=value ...]
//       one line per hop: the harmonic spacing it reads, the loudest bin of the whole spectrum, and at the tallest bin within a
//       24th of an octave of hz its level, the harmonics either side, how far it stands over them, for how long something
//       there has stood out (ms), and the reduction asked for by the old judgement and the reduction applied.
//   RingProbe spacing <wav> <from sec> <to sec> [name=value ...]
//       one line per look at the harmonic spacing: what the engine holds, and the correlations the look found.
//   RingProbe rings <events.txt> <folder of wavs> [name=value ...]
//       events.txt: lines of "<take number> <start sec> <length sec> <hz>". For each, the mean of dry dB minus output dB at the
//       ring's tallest bin from 107 ms after it starts (Hann 2048, hop 512, the way the real vocal check reads a lasting resonance).
//
// name=value: held=0|1 (default 1), depth, sel, sharp, low, high, quality.
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#define private public
#include "ResonanceEngine.h"
#undef private

using smoothres::Params;
using smoothres::ResonanceEngine;

static std::vector<float> readWav (const std::string& path)
{
    std::vector<float> x;
    FILE* f = std::fopen (path.c_str(), "rb");
    if (f == nullptr) { std::fprintf (stderr, "cannot open %s\n", path.c_str()); return x; }
    unsigned char head[12];
    if (std::fread (head, 1, 12, f) != 12) { std::fclose (f); return x; }
    int channels = 1, bits = 24, format = 1;
    for (;;)
    {
        unsigned char ck[8];
        if (std::fread (ck, 1, 8, f) != 8) break;
        const unsigned size = (unsigned) ck[4] | ((unsigned) ck[5] << 8) | ((unsigned) ck[6] << 16) | ((unsigned) ck[7] << 24);
        if (std::memcmp (ck, "fmt ", 4) == 0)
        {
            std::vector<unsigned char> b (size);
            if (std::fread (b.data(), 1, size, f) != size) break;
            format = b[0] | (b[1] << 8); channels = b[2] | (b[3] << 8); bits = b[14] | (b[15] << 8);
            if (size & 1) std::fgetc (f);
        }
        else if (std::memcmp (ck, "data", 4) == 0)
        {
            std::vector<unsigned char> b (size);
            const size_t got = std::fread (b.data(), 1, size, f);
            const int bytes = bits / 8;
            const size_t frames = got / (size_t) (bytes * channels);
            x.resize (frames);
            for (size_t i = 0; i < frames; ++i)
            {
                const unsigned char* p = b.data() + i * (size_t) (bytes * channels);
                if (format == 3 && bits == 32) { float v; std::memcpy (&v, p, 4); x[i] = v; }
                else if (bits == 24) x[i] = (float) ((int) ((unsigned) p[0] << 8 | (unsigned) p[1] << 16 | (unsigned) p[2] << 24) >> 8) / 8388608.0f;
                else if (bits == 16) x[i] = (float) (short) (p[0] | (p[1] << 8)) / 32768.0f;
            }
            break;
        }
        else std::fseek (f, (long) (size + (size & 1)), SEEK_CUR);
    }
    std::fclose (f);
    return x;
}

static Params paramsFrom (int argc, char** argv, int first)
{
    Params p;
    p.heldNotes = true;
    for (int i = first; i < argc; ++i)
    {
        const char* eq = std::strchr (argv[i], '=');
        if (eq == nullptr) continue;
        const std::string name (argv[i], (size_t) (eq - argv[i]));
        const float v = (float) std::atof (eq + 1);
        if (name == "held") p.heldNotes = v > 0.5f;
        else if (name == "depth") p.depthDb = v;
        else if (name == "sel") p.selectivity = v;
        else if (name == "sharp") p.sharpness = v;
        else if (name == "low") p.lowHz = v;
        else if (name == "high") p.highHz = v;
        else if (name == "quality") p.quality = (smoothres::Quality) (int) v;
        else if (name == "hard") p.hard = v > 0.5f;
    }
    return p;
}

static double db (double power) { return 10.0 * std::log10 (std::max (power, 1.0e-30)); }

static int frames (int argc, char** argv)
{
    if (argc < 6) return 2;
    const auto x = readWav (argv[2]);
    const double from = std::atof (argv[3]), to = std::atof (argv[4]), hz = std::atof (argv[5]);
    const Params p = paramsFrom (argc, argv, 6);
    ResonanceEngine engine;
    engine.prepare (48000.0, 1);
    engine.setParams (p);
    const int hop = engine.getHopSize(), fftSize = engine.getFftSize();
    const double binHz = engine.getBinHz();
    std::vector<float> block ((size_t) hop);
    // Start two seconds early, so that every long view has settled by the time the printing starts.
    const size_t startAt = (size_t) std::max (0.0, (from - 2.0) * 48000.0) / (size_t) hop * (size_t) hop;
    std::printf ("# centre_s  f0est  bin_hz loudest level  h-2   h-1   h+1   h+2  stands  count_ms  raw  applied\n");
    for (size_t pos = startAt; pos + (size_t) hop <= x.size(); pos += (size_t) hop)
    {
        std::copy (x.begin() + (long) pos, x.begin() + (long) (pos + (size_t) hop), block.begin());
        float* ptr = block.data();
        engine.process (&ptr, 1, hop);
        const double centre = ((double) (pos + (size_t) hop) - 0.5 * fftSize) / 48000.0;
        if (centre < from) continue;
        if (centre > to) break;
        const auto& c = engine.cores[(size_t) engine.active];
        const auto& s = c.slots[0];
        const int k0 = (int) std::floor (hz / std::pow (2.0, 1.0 / 24.0) / binHz), k1 = (int) std::ceil (hz * std::pow (2.0, 1.0 / 24.0) / binHz);
        int k = k0;
        for (int q = k0; q <= k1; ++q) if (s.smoothedPower[(size_t) q] > s.smoothedPower[(size_t) k]) k = q;
        const int one = juce::roundToInt (c.spacingBins), two = juce::roundToInt (2.0f * c.spacingBins);
        const auto hp = [&] (int q) { return q >= 0 && q < c.numBins && c.spacingBins > 0 ? db (c.harmonicPeak[(size_t) q]) : -999.0; };
        float raw = 0, applied = 0;
        for (int q = k0; q <= k1; ++q) { raw = std::max (raw, s.raw[(size_t) q]); applied = std::max (applied, s.applied[(size_t) q]); }
        std::printf ("%8.3f %6.1f %7.1f %6.1f %6.1f %6.1f %5.1f %5.1f %5.1f %6.1f %5.1f %5.2f %5.2f\n", centre, c.spacingBins * binHz, k * binHz, db (s.loudest), db (s.smoothedPower[(size_t) k]),
                     hp (k - two), hp (k - one), hp (k + one), hp (k + two), (double) c.standing[(size_t) k], (double) s.slowStanding[(size_t) k], (double) raw, (double) applied);
    }
    return 0;
}

/** One line per look at the harmonic spacing (every fourth hop in Normal): the spacing the engine holds, for how many more looks
    it will hold it, and what the flattened spectrum it looked at says by itself: the strongest correlation at any spacing, and the
    correlation at the spacing being held. Shows why a spacing is dropped on a note that is still sounding. */
static int spacing (int argc, char** argv)
{
    if (argc < 5) return 2;
    const auto x = readWav (argv[2]);
    const double from = std::atof (argv[3]), to = std::atof (argv[4]);
    const Params p = paramsFrom (argc, argv, 5);
    ResonanceEngine engine;
    engine.prepare (48000.0, 1);
    engine.setParams (p);
    const int hop = engine.getHopSize(), fftSize = engine.getFftSize();
    const double binHz = engine.getBinHz();
    std::vector<float> block ((size_t) hop);
    const size_t startAt = (size_t) std::max (0.0, (from - 2.0) * 48000.0) / (size_t) hop * (size_t) hop;
    std::printf ("# centre_s  spacing_hz  looks_left  best_corr  best_hz  corr_at_held  mean_log\n");
    float lastSpacing = 0.0f;
    for (size_t pos = startAt; pos + (size_t) hop <= x.size(); pos += (size_t) hop)
    {
        std::copy (x.begin() + (long) pos, x.begin() + (long) (pos + (size_t) hop), block.begin());
        float* ptr = block.data();
        engine.process (&ptr, 1, hop);
        const double centre = ((double) (pos + (size_t) hop) - 0.5 * fftSize) / 48000.0;
        const auto& c = engine.cores[(size_t) engine.active];
        if (c.spacingCounter != 0) continue;   // this hop did not look
        const float held = c.spacingBins > 0.0f ? c.spacingBins : lastSpacing;
        if (c.spacingBins > 0.0f) lastSpacing = c.spacingBins;
        if (centre < from) continue;
        if (centre > to) break;
        const int count = c.spacingLastBin - c.spacingFirstBin + 1;
        const float* flat = c.spacingFlat.data();
        const auto corr = [flat, count] (int lag)
        {
            float cross = 0.0f, a = 0.0f, b = 0.0f;
            for (int i = 0; i + lag < count; ++i) { cross += flat[i] * flat[i + lag]; a += flat[i] * flat[i]; b += flat[i + lag] * flat[i + lag]; }
            return cross / std::sqrt (std::max (a * b, 1.0e-12f));
        };
        float best = 0.0f; int bestLag = 0;
        for (int lag = c.spacingMinLag; lag <= c.spacingMaxLag; ++lag) { const float v = corr (lag); if (v > best) { best = v; bestLag = lag; } }
        float atHeld = 0.0f;
        if (held > 0.0f) for (int lag = juce::roundToInt (held) - 1; lag <= juce::roundToInt (held) + 1; ++lag) atHeld = std::max (atHeld, corr (lag));
        double sum = 0.0;
        for (int i = 0; i < count; ++i) sum += c.spacingLog[(size_t) i];
        std::printf ("%8.3f %7.1f %4d %6.2f %7.1f %6.2f %7.1f\n", centre, c.spacingBins * binHz, c.spacingHeld, (double) best, bestLag * binHz, (double) atHeld, sum / count);
    }
    return 0;
}

namespace
{
constexpr int N = 2048, H = 512;

struct Stft
{
    juce::dsp::FFT fft { 11 };
    std::vector<float> win, buf;
    Stft() : win (N), buf (2 * N) { for (int i = 0; i < N; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / N)); }
    void power (const std::vector<float>& x, long from, std::vector<double>& p)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i) { const long j = from + i; if (j >= 0 && j < (long) x.size()) buf[(size_t) i] = x[(size_t) j] * win[(size_t) i]; }
        fft.performRealOnlyForwardTransform (buf.data(), true);
        p.resize (N / 2 + 1);
        for (int k = 0; k <= N / 2; ++k) p[(size_t) k] = (double) buf[(size_t) (2 * k)] * buf[(size_t) (2 * k)] + (double) buf[(size_t) (2 * k + 1)] * buf[(size_t) (2 * k + 1)] + 1.0e-20;
    }
};
}

static int rings (int argc, char** argv)
{
    if (argc < 4) return 2;
    struct Event { int take; double start, length, hz; };
    std::vector<Event> events;
    if (FILE* f = std::fopen (argv[2], "r"))
    {
        char line[512];
        while (std::fgets (line, sizeof (line), f))
        {
            Event e {};
            if (std::sscanf (line, "%d %lf %lf %lf", &e.take, &e.start, &e.length, &e.hz) == 4) events.push_back (e);
        }
        std::fclose (f);
    }
    const Params p = paramsFrom (argc, argv, 4);
    std::vector<std::string> wavs;
    if (DIR* d = opendir (argv[3]))
    {
        while (auto* entry = readdir (d)) { const std::string n = entry->d_name; if (n.size() > 4 && n.substr (n.size() - 4) == ".wav") wavs.push_back (n); }
        closedir (d);
    }
    std::sort (wavs.begin(), wavs.end());
    int total = 0, cut3 = 0;
    std::vector<double> all;
    Stft stft;
    for (const auto& name : wavs)
    {
        const int take = std::atoi (name.c_str());
        bool wanted = false;
        for (const auto& e : events) wanted = wanted || e.take == take;
        if (! wanted) continue;
        const auto x = readWav (std::string (argv[3]) + "/" + name);
        ResonanceEngine engine;
        engine.prepare (48000.0, 1);
        engine.setParams (p);
        const int latency = engine.getLatencySamples();
        std::vector<float> y = x;
        y.resize (x.size() + (size_t) latency, 0.0f);
        for (size_t pos = 0; pos < y.size(); pos += 512) { float* ptr = y.data() + pos; engine.process (&ptr, 1, (int) std::min ((size_t) 512, y.size() - pos)); }
        y.erase (y.begin(), y.begin() + latency);
        double ex = 0, ey = 0;
        for (size_t i = 0; i < x.size(); ++i) { ex += (double) x[i] * x[i]; ey += (double) y[i] * y[i]; }
        std::vector<double> pd, po;
        for (const auto& e : events)
        {
            if (e.take != take) continue;
            const long first = std::lround ((e.start * 48000.0 - N / 2) / H), span = std::lround (e.length * 48000.0 / H);
            const int k0 = (int) std::floor (e.hz / std::pow (2.0, 1.0 / 24.0) / (48000.0 / N)), k1 = (int) std::ceil (e.hz * std::pow (2.0, 1.0 / 24.0) / (48000.0 / N));
            double sum = 0; int n = 0;
            for (long j = first + 10; j < first + span; ++j)
            {
                stft.power (x, j * H, pd);
                stft.power (y, j * H, po);
                int k = k0;
                for (int q = k0; q <= k1; ++q) if (pd[(size_t) q] > pd[(size_t) k]) k = q;
                sum += db (pd[(size_t) k]) - db (po[(size_t) k]); ++n;
            }
            const double cut = n > 0 ? sum / n : 0.0;
            std::printf ("%02d %8.3f %5.2f %7.1f  cut %5.2f\n", take, e.start, e.length, e.hz, cut);
            ++total; cut3 += cut >= 3.0; all.push_back (cut);
        }
        std::printf ("# take %02d level %+.3f dB\n", take, db (ey / std::max (ex, 1e-30)));
        std::fflush (stdout);
    }
    std::sort (all.begin(), all.end());
    std::printf ("# rings %d, cut by 3 dB or more %d (%.0f%%), median %.2f dB\n", total, cut3, total ? 100.0 * cut3 / total : 0.0, all.empty() ? 0.0 : all[all.size() / 2]);
    return 0;
}

int main (int argc, char** argv)
{
    if (argc >= 2 && std::strcmp (argv[1], "frames") == 0) return frames (argc, argv);
    if (argc >= 2 && std::strcmp (argv[1], "rings") == 0) return rings (argc, argv);
    if (argc >= 2 && std::strcmp (argv[1], "spacing") == 0) return spacing (argc, argv);
    std::fprintf (stderr, "RingProbe frames <wav> <from> <to> <hz> [name=value ...]\nRingProbe rings <events.txt> <wav folder> [name=value ...]\n");
    return 2;
}
