// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <chrono>
#include <cstring>
#include "NullTest.h"
#include <thread>
#include <unistd.h>

using namespace testkit;

static constexpr double twoPi = juce::MathConstants<double>::twoPi;

// ---------------------------------------------------------------- set up

struct Setup
{
    int doubles = 1;
    float loose = 30.0f, width = 0.0f, lead = -60.0f, d1 = -60.0f, d2 = -60.0f, d3 = -60.0f;
    float timing = 100.0f, pitch = 100.0f, vibrato = 0.0f, timbre = 0.0f, leadPan = 0.0f, doublesPan = 0.0f, extra = 0.0f;
    int range = 1;
};

static void setParam (PluginProcessor& p, const char* id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

static void apply (PluginProcessor& p, const Setup& s)
{
    setParam (p, "doubles", (float) s.doubles);
    setParam (p, "tightness", s.loose);
    setParam (p, "width", s.width);
    setParam (p, "lead", s.lead);
    setParam (p, "double1", s.d1);
    setParam (p, "double2", s.d2);
    setParam (p, "double3", s.d3);
    setParam (p, "timingamount", s.timing);
    setParam (p, "pitchamount", s.pitch);
    setParam (p, "vibrato", s.vibrato);
    setParam (p, "timbre", s.timbre);
    setParam (p, "leadpan", s.leadPan);
    setParam (p, "doublespan", s.doublesPan);
    setParam (p, "extradelay", s.extra);
    setParam (p, "range", (float) s.range);
    setParam (p, "bypass", 0.0f);
    // A Setup names every level it wants. Match Level, which a new Duo now opens with, would turn them all into something else, so
    // the tests that set levels switch it off; the tests of Match Level itself switch it on.
    setParam (p, "matchlevel", 0.0f);
}

// One double alone: lead and the others off. Doubles stays at 3 so every voice is available.
static Setup solo (int voice, float loose, float level = 0.0f, float width = 0.0f, int doubles = 3)
{
    Setup s;
    s.doubles = doubles; s.loose = loose; s.width = width;
    (voice == 0 ? s.d1 : voice == 1 ? s.d2 : s.d3) = level;
    return s;
}

static void prepareMonoToStereo (PluginProcessor& p, double sr, int block)
{
    p.releaseResources();
    auto layout = p.getBusesLayout();
    layout.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
    layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    p.setBusesLayout (layout);
    p.setRateAndBufferSizeDetails (sr, block);
    p.prepareToPlay (sr, block);
}

static std::vector<float> runSolo (const std::vector<float>& in, double sr, const Setup& s, int block = 512)
{
    PluginProcessor p;
    apply (p, s);
    prepare (p, 1, sr, block);
    auto out = render (p, in, block);
    return std::vector<float> (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples());
}

static double correlation (const float* a, const float* b, int n)
{
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < n; ++i) { ab += (double) a[i] * b[i]; aa += (double) a[i] * a[i]; bb += (double) b[i] * b[i]; }
    return ab / std::max (std::sqrt (aa * bb), 1.0e-20);
}

static double correlation (const std::vector<double>& a, const std::vector<double>& b)
{
    const auto n = std::min (a.size(), b.size());
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double) n; mb /= (double) n;
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i) { ab += (a[i] - ma) * (b[i] - mb); aa += (a[i] - ma) * (a[i] - ma); bb += (b[i] - mb) * (b[i] - mb); }
    return ab / std::max (std::sqrt (aa * bb), 1.0e-20);
}

// A sine whose period is a whole number of samples, so block sums below cancel its double frequency part exactly.
static std::vector<float> wholeCycleSine (int n, int period, double amp = 0.5)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (amp * std::sin (twoPi * (i % period) / period));
    return x;
}

// ---------------------------------------------------------------- tracking a voice from its audio

// Follows the phase of a sine through a block sum. A delay D turns into phase -2 pi f D, so this gives the delay change
// and, through its slope, the pitch. Nothing here reads the plug-in's internals.
struct Tracked
{
    std::vector<double> delayMs, cents, amp;
    double hopSeconds = 0;
    double timingPeak() const
    {
        const auto [lo, hi] = std::minmax_element (delayMs.begin(), delayMs.end());
        return 0.5 * (*hi - *lo);
    }
    double centsPeak() const { double m = 0; for (auto c : cents) m = std::max (m, std::abs (c)); return m; }
    double centsRms() const { double s = 0; for (auto c : cents) s += c * c; return std::sqrt (s / std::max<size_t> (cents.size(), 1)); }
};

static Tracked trackSine (const std::vector<float>& x, double sr, int period, int cyclesPerHop = 5, double skipSeconds = 0.2)
{
    Tracked t;
    const int hop = period * cyclesPerHop;
    t.hopSeconds = hop / sr;
    const double f = sr / period;
    double last = 0, unwrapped = 0;
    bool first = true;
    for (int start = (int) (skipSeconds * sr) / period * period; start + hop <= (int) x.size(); start += hop)
    {
        double i = 0, q = 0;
        for (int k = 0; k < hop; ++k)
        {
            const double a = twoPi * ((start + k) % period) / period;
            i += (double) x[(size_t) (start + k)] * std::sin (a);
            q += (double) x[(size_t) (start + k)] * std::cos (a);
        }
        const double b = std::atan2 (-q, i);
        if (first) { unwrapped = b; first = false; }
        else
        {
            double d = b - last;
            while (d > juce::MathConstants<double>::pi) d -= twoPi;
            while (d < -juce::MathConstants<double>::pi) d += twoPi;
            unwrapped += d;
        }
        last = b;
        t.delayMs.push_back (unwrapped / (twoPi * f) * 1000.0);
        t.amp.push_back (2.0 * std::sqrt (i * i + q * q) / hop);
    }
    // Pitch from the slope of the delay across 20 ms. A delay that grows lowers the pitch.
    for (size_t k = 2; k + 2 < t.delayMs.size(); ++k)
    {
        const double slope = (t.delayMs[k + 2] - t.delayMs[k - 2]) * 0.001 / (4.0 * t.hopSeconds);
        t.cents.push_back (1200.0 * std::log2 (1.0 - slope));
    }
    return t;
}

// Absolute delay at every impulse of an impulse train.
static std::vector<double> trackImpulses (const std::vector<float>& y, double sr, int spacing)
{
    std::vector<double> ms;
    for (int at = spacing; at + spacing < (int) y.size(); at += spacing)
    {
        int best = at; double bestV = 0;
        for (int i = at; i < at + spacing; ++i) if (std::abs (y[(size_t) i]) > bestV) { bestV = std::abs (y[(size_t) i]); best = i; }
        const double a = y[(size_t) best - 1], b = y[(size_t) best], c = y[(size_t) best + 1];
        const double denom = a - 2.0 * b + c;
        const double frac = std::abs (denom) > 1.0e-9 ? 0.5 * (a - c) / denom : 0.0;
        ms.push_back ((best + frac - at) / sr * 1000.0);
    }
    return ms;
}

static std::vector<float> impulseTrain (int n, int spacing)
{
    std::vector<float> x ((size_t) n, 0.0f);
    for (int i = spacing; i < n; i += spacing) x[(size_t) i] = 1.0f;
    return x;
}

// Share of a contour's energy that sits in one spectral line. A repeating sweep puts nearly everything in one line.
static double strongestLineShare (const std::vector<double>& contour)
{
    int order = 1;
    while ((1u << order) < contour.size() * 2) ++order;
    juce::dsp::FFT fft (order);
    const int size = 1 << order;
    std::vector<float> buf ((size_t) size * 2, 0.0f);
    double mean = 0; for (auto c : contour) mean += c; mean /= (double) contour.size();
    for (size_t i = 0; i < contour.size(); ++i)
        buf[i] = (float) ((contour[i] - mean) * (0.5 - 0.5 * std::cos (twoPi * (double) i / (double) (contour.size() - 1))));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    double total = 0, best = 0;
    for (int k = 1; k < size / 2; ++k) total += (double) buf[(size_t) k] * buf[(size_t) k];
    for (int k = 2; k + 2 < size / 2; ++k)
    {
        // Doubled length FFT with a Hann window: one line covers about 5 bins.
        double s = 0; for (int j = -2; j <= 2; ++j) s += (double) buf[(size_t) (k + j)] * buf[(size_t) (k + j)];
        best = std::max (best, s);
    }
    return best / std::max (total, 1.0e-20);
}

// What is left of a sine after taking away a clean sine that follows the voice's slow phase and level. Interpolation
// noise and any zipper show up here. Movement slower than about 100 Hz belongs to the voice's own drift and is not counted.
// Returns dB against the carrier and the level ripple in dB, peak to peak.
struct Residual { double db = 0, rippleDb = 0; };
static Residual sineResidual (const std::vector<float>& x, double sr, int period, double skipSeconds = 0.2)
{
    // Coarse 1 ms phase track, a whole number of cycles per block.
    const int cycles = juce::jmax (1, (int) std::round (sr * 0.001 / period));
    const auto t = trackSine (x, sr, period, cycles, skipSeconds);
    const double f = sr / period;
    const int hop = period * cycles;
    const int start0 = (int) (skipSeconds * sr) / period * period;
    // Savitzky-Golay smoothing keeps a curve's slope and bend, so a moving pitch is followed without error.
    static constexpr double sg[11] = { -36, 9, 44, 69, 84, 89, 84, 69, 44, 9, -36 };
    std::vector<double> phase (t.delayMs.size(), 0.0);
    for (size_t k = 5; k + 5 < t.delayMs.size(); ++k)
    {
        double s = 0; for (int j = -5; j <= 5; ++j) s += sg[j + 5] * t.delayMs[(size_t) ((int) k + j)];
        phase[k] = s / 429.0 * 0.001 * twoPi * f;
    }
    const auto spline = [] (const std::vector<double>& c, double pos)
    {
        const auto k = (size_t) pos;
        const double u = pos - (double) k, p0 = c[k - 1], p1 = c[k], p2 = c[k + 1], p3 = c[k + 2];
        return p1 + 0.5 * u * (p2 - p0 + u * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + u * (3.0 * (p1 - p2) + p3 - p0)));
    };
    // A block sum reports the phase at the middle of its block.
    const auto hopPos = [&] (int i) { return (i - start0 - 0.5 * (hop - 1)) / hop; };
    // Leave room so the wide window and the smoother never reach the unsmoothed ends of the track.
    const int firstK = 16, lastK = (int) t.delayMs.size() - 17;

    // Three refinements: turn the sine down to nearly nothing with the current reference, then smooth what is left with a
    // 10 ms Hann window. Its double frequency part cannot get through, unlike the coarse block sums.
    const int win = (int) (sr * 0.01);
    std::vector<double> hann ((size_t) win);
    double hannSum = 0;
    for (int i = 0; i < win; ++i) { hann[(size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / win); hannSum += hann[(size_t) i]; }
    std::vector<double> level (phase.size(), 0.0);
    for (int pass = 0; pass < 3; ++pass)
    {
        auto next = phase;
        for (int k = firstK; k <= lastK; ++k)
        {
            const int mid = start0 + k * hop + hop / 2; // window centre falls exactly on this block's middle (both lengths are even)
            double re = 0, im = 0;
            for (int w = 0; w < win; ++w)
            {
                const int i = mid - win / 2 + w;
                const double a = twoPi * (i % period) / period - spline (phase, hopPos (i));
                re += hann[(size_t) w] * (double) x[(size_t) i] * std::sin (a);
                im += hann[(size_t) w] * (double) x[(size_t) i] * std::cos (a);
            }
            next[(size_t) k] = phase[(size_t) k] + std::atan2 (-im, re);
            level[(size_t) k] = 2.0 * std::sqrt (re * re + im * im) / hannSum;
        }
        // Ripple the coarse track put in cannot be seen by the wide window, so it comes out here. The smoother keeps slope and bend.
        phase = next;
        for (int k = firstK; k <= lastK; ++k)
        {
            double sum = 0; for (int j = -5; j <= 5; ++j) sum += sg[j + 5] * next[(size_t) (k + j)];
            phase[(size_t) k] = sum / 429.0;
        }
    }

    const int from = start0 + (firstK + 20) * hop, to = start0 + (lastK - 20) * hop, n = to - from;
    std::vector<double> ref ((size_t) n);
    for (int i = 0; i < n; ++i)
        ref[(size_t) i] = std::sin (twoPi * ((from + i) % period) / period - spline (phase, hopPos (from + i)));
    double xr = 0, rr = 0;
    for (int i = 0; i < n; ++i) { xr += (double) x[(size_t) (from + i)] * ref[(size_t) i]; rr += ref[(size_t) i] * ref[(size_t) i]; }
    const double a = xr / rr;
    double e = 0;
    for (int i = 0; i < n; ++i) { const double d = (double) x[(size_t) (from + i)] - a * ref[(size_t) i]; e += d * d; }
    Residual r;
    r.db = 10.0 * std::log10 (std::max (e / (a * a * rr), 1.0e-20));
    double lo = 1.0e9, hi = 0;
    for (int k = firstK + 20; k <= lastK - 20; ++k) { lo = std::min (lo, level[(size_t) k]); hi = std::max (hi, level[(size_t) k]); }
    r.rippleDb = 20.0 * std::log10 (hi / lo);
    return r;
}

// ---------------------------------------------------------------- tests

// Doubles sets the number of separate copies.
static void testDoublesCount()
{
    const double sr = 48000.0;
    const int spacing = 12000;
    const auto in = impulseTrain (48000 * 8, spacing);
    for (float loose : { 0.0f, 100.0f })
        for (int n = 1; n <= 3; ++n)
        {
            Setup s; s.doubles = n; s.loose = loose; s.lead = 0.0f; s.d1 = s.d2 = s.d3 = 0.0f;
            PluginProcessor p; apply (p, s); prepare (p, 1, sr, 512);
            auto out = render (p, in, 512);
            const auto* y = out.getReadPointer (0);
            int exact = 0, total = 0, leadOk = 0;
            for (int at = spacing; at + spacing < out.getNumSamples(); at += spacing)
            {
                int peaks = 0; int lastPeak = -100000;
                for (int i = at; i < at + spacing - 1; ++i)
                    if (std::abs (y[i]) > 0.3f && i - lastPeak > (int) (0.001 * sr)) { ++peaks; lastPeak = i; }
                if (peaks == n + 1) ++exact;
                if (std::abs (y[at] - 1.0f) < 1.0e-4f) ++leadOk;
                ++total;
            }
            const auto tag = "Doubles " + std::to_string (n) + " at Tightness " + std::to_string ((int) loose);
            if (loose == 0.0f)
                check (exact == total, tag + ": every impulse gives lead plus this many copies, share", (double) exact / total);
            else // loose voices wander and can meet for a moment, which is what separate takes do
                check ((double) exact / total >= 0.85, tag + ": impulses showing lead plus this many copies, share", (double) exact / total);
            check (leadOk == total, tag + ": lead stays at its own time");
        }

    // A level knob for an unused double does nothing.
    {
        Setup s = solo (0, 30.0f, -60.0f, 0.0f, 1); s.d2 = 0.0f; s.d3 = 0.0f;
        const auto out = runSolo (wholeCycleSine (48000, 48), sr, s);
        check (toDb (rms (out.data(), 48000)) < -120.0, "Doubles 1: double 2 and 3 levels make no sound", toDb (rms (out.data(), 48000)), "dB");
    }
}

// Timing and pitch range against Tightness, measured from the audio of each voice.
static void testDriftRanges()
{
    const double sr = 48000.0;
    const int seconds = 120, n = (int) sr * seconds;
    const auto sineIn = wholeCycleSine (n, 48);
    const int spacing = 9600;
    const auto impulses = impulseTrain (n, spacing);
    const float steps[] = { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f };
    for (int v = 0; v < 3; ++v)
    {
        double lastTiming = -1, lastCents = -1;
        bool growTiming = true, growCents = true;
        for (float loose : steps)
        {
            const auto t = trackSine (runSolo (sineIn, sr, solo (v, loose)), sr, 48);
            const auto ms = trackImpulses (runSolo (impulses, sr, solo (v, loose)), sr, spacing);
            const auto [lo, hi] = std::minmax_element (ms.begin(), ms.end());
            double mean = 0; for (auto m : ms) mean += m; mean /= (double) ms.size();
            const auto tag = "Double " + std::to_string (v + 1) + " Tightness " + std::to_string ((int) loose);
            info (tag + " timing swing, plus or minus", t.timingPeak(), "ms");
            info (tag + " pitch peak", t.centsPeak(), "cents");
            info (tag + " pitch rms", t.centsRms(), "cents");
            info (tag + " delay behind the lead, mean", mean, "ms");
            info (tag + " delay behind the lead, shortest", *lo, "ms");
            info (tag + " delay behind the lead, longest", *hi, "ms");

            // Two independent measurements of the same movement.
            const double impulseSwing = 0.5 * (*hi - *lo);
            check (std::abs (impulseSwing - t.timingPeak()) <= std::max (0.2, 0.15 * t.timingPeak()), tag + ": impulse and sine agree on the timing swing, impulse", impulseSwing, "ms");
            check (*lo >= 5.0, tag + ": a double is never closer than 5 ms", *lo, "ms");

            growTiming = growTiming && t.timingPeak() > lastTiming;
            growCents = growCents && t.centsPeak() > lastCents;
            lastTiming = t.timingPeak(); lastCents = t.centsPeak();

            if (loose == 0.0f)
            {
                check (t.timingPeak() >= 0.5 && t.timingPeak() <= 2.5, tag + ": tight timing swing between 0.5 and 2.5 ms", t.timingPeak(), "ms");
                check (t.centsPeak() >= 0.5 && t.centsPeak() <= 5.0, tag + ": tight pitch peak between 0.5 and 5 cents", t.centsPeak(), "cents");
            }
            if (loose == 100.0f)
            {
                check (t.timingPeak() >= 8.0 && t.timingPeak() <= 17.0, tag + ": loose timing swing between 8 and 17 ms", t.timingPeak(), "ms");
                check (t.centsPeak() >= 10.0 && t.centsPeak() <= 35.0, tag + ": loose pitch peak between 10 and 35 cents", t.centsPeak(), "cents");
            }
        }
        check (growTiming, "Double " + std::to_string (v + 1) + ": timing swing grows at every step of Tightness");
        check (growCents, "Double " + std::to_string (v + 1) + ": pitch peak grows at every step of Tightness");
    }
}

// Lead and double levels within 0.2 dB.
static void testLevels()
{
    const double sr = 48000.0;
    const int n = 48000 * 3, skip = 9600;
    const float levels[] = { -24.0f, -12.0f, -6.0f, 0.0f, 6.0f };
    double worst = 0;
    bool allOk = true;
    const auto note = [&] (double err, const std::string& tag)
    {
        worst = std::max (worst, std::abs (err));
        if (std::abs (err) > 0.2) { allOk = false; check (false, tag + " level error", err, "dB"); }
    };

    for (int period : { 48, 10 }) // 1 kHz and 4.8 kHz
    {
        const auto in = wholeCycleSine (n, period, 0.25);
        const double inDb = toDb (rms (in.data() + skip, n - skip));
        for (float level : levels)
        {
            { // lead alone
                Setup s; s.lead = level;
                PluginProcessor p; apply (p, s); prepare (p, 2, sr, 512);
                auto out = render (p, in, 512);
                note (rmsDb (out, 0, skip, n - skip) - inDb - level, "Lead " + std::to_string ((int) level));
                note (rmsDb (out, 1, skip, n - skip) - inDb - level, "Lead right " + std::to_string ((int) level));
            }
            for (int doubles = 1; doubles <= 3; ++doubles)
                for (int v = 0; v < doubles; ++v)
                    for (float loose : { 30.0f, 100.0f })
                        for (float width : { 0.0f, 100.0f })
                        {
                            PluginProcessor p; apply (p, solo (v, loose, level, width, doubles)); prepare (p, 2, sr, 512);
                            auto out = render (p, in, 512);
                            const double l = rms (out.getReadPointer (0) + skip, n - skip), r = rms (out.getReadPointer (1) + skip, n - skip);
                            const auto tag = "Double " + std::to_string (v + 1) + " of " + std::to_string (doubles) + " level " + std::to_string ((int) level) + " width " + std::to_string ((int) width);
                            if (width == 0.0f)
                            {
                                note (toDb (l) - inDb - level, tag);
                                note (toDb (r) - inDb - level, tag);
                            }
                            else // panned voices keep their power: left and right together match the dial
                                note (toDb (std::sqrt (0.5 * (l * l + r * r))) - inDb - level, tag);
                        }
        }
    }
    check (allOk, "lead and every double within 0.2 dB, at 1 kHz and 4.8 kHz, Tightness 30 and 100, Width 0 and 100, worst", worst, "dB");

    // Width in between keeps power on a broad signal. A sine would beat against the split copy, as any double does.
    const int m = 48000 * 12;
    std::vector<float> nz ((size_t) m); noise (nz.data(), m, 0.25);
    const double nzDb = toDb (rms (nz.data() + skip, m - skip));
    double worstMid = 0;
    for (int doubles = 1; doubles <= 3; ++doubles)
        for (int v = 0; v < doubles; ++v)
            for (float width : { 25.0f, 50.0f, 75.0f })
            {
                PluginProcessor p; apply (p, solo (v, 30.0f, 0.0f, width, doubles)); prepare (p, 2, sr, 512);
                auto out = render (p, nz, 512);
                const double l = rms (out.getReadPointer (0) + skip, m - skip), r = rms (out.getReadPointer (1) + skip, m - skip);
                worstMid = std::max (worstMid, std::abs (toDb (std::sqrt (0.5 * (l * l + r * r))) - nzDb));
            }
    check (worstMid <= 0.2, "partial Width keeps the level on noise within 0.2 dB, worst", worstMid, "dB");
}

static std::vector<float> voiceBandNoise (int n, unsigned seed = 3)
{
    std::vector<float> x ((size_t) n); noise (x.data(), n, 0.5, seed);
    // One pole low-pass at 2 kHz, then high-pass at 100 Hz: energy where a voice has it.
    const double c = 1.0 - std::exp (-twoPi * 2000.0 / 48000.0), h = 1.0 - std::exp (-twoPi * 100.0 / 48000.0);
    double lp = 0, hp = 0;
    for (auto& s : x) { lp += c * (s - lp); hp += h * (lp - hp); s = (float) (lp - hp); }
    return x;
}

// Width changes left and right correlation.
static void testWidth()
{
    const double sr = 48000.0;
    const int n = 48000 * 20, skip = 9600;
    std::vector<float> white ((size_t) n); noise (white.data(), n, 0.25);
    const auto band = voiceBandNoise (n);
    const float widths[] = { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f };

    for (int doubles = 1; doubles <= 3; ++doubles)
    {
        double last = 2.0; bool falling = true;
        for (float w : widths)
        {
            Setup s; s.doubles = doubles; s.width = w; s.d1 = s.d2 = s.d3 = 0.0f;
            PluginProcessor p; apply (p, s); prepareMonoToStereo (p, sr, 512);
            auto out = render (p, white, 512);
            const double r = correlation (out.getReadPointer (0) + skip, out.getReadPointer (1) + skip, n - skip);
            info ("Doubles " + std::to_string (doubles) + " Width " + std::to_string ((int) w) + " left and right correlation, doubles only, noise", r);
            falling = falling && r < last - 0.05;
            last = r;
            if (w == 0.0f) check (r >= 0.9999, "Doubles " + std::to_string (doubles) + " Width 0: left equals right, correlation", r);
            if (w == 100.0f) check (std::abs (r) <= 0.1, "Doubles " + std::to_string (doubles) + " Width 100: left and right separate, correlation", r);
        }
        check (falling, "Doubles " + std::to_string (doubles) + ": correlation falls by at least 0.05 at every step of Width");

        // Energy shaped like a voice
        Setup s; s.doubles = doubles; s.width = 100.0f; s.d1 = s.d2 = s.d3 = 0.0f;
        PluginProcessor p; apply (p, s); prepareMonoToStereo (p, sr, 512);
        auto out = render (p, band, 512);
        const double r = correlation (out.getReadPointer (0) + skip, out.getReadPointer (1) + skip, n - skip);
        check (std::abs (r) <= 0.25, "Doubles " + std::to_string (doubles) + " Width 100, voice band noise, correlation", r);

        // With the lead on, what stays correlated is the lead.
        s.lead = 0.0f; s.d1 = s.d2 = s.d3 = -6.0f;
        PluginProcessor q; apply (q, s); prepareMonoToStereo (q, sr, 512);
        auto outLead = render (q, white, 512);
        const double rl = correlation (outLead.getReadPointer (0) + skip, outLead.getReadPointer (1) + skip, n - skip);
        const double expected = 1.0 / (1.0 + doubles * 0.25118);
        check (std::abs (rl - expected) <= 0.05, "Doubles " + std::to_string (doubles) + " Width 100 with lead 0 dB and doubles -6 dB, correlation close to lead power share " + std::to_string (expected).substr (0, 5), rl);
    }
}

// Largest normalised cross-correlation at any lag up to 100 ms.
static double peakCrossCorrelation (const std::vector<float>& a, const std::vector<float>& b, int skip, int lagSamples)
{
    const int n = (int) std::min (a.size(), b.size()) - skip;
    int order = 1; while ((1 << order) < 2 * n) ++order;
    juce::dsp::FFT fft (order);
    const int size = 1 << order;
    std::vector<float> fa ((size_t) size * 2, 0.0f), fb ((size_t) size * 2, 0.0f);
    std::copy_n (a.data() + skip, n, fa.data());
    std::copy_n (b.data() + skip, n, fb.data());
    fft.performRealOnlyForwardTransform (fa.data());
    fft.performRealOnlyForwardTransform (fb.data());
    for (int k = 0; k <= size / 2; ++k)
    {
        const float ar = fa[(size_t) (2 * k)], ai = fa[(size_t) (2 * k + 1)], br = fb[(size_t) (2 * k)], bi = fb[(size_t) (2 * k + 1)];
        fa[(size_t) (2 * k)] = ar * br + ai * bi;
        fa[(size_t) (2 * k + 1)] = ai * br - ar * bi;
    }
    fft.performRealOnlyInverseTransform (fa.data());
    const double norm = std::sqrt ((double) rms (a.data() + skip, n) * rms (a.data() + skip, n) * rms (b.data() + skip, n) * rms (b.data() + skip, n)) * n;
    double best = 0;
    for (int lag = 0; lag <= lagSamples; ++lag)
    {
        best = std::max (best, (double) std::abs (fa[(size_t) lag]) / norm);
        if (lag > 0) best = std::max (best, (double) std::abs (fa[(size_t) (size - lag)]) / norm);
    }
    return best;
}

// Doubles stay separate from each other.
static void testDecorrelation()
{
    const double sr = 48000.0;
    const int n = 48000 * 20, skip = 9600, lags = 4800;
    std::vector<float> white ((size_t) n); noise (white.data(), n, 0.25);
    const auto band = voiceBandNoise (n);
    const std::pair<int, int> pairs[] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };

    // The control: a plain fixed copy 20 ms later has to read as fully correlated, or the numbers below prove nothing.
    {
        std::vector<float> later ((size_t) n, 0.0f);
        std::copy_n (white.data(), n - 960, later.data() + 960);
        const double c = peakCrossCorrelation (white, later, skip, lags);
        check (c >= 0.95, "control: a fixed copy 20 ms later reads as the same signal, correlation at best lag", c);
    }

    for (float loose : { 0.0f, 30.0f, 100.0f })
    {
        const auto tag = "Tightness " + std::to_string ((int) loose);
        std::array<std::vector<float>, 3> w, b;
        for (int v = 0; v < 3; ++v) { w[(size_t) v] = runSolo (white, sr, solo (v, loose)); b[(size_t) v] = runSolo (band, sr, solo (v, loose)); }
        double worstZero = 0, worstLagW = 0, worstLagB = 0;
        for (auto [x, y] : pairs)
        {
            worstZero = std::max (worstZero, std::abs (correlation (w[(size_t) x].data() + skip, w[(size_t) y].data() + skip, n - skip)));
            worstLagW = std::max (worstLagW, peakCrossCorrelation (w[(size_t) x], w[(size_t) y], skip, lags));
            worstLagB = std::max (worstLagB, peakCrossCorrelation (b[(size_t) x], b[(size_t) y], skip, lags));
        }
        check (worstZero <= 0.05, tag + ": doubles against each other, noise, same time, worst pair", worstZero);
        check (worstLagW <= 0.10, tag + ": doubles against each other, noise, best lag up to 100 ms, worst pair", worstLagW);
        check (worstLagB <= 0.35, tag + ": doubles against each other, voice band noise, best lag up to 100 ms, worst pair", worstLagB);
    }

    // The drift paths themselves share nothing.
    const int m = 48000 * 300;
    const auto in = wholeCycleSine (m, 48);
    std::array<Tracked, 3> t;
    for (int v = 0; v < 3; ++v) t[(size_t) v] = trackSine (runSolo (in, sr, solo (v, 50.0f)), sr, 48);
    double worstTiming = 0, worstPitch = 0;
    for (auto [x, y] : pairs)
    {
        worstTiming = std::max (worstTiming, std::abs (correlation (t[(size_t) x].delayMs, t[(size_t) y].delayMs)));
        worstPitch = std::max (worstPitch, std::abs (correlation (t[(size_t) x].cents, t[(size_t) y].cents)));
    }
    check (worstTiming <= 0.35, "timing paths over 5 minutes, correlation between doubles, worst pair", worstTiming);
    check (worstPitch <= 0.20, "pitch paths over 5 minutes, correlation between doubles, worst pair", worstPitch);
}

// No zipper from the moving read head, and no repeating sweep.
static void testArtifacts()
{
    const double sr = 48000.0;
    const int n = 48000 * 40;

    // Zipper: limits at the loosest setting, where the head moves fastest.
    struct Case { int period; double limit; };
    for (auto c : { Case { 48, -70.0 }, Case { 10, -70.0 }, Case { 5, -70.0 }, Case { 3, -50.0 } })
    {
        const auto in = wholeCycleSine (n, c.period);
        for (int v = 0; v < 3; ++v)
        {
            const auto r = sineResidual (runSolo (in, sr, solo (v, 100.0f)), sr, c.period);
            const auto hz = std::to_string ((int) (sr / c.period));
            check (r.db <= c.limit, "zipper and read noise, double " + std::to_string (v + 1) + ", " + hz + " Hz, against the note, limit " + std::to_string ((int) c.limit), r.db, "dB");
            if (c.period >= 5)
                check (r.rippleDb <= 0.1, "level ripple, double " + std::to_string (v + 1) + ", " + hz + " Hz, peak to peak, limit 0.1", r.rippleDb, "dB");
            else
                info ("level ripple, double " + std::to_string (v + 1) + ", " + hz + " Hz, peak to peak", r.rippleDb, "dB");
        }
    }

    // The instrument itself: a perfect moving sine, written directly and moving as fast as the loosest double (about
    // 26 cents at its peak), must measure far below the limits, and the same sine with a known -60 dB side tone must measure -60 dB.
    for (auto c : { Case { 48, -70.0 }, Case { 10, -70.0 }, Case { 5, -70.0 }, Case { 3, -50.0 } })
    {
        const double f = sr / c.period;
        std::vector<float> perfect ((size_t) n), marked ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double delay = 0.040 + 0.012 * std::sin (twoPi * 0.1 * t) + 0.0015 * std::sin (twoPi * 0.8 * t);
            const double whole = twoPi * (i % c.period) / c.period - twoPi * f * delay;
            perfect[(size_t) i] = (float) (0.5 * std::sin (whole));
            marked[(size_t) i] = (float) (0.5 * std::sin (whole) + 0.0005 * std::sin (whole + twoPi * 500.0 * t));
        }
        const auto hz = std::to_string ((int) f);
        const auto floor = sineResidual (perfect, sr, c.period), known = sineResidual (marked, sr, c.period);
        check (floor.db <= c.limit - 10.0, "instrument floor at " + hz + " Hz is at least 10 dB under the limit", floor.db, "dB");
        check (std::abs (known.db + 60.0) <= 1.0, "instrument reads a known -60 dB side tone at " + hz + " Hz as", known.db, "dB");
    }

    // Periodic sweep: how much of each path's energy sits in one line. Five minutes, so a slow repeat would show.
    const int m = 48000 * 300;
    const auto in = wholeCycleSine (m, 48);
    double worstTiming = 0, worstPitch = 0;
    for (float loose : { 20.0f, 100.0f })
        for (int v = 0; v < 3; ++v)
        {
            const auto t = trackSine (runSolo (in, sr, solo (v, loose)), sr, 48);
            worstTiming = std::max (worstTiming, strongestLineShare (t.delayMs));
            worstPitch = std::max (worstPitch, strongestLineShare (t.cents));
        }
    check (worstTiming <= 0.30, "sweep rate: largest share of timing path energy in one spectral line, limit 0.30", worstTiming);
    check (worstPitch <= 0.10, "sweep rate: largest share of pitch path energy in one spectral line, limit 0.10", worstPitch);

    // The control: the same measurement on a plain repeating chorus sweep must light up, or the two lines above prove nothing.
    {
        std::vector<float> lfo ((size_t) m);
        for (int i = 0; i < m; ++i)
        {
            const double t = i / sr, delay = 0.02 + 0.003 * std::sin (twoPi * 0.8 * t);
            lfo[(size_t) i] = (float) (0.5 * std::sin (twoPi * 1000.0 * (t - delay)));
        }
        const auto t = trackSine (lfo, sr, 48);
        const double timing = strongestLineShare (t.delayMs), pitch = strongestLineShare (t.cents);
        check (timing >= 0.8, "control: a repeating 0.8 Hz sweep does show as a line in timing, share", timing);
        check (pitch >= 0.8, "control: a repeating 0.8 Hz sweep does show as a line in pitch, share", pitch);
    }
}

// A small two head pitch shifter, only to compare against the plug-in's moving delay. The brief asked for whichever measures best.
static std::vector<float> twoHeadShifter (const std::vector<float>& x, double sr, double cents, double grainMs)
{
    const double ratio = std::pow (2.0, cents / 1200.0);
    const double grain = grainMs * 0.001 * sr;
    const double rate = (1.0 - ratio) / grain; // change of phase per sample
    double ph = 0.0;
    std::vector<float> y (x.size(), 0.0f);
    const auto hermite = [&] (double pos)
    {
        const int i = (int) std::floor (pos);
        if (i < 1 || i + 2 >= (int) x.size()) return 0.0;
        const double t = pos - i, a = x[(size_t) i - 1], b = x[(size_t) i], c = x[(size_t) i + 1], d = x[(size_t) i + 2];
        return b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
    };
    for (int i = 0; i < (int) x.size(); ++i)
    {
        ph += rate; ph -= std::floor (ph);
        double ph2 = ph + 0.5; ph2 -= std::floor (ph2);
        const double g1 = std::sin (juce::MathConstants<double>::pi * ph), g2 = std::sin (juce::MathConstants<double>::pi * ph2);
        y[(size_t) i] = (float) (g1 * g1 * hermite (i - 5.0 - ph * grain) + g2 * g2 * hermite (i - 5.0 - ph2 * grain));
    }
    return y;
}

static void testMethodChoice()
{
    const double sr = 48000.0;
    const int n = 48000 * 40;
    for (int period : { 48, 10 })
    {
        const auto in = wholeCycleSine (n, period);
        const auto moving = sineResidual (runSolo (in, sr, solo (0, 60.0f)), sr, period);
        const auto shifted = sineResidual (twoHeadShifter (in, sr, 10.0, 34.7), sr, period);
        const auto hz = std::to_string ((int) (sr / period));
        info ("method: moving delay residual at " + hz + " Hz", moving.db, "dB");
        info ("method: moving delay level ripple at " + hz + " Hz", moving.rippleDb, "dB");
        info ("method: two head pitch shifter residual at " + hz + " Hz", shifted.db, "dB");
        info ("method: two head pitch shifter level ripple at " + hz + " Hz", shifted.rippleDb, "dB");
        check (moving.db + 20.0 <= shifted.db, "method: moving delay is at least 20 dB cleaner than a two head shifter at " + hz + " Hz, moving delay minus shifter", moving.db - shifted.db, "dB");
    }
}

// Mono in to stereo out, and the other layouts.
static void testLayouts()
{
    const double sr = 48000.0;
    const int n = 48000 * 4, skip = 9600;
    std::vector<float> in ((size_t) n); noise (in.data(), n, 0.25);

    {
        PluginProcessor p;
        auto l = p.getBusesLayout();
        const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
        l.inputBuses.getReference (0) = mono; l.outputBuses.getReference (0) = stereo;
        check (p.checkBusesLayoutSupported (l), "mono in to stereo out is supported");
        l.inputBuses.getReference (0) = mono; l.outputBuses.getReference (0) = mono;
        check (p.checkBusesLayoutSupported (l), "mono in to mono out is supported");
        l.inputBuses.getReference (0) = stereo; l.outputBuses.getReference (0) = stereo;
        check (p.checkBusesLayoutSupported (l), "stereo in to stereo out is supported");
        l.inputBuses.getReference (0) = stereo; l.outputBuses.getReference (0) = mono;
        check (! p.checkBusesLayoutSupported (l), "stereo in to mono out is refused");
    }

    { // Default settings from a mono track
        PluginProcessor p; prepareMonoToStereo (p, sr, 512);
        auto out = render (p, in, 512);
        const double l = rmsDb (out, 0, skip, n - skip), r = rmsDb (out, 1, skip, n - skip);
        check (l > -40.0 && r > -40.0, "mono to stereo at default settings: both sides sound, left", l, "dB");
        check (std::abs (l - r) <= 0.5, "mono to stereo at default settings: sides balanced within 0.5 dB, difference", l - r, "dB");
        check (correlation (out.getReadPointer (0) + skip, out.getReadPointer (1) + skip, n - skip) < 0.95, "mono to stereo at default settings: the sides differ");
    }

    for (int doubles = 1; doubles <= 3; ++doubles)
    {
        Setup s; s.doubles = doubles; s.width = 100.0f; s.lead = 0.0f; s.d1 = s.d2 = s.d3 = -6.0f;
        PluginProcessor p; apply (p, s); prepareMonoToStereo (p, sr, 512);
        auto out = render (p, in, 512);
        const double l = rmsDb (out, 0, skip, n - skip), r = rmsDb (out, 1, skip, n - skip);
        check (std::abs (l - r) <= 1.0, "mono to stereo, Doubles " + std::to_string (doubles) + " Width 100: balanced within 1 dB, difference", l - r, "dB");
    }

    { // Width 0 is exactly mono
        Setup s; s.doubles = 3; s.width = 0.0f; s.lead = 0.0f; s.d1 = s.d2 = s.d3 = -6.0f;
        PluginProcessor p; apply (p, s); prepareMonoToStereo (p, sr, 512);
        auto out = render (p, in, 512);
        double diff = 0; for (int i = 0; i < n; ++i) diff = std::max (diff, (double) std::abs (out.getSample (0, i) - out.getSample (1, i)));
        check (diff == 0.0, "mono to stereo at Width 0: left and right identical, largest difference", diff);
    }

    { // What a host leaves in the unused output channel must not reach the sound
        Setup s; s.doubles = 2; s.width = 100.0f; s.lead = 0.0f; s.d1 = s.d2 = 0.0f;
        PluginProcessor a, b; apply (a, s); apply (b, s); prepareMonoToStereo (a, sr, 512); prepareMonoToStereo (b, sr, 512);
        std::vector<float> junk ((size_t) n); noise (junk.data(), n, 1.0, 99);
        auto clean = render (a, in, 512);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        double diff = 0;
        for (int pos = 0; pos + 512 <= n; pos += 512)
        {
            std::copy_n (in.data() + pos, 512, buf.getWritePointer (0));
            std::copy_n (junk.data() + pos, 512, buf.getWritePointer (1));
            b.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    diff = std::max (diff, (double) std::abs (buf.getSample (ch, i) - clean.getSample (ch, pos + i)));
        }
        check (diff == 0.0, "mono to stereo: junk in the unused channel changes nothing, largest difference", diff);
    }

    // Every rate and small and large blocks, with all controls moving, and bad input from upstream.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (int block : { 32, 4096 })
        {
            PluginProcessor p; prepareMonoToStereo (p, rate, block);
            const int len = (int) rate;
            std::vector<float> sweepIn ((size_t) len); noise (sweepIn.data(), len, 0.2);
            for (int i = 0; i < 64; ++i) sweepIn[(size_t) (len / 2 + i)] = i % 2 == 0 ? std::numeric_limits<float>::quiet_NaN() : 1.0e30f;
            long before = 0, total = 0; bool armed = false;
            auto out = render (p, sweepIn, block, nullptr, [&] (int idx)
            {
                if (armed) total += allocs.load() - before;
                int k = 0;
                for (auto* prm : p.getParameters())
                    prm->setValueNotifyingHost (0.5f + 0.5f * (float) std::sin (0.13 * ++k + 0.2 * idx));
                armed = idx > 0;
                before = allocs.load();
            });
            double pk = 0; for (int ch = 0; ch < 2; ++ch) pk = std::max (pk, peak (out.getReadPointer (ch), out.getNumSamples()));
            const auto tag = std::to_string ((int) rate) + " Hz block " + std::to_string (block);
            check (countNonFinite (out) == 0 && pk <= 16.0 * 6.0, "mono to stereo, all controls moving, bad input, stays finite and bounded, " + tag + ", peak", pk);
            check (total == 0, "mono to stereo, no allocations in the audio path, " + tag, (double) total);
        }

    { // Mono to mono has no left or right to spread: same level at any Width
        Setup s = solo (0, 30.0f, -6.0f, 100.0f, 1);
        PluginProcessor p; apply (p, s); prepare (p, 1, sr, 512);
        auto out = render (p, in, 512);
        const double level = rmsDb (out, 0, skip, n - skip) - toDb (rms (in.data() + skip, n - skip));
        check (std::abs (level + 6.0) <= 0.2, "mono to mono, Width has no effect on level, double at -6 dB measures", level, "dB");
    }

    { // Stereo in: the lead keeps its own left and right
        Setup s; s.lead = 0.0f;
        PluginProcessor p; apply (p, s); prepare (p, 2, sr, 512);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        double worst = 0;
        for (int pos = 0; pos + 512 <= n; pos += 512)
        {
            std::copy_n (in.data() + pos, 512, buf.getWritePointer (0));
            std::copy_n (in.data() + n - pos - 512, 512, buf.getWritePointer (1));
            p.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i)
            {
                worst = std::max (worst, (double) std::abs (buf.getSample (0, i) - in[(size_t) (pos + i)]));
                worst = std::max (worst, (double) std::abs (buf.getSample (1, i) - in[(size_t) (n - pos - 512 + i)]));
            }
        }
        check (worst == 0.0, "stereo in: with doubles off the lead is untouched on both sides, largest difference", worst);
    }
}

// Latency, the same whether bypassed or not, at every rate.
static void testLatencyAndBypass()
{
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        const auto tag = "at " + std::to_string ((int) sr) + " Hz";
        for (int block : { 32, 512 })
        {
            Setup s; s.doubles = 3; s.lead = 0.0f; s.d1 = s.d2 = s.d3 = -6.0f; s.width = 50.0f;
            PluginProcessor p; apply (p, s); prepare (p, 2, sr, block);
            check (p.getLatencySamples() == 0, "reported latency " + tag + ", block " + std::to_string (block), (double) p.getLatencySamples(), "samples");
            std::vector<float> in ((size_t) (sr * 0.5), 0.0f);
            const int at = 2000;
            in[(size_t) at] = 1.0f;
            auto out = render (p, in, block);
            int first = -1; for (int i = 0; i < out.getNumSamples() && first < 0; ++i) if (std::abs (out.getSample (0, i)) > 0.1f) first = i;
            check (first == at, "measured lead latency " + tag + ", block " + std::to_string (block), (double) (first - at), "samples");

            // Doubles are behind the lead by real time, whatever the rate.
            int firstDouble = -1; for (int i = at + 1; i < out.getNumSamples() && firstDouble < 0; ++i) if (std::abs (out.getSample (0, i)) > 0.1f) firstDouble = i;
            const double ms = (firstDouble - at) / sr * 1000.0;
            check (ms > 5.0 && ms < 40.0, "first double behind the lead " + tag, ms, "ms");
        }

        // Host bypass
        {
            Setup s; s.doubles = 3; s.lead = -12.0f; s.d1 = s.d2 = s.d3 = 0.0f; s.width = 100.0f;
            PluginProcessor p; apply (p, s); prepare (p, 2, sr, 256);
            std::vector<float> in ((size_t) sr); noise (in.data(), (int) sr, 0.25);
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer midi;
            double worst = 0;
            const int settled = (int) (sr * 0.1);
            for (int pos = 0; pos + 256 <= (int) sr; pos += 256)
            {
                for (int ch = 0; ch < 2; ++ch) std::copy_n (in.data() + pos, 256, buf.getWritePointer (ch));
                p.processBlockBypassed (buf, midi);
                if (pos >= settled)
                    for (int i = 0; i < 256; ++i)
                        worst = std::max (worst, (double) std::abs (buf.getSample (0, i) - in[(size_t) (pos + i)]));
            }
            check (worst == 0.0, "host bypass gives the exact dry signal, no delay, " + tag + ", largest difference", worst);
            check (p.getLatencySamples() == 0, "latency when bypassed " + tag, (double) p.getLatencySamples(), "samples");
        }

        // Bypass parameter
        {
            Setup s; s.doubles = 2; s.lead = 3.0f; s.d1 = s.d2 = 0.0f;
            PluginProcessor p; apply (p, s); prepareMonoToStereo (p, sr, 256);
            setParam (p, "bypass", 1.0f);
            std::vector<float> in ((size_t) sr); noise (in.data(), (int) sr, 0.25);
            auto out = render (p, in, 256);
            double worst = 0;
            for (int i = (int) (sr * 0.1); i < (int) sr; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    worst = std::max (worst, (double) std::abs (out.getSample (ch, i) - in[(size_t) i]));
            check (worst == 0.0, "Bypass control, mono to stereo, exact dry on both sides, " + tag + ", largest difference", worst);
        }
    }
    check (std::abs (PluginProcessor().getTailLengthSeconds() - 0.25) < 1.0e-9, "reports a 250 ms tail");
}

// Times and pitch stay the same in real terms at every sample rate.
static void testSampleRates()
{
    const int seconds = 60;
    std::vector<double> reference;
    double refCents = 0;
    for (double sr : { 48000.0, 44100.0, 96000.0 })
    {
        const int n = (int) sr * seconds, spacing = (int) (sr * 0.25);
        const auto ms = trackImpulses (runSolo (impulseTrain (n, spacing), sr, solo (1, 60.0f)), sr, spacing);
        const int period = (int) std::round (sr / 1000.0);
        const auto t = trackSine (runSolo (wholeCycleSine (n, period), sr, solo (1, 60.0f)), sr, period);
        const auto tag = "at " + std::to_string ((int) sr) + " Hz";
        if (reference.empty()) { reference = ms; refCents = t.centsPeak(); continue; }
        double worst = 0;
        for (size_t i = 0; i < std::min (ms.size(), reference.size()); ++i) worst = std::max (worst, std::abs (ms[i] - reference[i]));
        check (worst <= 0.1, "timing path matches 48 kHz " + tag + ", worst", worst, "ms");
        check (std::abs (t.centsPeak() - refCents) <= 0.1 * refCents, "pitch peak matches 48 kHz within 10 percent " + tag, t.centsPeak(), "cents");
    }
}

struct FakeHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i;
        i.setBpm (97.0);
        i.setTimeInSamples (123456);
        i.setIsPlaying (true);
        return i;
    }
};

// Bounces repeat exactly, and block size or a playhead changes nothing.
static void testRepeatable()
{
    const double sr = 48000.0;
    const int n = 48000 * 5;
    std::vector<float> in ((size_t) n); noise (in.data(), n, 0.25);
    Setup s; s.doubles = 3; s.loose = 60.0f; s.width = 80.0f; s.lead = 0.0f; s.d1 = s.d2 = s.d3 = -6.0f;

    PluginProcessor ref; apply (ref, s); prepare (ref, 2, sr, 512);
    const auto base = render (ref, in, 512);
    const auto differs = [&] (const juce::AudioBuffer<float>& o)
    {
        double worst = 0;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i) worst = std::max (worst, (double) std::abs (o.getSample (ch, i) - base.getSample (ch, i)));
        return worst;
    };
    for (int block : { 1, 7, 64, 4096 })
    {
        PluginProcessor p; apply (p, s); prepare (p, 2, sr, block);
        check (differs (render (p, in, block)) == 0.0, "block size " + std::to_string (block) + " gives the same output as 512");
    }
    {
        PluginProcessor p; apply (p, s); prepare (p, 2, sr, 512);
        FakeHead head; p.setPlayHead (&head);
        check (differs (render (p, in, 512)) == 0.0, "a playhead changes nothing");
        p.setPlayHead (nullptr);
    }
    {
        PluginProcessor p; apply (p, s); prepare (p, 2, sr, 512);
        render (p, in, 512);
        prepare (p, 2, 96000.0, 64);
        prepare (p, 2, sr, 512);
        check (differs (render (p, in, 512)) == 0.0, "prepare called again restarts cleanly, same bounce");
    }
}

// A sudden Tightness jump moves delays, so it bends the pitch for a moment. It must stay a bend, not a jump.
static void testTightnessGlide()
{
    const double sr = 48000.0;
    const int n = 48000 * 6;
    const auto in = wholeCycleSine (n, 48);
    double worst = 0, worstStep = 0;
    for (int v = 0; v < 3; ++v)
    {
        PluginProcessor p; apply (p, solo (v, 0.0f)); prepare (p, 1, sr, 256);
        auto out = render (p, in, 256, nullptr, [&] (int idx) { if (idx == 3 * 48000 / 256) setParam (p, "tightness", 100.0f); });
        std::vector<float> y (out.getReadPointer (0), out.getReadPointer (0) + n);
        worst = std::max (worst, trackSine (y, sr, 48).centsPeak());
        worstStep = std::max (worstStep, maxJump (y.data() + 48000, n - 48000));
    }
    check (worst <= 120.0, "Tightness jumped from 0 to 100: the bend stays under 120 cents, worst", worst, "cents");
    check (worstStep <= 0.5 * 2.0 * juce::MathConstants<double>::pi * 1000.0 / sr * 1.1, "Tightness jumped from 0 to 100: no sample step beyond the sine's own slope, largest", worstStep);
}

// Timing Amount and Pitch Amount: the two halves of the drift, each on its own.
static void testTimingAndPitchAmounts()
{
    const double sr = 48000.0;
    const int n = 48000 * 120;
    const auto in = wholeCycleSine (n, 48);
    const auto track = [&] (int v, float timing, float pitch)
    {
        Setup s = solo (v, 100.0f); s.timing = timing; s.pitch = pitch;
        return trackSine (runSolo (in, sr, s), sr, 48);
    };
    for (int v = 0; v < 3; ++v)
    {
        const auto tag = "Double " + std::to_string (v + 1) + " at Tightness 100: ";
        const auto normal = track (v, 100.0f, 100.0f), noTiming = track (v, 0.0f, 100.0f), bigTiming = track (v, 200.0f, 100.0f);
        const auto noPitch = track (v, 100.0f, 0.0f), bigPitch = track (v, 100.0f, 200.0f), neither = track (v, 0.0f, 0.0f);
        check (noTiming.timingPeak() <= 0.4 * normal.timingPeak(), tag + "Timing Amount 0, timing swing under 40 percent of normal", noTiming.timingPeak(), "ms");
        check (bigTiming.timingPeak() >= 1.6 * normal.timingPeak(), tag + "Timing Amount 200, timing swing at least 1.6 times normal", bigTiming.timingPeak(), "ms");
        check (noTiming.centsRms() >= 0.4 * normal.centsRms(), tag + "Timing Amount 0 keeps pitch drift, rms", noTiming.centsRms(), "cents");
        check (noPitch.centsRms() <= 0.9 * normal.centsRms(), tag + "Pitch Amount 0, pitch rms under 90 percent of normal, some stays because timing drift bends pitch too", noPitch.centsRms(), "cents");
        check (bigPitch.centsRms() >= 1.3 * normal.centsRms(), tag + "Pitch Amount 200, pitch rms at least 1.3 times normal", bigPitch.centsRms(), "cents");
        check (noPitch.timingPeak() >= 0.7 * normal.timingPeak(), tag + "Pitch Amount 0 keeps timing drift", noPitch.timingPeak(), "ms");
        check (neither.centsPeak() <= 0.1 && neither.timingPeak() <= 0.01, tag + "both at 0: no drift at all, pitch peak", neither.centsPeak(), "cents");
    }
    // Wide settings stay clear of the delay floor.
    const auto impulses = impulseTrain (n, 9600);
    Setup s = solo (0, 100.0f); s.timing = 200.0f; s.pitch = 200.0f;
    const auto ms = trackImpulses (runSolo (impulses, sr, s), sr, 9600);
    check (*std::min_element (ms.begin(), ms.end()) >= 12.0, "Timing and Pitch Amount at 200, Tightness 100: nearest double stays 12 ms or more behind", *std::min_element (ms.begin(), ms.end()), "ms");
}

static void testExtraDelay()
{
    const double sr = 48000.0;
    const int spacing = 24000, n = 48000 * 40;
    const auto in = impulseTrain (n, spacing);
    const auto mean = [&] (float extra)
    {
        Setup s = solo (0, 0.0f); s.extra = extra;
        const auto ms = trackImpulses (runSolo (in, sr, s), sr, spacing);
        double sum = 0; for (auto m : ms) sum += m;
        return sum / (double) ms.size();
    };
    const double none = mean (0.0f);
    for (float extra : { 25.0f, 50.0f, 100.0f })
    {
        const double moved = mean (extra) - none;
        check (std::abs (moved - extra) <= 0.5, "Extra Delay " + std::to_string ((int) extra) + " ms moves the double by", moved, "ms");
    }
    // The lead stays put.
    Setup s = solo (0, 0.0f); s.extra = 100.0f; s.lead = 0.0f;
    const auto out = runSolo (in, sr, s);
    check (std::abs (out[(size_t) spacing] - 1.0f) < 1.0e-4f, "Extra Delay leaves the lead at its own time");
    // The longest possible reach reads clean.
    Setup w = solo (2, 100.0f); w.extra = 100.0f; w.timing = 200.0f; w.pitch = 200.0f;
    const auto far = trackImpulses (runSolo (in, sr, w), sr, spacing);
    check (*std::max_element (far.begin(), far.end()) >= 150.0, "longest settings reach past 150 ms, longest", *std::max_element (far.begin(), far.end()), "ms");
}

// Lead Pan and Doubles Pan follow the equal power law within 0.2 dB.
static void testPans()
{
    const double sr = 48000.0;
    const int n = 48000 * 2, skip = 9600;
    const auto in = wholeCycleSine (n, 48, 0.25);
    const double inDb = toDb (rms (in.data() + skip, n - skip));
    double worstLead = 0, worstDoubles = 0;
    bool hardOk = true;
    for (float pan : { -100.0f, -50.0f, 0.0f, 50.0f, 100.0f })
    {
        const double th = pan * 0.01 * juce::MathConstants<double>::pi * 0.25;
        const double wantL = std::cos (th) - std::sin (th), wantR = std::cos (th) + std::sin (th);
        for (int which : { 0, 1 })
        {
            Setup s = which == 0 ? Setup {} : solo (0, 30.0f, 0.0f, 0.0f, 1);
            if (which == 0) { s.lead = 0.0f; s.leadPan = pan; } else s.doublesPan = pan;
            PluginProcessor p; apply (p, s); prepareMonoToStereo (p, sr, 512);
            auto out = render (p, in, 512);
            const double l = rmsDb (out, 0, skip, n - skip) - inDb, r = rmsDb (out, 1, skip, n - skip) - inDb;
            auto& worst = which == 0 ? worstLead : worstDoubles;
            if (wantL > 1.0e-6) worst = std::max (worst, std::abs (l - toDb (wantL))); else hardOk = hardOk && l < -100.0;
            if (wantR > 1.0e-6) worst = std::max (worst, std::abs (r - toDb (wantR))); else hardOk = hardOk && r < -100.0;
        }
    }
    check (worstLead <= 0.2, "Lead Pan follows the pan law at -100 to +100, worst error", worstLead, "dB");
    check (worstDoubles <= 0.2, "Doubles Pan follows the pan law at -100 to +100, worst error", worstDoubles, "dB");
    check (hardOk, "hard pans leave the far side silent, under -100 dB");

    // Doubles Pan moves the whole group. Width still spreads around it.
    Setup s; s.doubles = 2; s.width = 40.0f; s.doublesPan = 60.0f; s.d1 = s.d2 = 0.0f;
    std::vector<float> nz ((size_t) (48000 * 8)); noise (nz.data(), (int) nz.size(), 0.25);
    PluginProcessor p; apply (p, s); prepareMonoToStereo (p, sr, 512);
    auto out = render (p, nz, 512);
    const int m = out.getNumSamples();
    const double diff = rmsDb (out, 1, skip, m - skip) - rmsDb (out, 0, skip, m - skip);
    check (diff >= 3.0, "Doubles Pan +60 with Width 40 leans the pair to the right, right over left", diff, "dB");

    // Mono out ignores pans.
    Setup mono; mono.lead = 0.0f; mono.leadPan = 100.0f;
    PluginProcessor q; apply (q, mono); prepare (q, 1, sr, 512);
    auto mo = render (q, in, 512);
    check (std::abs (rmsDb (mo, 0, skip, n - skip) - inDb) <= 0.01, "mono out: Lead Pan has no effect on level");
}

// A formant shape over noise, with one clear peak near 2 kHz.
static std::vector<float> formantNoise (int n, double formantHz, double sr)
{
    std::vector<float> x ((size_t) n);
    noise (x.data(), n, 0.4, 11);
    const double w = twoPi * formantHz / sr, r = std::exp (-juce::MathConstants<double>::pi * 180.0 / sr);
    const double a1 = -2.0 * r * std::cos (w), a2 = r * r;
    double y1 = 0, y2 = 0;
    for (auto& s : x) { const double y = 0.06 * s - a1 * y1 - a2 * y2; y2 = y1; y1 = y; s = (float) y; }
    return x;
}

// Long average spectrum, its peak in Hz between lo and hi.
static double spectrumPeakHz (const std::vector<float>& x, int skip, double sr, double lo, double hi)
{
    const int order = 13, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2), avg ((size_t) size / 2 + 1, 0.0f);
    for (int start = skip; start + size <= (int) x.size(); start += size / 2)
    {
        for (int i = 0; i < size; ++i) buf[(size_t) i] = x[(size_t) (start + i)] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k <= size / 2; ++k) avg[(size_t) k] += buf[(size_t) k] * buf[(size_t) k];
    }
    // Smooth across bins: this is about the outline, not noise grain.
    int best = 0; double bestV = -1;
    for (int k = (int) (lo * size / sr); k <= (int) (hi * size / sr); ++k)
    {
        double s = 0; for (int j = -12; j <= 12; ++j) s += avg[(size_t) (k + j)];
        if (s > bestV) { bestV = s; best = k; }
    }
    return best * sr / size;
}

// A sawtooth like voice: many harmonics through two formants.
static std::vector<float> formantSaw (int n, double f0, double sr)
{
    std::vector<float> x ((size_t) n);
    double phase = 0;
    for (auto& s : x) { phase += f0 / sr; phase -= std::floor (phase); s = (float) (0.4 * (2.0 * phase - 1.0)); }
    for (double formant : { 700.0, 2400.0 })
    {
        const double w = twoPi * formant / sr, r = std::exp (-juce::MathConstants<double>::pi * 160.0 / sr);
        const double a1 = -2.0 * r * std::cos (w), a2 = r * r;
        double y1 = 0, y2 = 0;
        for (auto& s : x) { const double y = 0.1 * s - a1 * y1 - a2 * y2; y2 = y1; y1 = y; s = (float) y; }
    }
    double top = 0; for (auto s : x) top = std::max (top, (double) std::abs (s));
    for (auto& s : x) s = (float) (s * 0.5 / top);
    return x;
}

static void testTimbre()
{
    const double sr = 48000.0;
    const int n = 48000 * 12, skip = 9600;
    const auto in = formantNoise (n, 2000.0, sr);
    const double inPeak = spectrumPeakHz (in, 0, sr, 1000.0, 4500.0);
    info ("Timbre test signal peak", inPeak, "Hz");
    for (float amount : { 50.0f, 100.0f })
        for (int v = 0; v < 3; ++v)
        {
            Setup s = solo (v, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.timbre = amount;
            const auto out = runSolo (in, sr, s);
            const double peakHz = spectrumPeakHz (out, skip, sr, 1000.0, 4500.0);
            const double wanted = std::pow (2.0, amount * 0.01 * DuoEngine::timbreSemitones[(size_t) v] / 12.0);
            const double measured = peakHz / inPeak;
            check (std::abs (measured - wanted) <= 0.04, "Timbre Variation " + std::to_string ((int) amount) + ", double " + std::to_string (v + 1) + ": formant moves by " + std::to_string (wanted).substr (0, 5) + " within 0.04, measured", measured);
        }

    // The same move at the other sample rates, where the frame sizes differ.
    for (double rate : { 44100.0, 96000.0 })
    {
        const int len = (int) rate * 12;
        const auto x = formantNoise (len, 2000.0, rate);
        const double before = spectrumPeakHz (x, 0, rate, 1000.0, 4500.0);
        for (int v = 0; v < 2; ++v)
        {
            Setup s = solo (v, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.timbre = 100.0f;
            const double measured = spectrumPeakHz (runSolo (x, rate, s), (int) (0.2 * rate), rate, 1000.0, 4500.0) / before;
            const double wanted = std::pow (2.0, DuoEngine::timbreSemitones[(size_t) v] / 12.0);
            check (std::abs (measured - wanted) <= 0.04, "Timbre Variation 100 at " + std::to_string ((int) rate) + " Hz, double " + std::to_string (v + 1) + ": formant moves by " + std::to_string (wanted).substr (0, 5) + " within 0.04, measured", measured);
        }
    }

    // Pitch stays where it was and level holds, on a voice shaped signal.
    const auto saw = formantSaw (n, 180.0, sr);
    const double sawDb = toDb (rms (saw.data() + skip, n - skip));
    for (int v = 0; v < 3; ++v)
    {
        Setup s = solo (v, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.timbre = 100.0f;
        const auto out = runSolo (saw, sr, s);
        const double level = toDb (rms (out.data() + skip, n - skip)) - sawDb;
        check (std::abs (level) <= 1.0, "Timbre Variation 100, double " + std::to_string (v + 1) + ": level change on a voice shape, limit 1 dB", level, "dB");

        const auto harmonic = [&] (const std::vector<float>& x)
        {
            double bestF = 0, bestV = -300;
            for (double f = 170.0; f <= 190.0; f += 0.05)
            {
                const double d = toneDb (x.data() + skip, n - skip, f, sr);
                if (d > bestV) { bestV = d; bestF = f; }
            }
            return bestF;
        };
        const double f = harmonic (out);
        check (std::abs (f - 180.0) <= 0.2, "Timbre Variation 100, double " + std::to_string (v + 1) + ": 180 Hz note stays there, measured", f, "Hz");
    }

    // Off means off: the path still measures as clean as a plain delay.
    const auto tone = wholeCycleSine (48000 * 20, 10);
    Setup off = solo (0, 100.0f);
    const auto clean = sineResidual (runSolo (tone, sr, off), sr, 10);
    check (clean.db <= -70.0, "Timbre Variation 0: 4800 Hz read noise stays under -70", clean.db, "dB");

    // Turned on and back off, it settles back to exactly the untouched path.
    {
        Setup s = solo (0, 0.0f); s.timing = 0.0f; s.pitch = 0.0f;
        PluginProcessor a, b; apply (a, s); apply (b, s); prepare (a, 1, sr, 256); prepare (b, 1, sr, 256);
        auto plain = render (a, saw, 256);
        auto moved = render (b, saw, 256, nullptr, [&] (int idx)
        {
            if (idx == 100) setParam (b, "timbre", 100.0f);
            if (idx == 600) setParam (b, "timbre", 0.0f);
        });
        double worst = 0;
        for (int i = 48000 * 8; i < n; ++i) worst = std::max (worst, (double) std::abs (plain.getSample (0, i) - moved.getSample (0, i)));
        check (worst <= 1.0e-5, "Timbre Variation turned up and back down returns to the untouched path, largest difference", worst);
    }
}

// A held note with vibrato, written directly.
static std::vector<float> vibratoNote (int n, double sr, double hz, double vibHz, double depthCents)
{
    std::vector<float> x ((size_t) n);
    double phase = 0;
    for (int i = 0; i < n; ++i)
    {
        const double cents = depthCents * std::sin (twoPi * vibHz * i / sr);
        phase += hz * std::pow (2.0, cents / 1200.0) / sr;
        phase -= std::floor (phase);
        double s = 0;
        for (int h = 1; h <= 5; ++h) s += std::sin (twoPi * h * phase) / h;
        x[(size_t) i] = (float) (0.3 * s);
    }
    return x;
}

// Vibrato depth of a note, in cents, from its zero crossings.
static double vibratoDepth (const std::vector<float>& y, double sr, double hz, double vibHz, int skip)
{
    // Follow the fundamental only.
    const double c = 1.0 - std::exp (-twoPi * hz * 1.5 / sr);
    double a = 0, b = 0;
    std::vector<double> times;
    double prev = 0;
    for (int i = skip; i < (int) y.size(); ++i)
    {
        a += c * ((double) y[(size_t) i] - a);
        b += c * (a - b);
        if (prev <= 0.0 && b > 0.0) times.push_back (i - 1 + (-prev) / (b - prev));
        prev = b;
    }
    double re = 0, im = 0; int count = 0;
    for (size_t k = 3; k + 3 < times.size(); ++k)
    {
        const double period = (times[k + 3] - times[k - 3]) / 6.0;
        const double cents = 1200.0 * std::log2 ((sr / period) / hz);
        const double w = twoPi * vibHz * times[k] / sr;
        re += cents * std::cos (w); im += cents * std::sin (w); ++count;
    }
    return 2.0 * std::sqrt (re * re + im * im) / std::max (count, 1);
}

static void testVibrato()
{
    const double sr = 48000.0;
    const int n = 48000 * 12, skip = 48000 * 2;
    const auto note = vibratoNote (n, sr, 220.0, 5.5, 50.0);
    const double input = vibratoDepth (note, sr, 220.0, 5.5, skip);
    info ("Vibrato test note depth", input, "cents");
    const auto depth = [&] (int v, float amount, const std::vector<float>& x, double hz, int range)
    {
        Setup s = solo (v, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.vibrato = amount; s.range = range;
        return vibratoDepth (runSolo (x, sr, s), sr, hz, 5.5, skip);
    };
    for (int v = 0; v < 3; ++v)
    {
        const auto tag = "Vibrato Variation, double " + std::to_string (v + 1) + ": ";
        const double off = depth (v, 0.0f, note, 220.0, 1);
        check (std::abs (off - input) <= 0.05 * input, tag + "at 0 the double has the lead's depth within 5 percent", off, "cents");
    }
    const double d1 = depth (0, 100.0f, note, 220.0, 1), d2 = depth (1, 100.0f, note, 220.0, 1), d3 = depth (2, 100.0f, note, 220.0, 1);
    check (d1 <= 0.5 * input, "Vibrato Variation 100, double 1 sings straighter: depth at most half the lead's", d1, "cents");
    check (d2 >= 1.25 * input, "Vibrato Variation 100, double 2 sings wider: depth at least 1.25 times the lead's", d2, "cents");
    check (d3 <= 0.8 * input, "Vibrato Variation 100, double 3 somewhat straighter: depth at most 0.8 times the lead's", d3, "cents");
    const double half = depth (0, 50.0f, note, 220.0, 1);
    check (half > d1 && half < input, "Vibrato Variation 50 falls between 0 and 100 on double 1", half, "cents");

    // The same at the other sample rates.
    for (double rate : { 44100.0, 96000.0 })
    {
        const int len = (int) rate * 12;
        const auto x = vibratoNote (len, rate, 220.0, 5.5, 50.0);
        const double before = vibratoDepth (x, rate, 220.0, 5.5, (int) rate * 2);
        Setup s = solo (0, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.vibrato = 100.0f;
        const double after = vibratoDepth (runSolo (x, rate, s), rate, 220.0, 5.5, (int) rate * 2);
        check (after <= 0.5 * before, "Vibrato Variation 100 at " + std::to_string ((int) rate) + " Hz, double 1: depth at most half the lead's", after, "cents");
    }

    // The speeds singers actually use, not only the middle one.
    for (double vibHz : { 4.5, 6.8 })
    {
        const auto x = vibratoNote (n, sr, 220.0, vibHz, 50.0);
        Setup s = solo (0, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.vibrato = 100.0f;
        const double before = vibratoDepth (x, sr, 220.0, vibHz, skip);
        const double after = vibratoDepth (runSolo (x, sr, s), sr, 220.0, vibHz, skip);
        check (after <= 0.7 * before, "Vibrato Variation 100, double 1, vibrato at " + std::to_string (vibHz).substr (0, 3) + " Hz: depth at most 0.7 of the lead's", after, "cents");
    }

    // Vocal Range decides what can be followed: a low note is out of reach for the Soprano setting.
    const auto low = vibratoNote (n, sr, 110.0, 5.5, 50.0);
    const double lowInput = vibratoDepth (low, sr, 110.0, 5.5, skip);
    const double lowSoprano = depth (0, 100.0f, low, 110.0, 0), lowBaritone = depth (0, 100.0f, low, 110.0, 2);
    check (std::abs (lowSoprano - lowInput) <= 0.1 * lowInput, "Vocal Range Soprano does not follow a 110 Hz note, depth stays", lowSoprano, "cents");
    check (lowBaritone <= 0.5 * lowInput, "Vocal Range Baritone or Bass follows a 110 Hz note, depth at most half", lowBaritone, "cents");

    // No vibrato to change means nothing to change: a steady note stays steady.
    const auto steady = wholeCycleSine (n, 218);
    Setup still = solo (0, 0.0f); still.timing = 0.0f; still.pitch = 0.0f; still.vibrato = 100.0f;
    const auto t = trackSine (runSolo (steady, sr, still), sr, 218, 2, 1.0);
    check (t.centsPeak() <= 1.0, "Vibrato Variation 100 on a steady note adds no movement, pitch peak", t.centsPeak(), "cents");

    // Noise has no pitch to follow.
    std::vector<float> nz ((size_t) n); noise (nz.data(), n, 0.25);
    Setup ns = solo (0, 0.0f); ns.vibrato = 100.0f;
    PluginProcessor p; apply (p, ns); prepare (p, 1, sr, 256);
    int voiced = 0, total = 0;
    render (p, nz, 256, nullptr, [&] (int idx) { if (idx > 200) { ++total; if (p.getPitchHz() > 0.0f) ++voiced; } });
    check ((double) voiced / total <= 0.05, "noise reads as no pitch, share of blocks with a pitch", (double) voiced / total);

    // Changing notes is not vibrato. Steps of a fifth every second, read from the delay the plug-in reports.
    {
        std::vector<float> steps ((size_t) n);
        double phase = 0;
        for (int i = 0; i < n; ++i)
        {
            const double hz = (i / 48000) % 2 == 0 ? 220.0 : 330.0;
            phase += hz / sr; phase -= std::floor (phase);
            steps[(size_t) i] = (float) (0.4 * std::sin (twoPi * phase));
        }
        Setup s = solo (0, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.vibrato = 100.0f;
        PluginProcessor q; apply (q, s); prepare (q, 1, sr, 64);
        double last = 0, worst = 0; bool have = false;
        render (q, steps, 64, nullptr, [&] (int idx)
        {
            const double d = q.getDelayMs (0);
            if (have && idx > 1000) worst = std::max (worst, std::abs (1731.2 * (d - last) * 0.001 / (64.0 / sr)));
            last = d; have = true;
        });
        check (worst <= 30.0, "note steps of a fifth with Vibrato Variation 100: bend added stays under 30 cents, worst", worst, "cents");
    }

    // A slide is slower than the jump detector, so the vibrato filter sees it. Three semitones in a quarter second, every second.
    {
        std::vector<float> slide ((size_t) n);
        double phase = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = std::fmod (i / sr, 2.0);
            const double cents = t < 1.0 ? juce::jlimit (0.0, 300.0, (t - 0.5) * 1200.0) : juce::jlimit (0.0, 300.0, 300.0 - (t - 1.5) * 1200.0);
            phase += 220.0 * std::pow (2.0, cents / 1200.0) / sr; phase -= std::floor (phase);
            slide[(size_t) i] = (float) (0.4 * std::sin (twoPi * phase));
        }
        Setup s = solo (0, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.vibrato = 100.0f;
        PluginProcessor q; apply (q, s); prepare (q, 1, sr, 64);
        double last = 0, worst = 0; bool have = false;
        render (q, slide, 64, nullptr, [&] (int idx)
        {
            const double d = q.getDelayMs (0);
            if (have && idx > 1000) worst = std::max (worst, std::abs (1731.2 * (d - last) * 0.001 / (64.0 / sr)));
            last = d; have = true;
        });
        check (worst <= 40.0, "slides of 3 semitones with Vibrato Variation 100: bend added stays under 40 cents, worst", worst, "cents");
    }
}

// ---------------------------------------------------------------- 0.4.0: Precise Vibrato, Mono Safe, older sessions

// Vibrato Variation with the doubles wandering as they do in use (the tests above hold them still), at the speeds singers use.
// On Luke's real takes the 0.3 law left double 1 with 0.90 to 0.94 of the lead's vibrato and gave double 2 1.06 to 1.08 times it.
// Limits from the claim: double 1 almost straight, double 2 half again as wide, double 3 half as wide, 50 half way.
static void testPreciseVibrato()
{
    const double sr = 48000.0;
    const int n = 48000 * 12, skip = 48000 * 2;
    const auto ratio = [&] (int v, float loose, double vibHz, bool precise, float amount)
    {
        const auto x = vibratoNote (n, sr, 220.0, vibHz, 50.0);
        Setup s = solo (v, loose); s.vibrato = amount; // Timing and Pitch Amount stay at 100: the double wanders
        PluginProcessor p; apply (p, s); setParam (p, "precisevibrato", precise ? 1.0f : 0.0f);
        prepare (p, 1, sr, 512);
        auto out = render (p, x, 512);
        const std::vector<float> y (out.getReadPointer (0), out.getReadPointer (0) + n);
        return vibratoDepth (y, sr, 220.0, vibHz, skip) / vibratoDepth (x, sr, 220.0, vibHz, skip);
    };
    for (float loose : { 0.0f, 30.0f, 85.0f })
        for (double vibHz : { 4.5, 5.5, 6.8 })
        {
            const auto tag = "Precise Vibrato, Tightness " + std::to_string ((int) loose) + ", vibrato at " + std::to_string (vibHz).substr (0, 3) + " Hz, wandering: ";
            const double d1 = ratio (0, loose, vibHz, true, 100.0f);
            check (d1 <= 0.35, tag + "double 1 keeps at most 0.35 of the lead's depth", d1);
            info (tag + "the 0.3 law, double 1 keeps", ratio (0, loose, vibHz, false, 100.0f));
            const double d2 = ratio (1, loose, vibHz, true, 100.0f);
            check (d2 >= 1.35 && d2 <= 1.65, tag + "double 2 sings 1.35 to 1.65 times as wide", d2);
            info (tag + "the 0.3 law, double 2 sings", ratio (1, loose, vibHz, false, 100.0f));
        }
    const double d3 = ratio (2, 30.0f, 5.5, true, 100.0f);
    check (d3 >= 0.35 && d3 <= 0.65, "Precise Vibrato, double 3 sings 0.35 to 0.65 times as wide", d3);
    const double half = ratio (0, 30.0f, 5.5, true, 50.0f);
    check (half >= 0.35 && half <= 0.65, "Precise Vibrato, Vibrato Variation 50: double 1 keeps 0.35 to 0.65 of the depth", half);
    const double off = ratio (0, 30.0f, 5.5, true, 0.0f);
    check (std::abs (off - 1.0) <= 0.05, "Precise Vibrato, Vibrato Variation 0: double 1 has the lead's depth within 5 percent", off);

    // Vibrato that comes in half way through each note, as singers do it: straight for a second, vibrato for a second.
    {
        std::vector<float> x ((size_t) n);
        double phase = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = std::fmod (i / sr, 2.0);
            const double depth = t < 1.0 ? 0.0 : 50.0 * juce::jmin (1.0, (t - 1.0) / 0.15);
            phase += 220.0 * std::pow (2.0, depth * std::sin (twoPi * 5.5 * t) / 1200.0) / sr; phase -= std::floor (phase);
            double v = 0; for (int h = 1; h <= 5; ++h) v += std::sin (twoPi * h * phase) / h;
            x[(size_t) i] = (float) (0.3 * v);
        }
        Setup s = solo (0, 30.0f); s.timing = 0.0f; s.pitch = 0.0f; s.vibrato = 100.0f;
        PluginProcessor q; apply (q, s); prepare (q, 1, sr, 64);
        double last = 0, worstStraight = 0, sumBend = 0, sumVib = 0; bool have = false;
        render (q, x, 64, nullptr, [&] (int idx)
        {
            const double d = q.getDelayMs (0), t = std::fmod (idx * 64.0 / sr, 2.0);
            const double cents = have ? -1731.2 * (d - last) * 0.001 / (64.0 / sr) : 0.0; // what the bend adds to the double's pitch
            last = d; have = true;
            if (idx * 64.0 / sr < 2.0) return;
            if (t > 0.45 && t < 0.95) worstStraight = std::max (worstStraight, std::abs (cents));
            if (t > 1.5 && t < 1.95)
            {
                // The double plays what the lead sang one delay ago: its vibrato then, and the bend now, should cancel.
                const double then = t - d * 0.001;
                const double vib = 50.0 * std::sin (twoPi * 5.5 * then);
                sumBend += (vib + cents) * (vib + cents); sumVib += vib * vib;
            }
        });
        check (worstStraight <= 5.0, "vibrato that comes and goes: on the straight part the bend adds at most 5 cents", worstStraight, "cents");
        const double left = std::sqrt (sumBend / std::max (sumVib, 1.0e-9));
        check (left <= 0.35, "vibrato that comes and goes: half a second after it starts, double 1 keeps at most 0.35 of it", left);
    }
}

// Mono Safe: the doubles sit on left and right with opposite signs, so left plus right is the lead alone.
static void testMonoSafe()
{
    const double sr = 48000.0;
    const int n = 48000 * 4, skip = 9600;
    std::vector<float> in ((size_t) n); noise (in.data(), n, 0.2);
    for (int preset : { 0, 3, 4, 6 })
    {
        PluginProcessor p; p.setCurrentProgram (preset); setParam (p, "monosafe", 1.0f); setParam (p, "matchlevel", 0.0f); // the lead at its own level
        const auto name = p.getProgramName (preset).toStdString();
        prepareMonoToStereo (p, sr, 256);
        auto out = render (p, in, 256);
        const double leadGain = DuoEngine::levelToGain (p.apvts.getRawParameterValue ("lead")->load());
        const double mono = leadGain * std::cos (p.apvts.getRawParameterValue ("leadpan")->load() * 0.01 * juce::MathConstants<double>::pi * 0.25);
        double left = 0, whole = 0, side = 0, wanted = 0;
        for (int v = 0; v < juce::roundToInt (p.apvts.getRawParameterValue ("doubles")->load()); ++v)
        {
            const double g = DuoEngine::levelToGain (p.apvts.getRawParameterValue (v == 0 ? "double1" : v == 1 ? "double2" : "double3")->load());
            wanted += g * g;
        }
        for (int i = skip; i < n; ++i)
        {
            const double l = out.getSample (0, i), r = out.getSample (1, i), x = mono * in[(size_t) i];
            left += (0.5 * (l + r) - x) * (0.5 * (l + r) - x); whole += x * x;
            side += 0.25 * (l - r) * (l - r);
        }
        const double db = 10.0 * std::log10 ((left + 1.0e-40) / (whole + 1.0e-40));
        check (db <= -100.0, "Mono Safe, " + name + ": left plus right is the lead alone, what is left", db, "dB");
        // The lead's own left minus right (when it is panned) is small next to the doubles here; noise through different delays adds in power.
        double inPower = 0; for (int i = skip; i < n; ++i) inPower += (double) in[(size_t) i] * in[(size_t) i];
        const double leadSide = leadGain * std::sin (p.apvts.getRawParameterValue ("leadpan")->load() * 0.01 * juce::MathConstants<double>::pi * 0.25);
        const double sideDb = 10.0 * std::log10 (side / ((wanted + leadSide * leadSide) * inPower));
        check (std::abs (sideDb) <= 1.0, "Mono Safe, " + name + ": the doubles are all there in left minus right, against their levels", sideDb, "dB");
    }
    // The level does not change when it is switched on.
    {
        double level[2] = {};
        for (int on = 0; on < 2; ++on)
        {
            PluginProcessor p; setParam (p, "monosafe", (float) on);
            prepareMonoToStereo (p, sr, 256);
            auto out = render (p, in, 256);
            double e = 0;
            for (int i = skip; i < n; ++i) e += (double) out.getSample (0, i) * out.getSample (0, i) + (double) out.getSample (1, i) * out.getSample (1, i);
            level[on] = 10.0 * std::log10 (e);
        }
        check (std::abs (level[1] - level[0]) <= 0.3, "Mono Safe on against off at the default: level of left and right together", level[1] - level[0], "dB");
    }
    // One output has no left and right to cancel between: nothing changes.
    {
        std::vector<float> a, b;
        for (int on = 0; on < 2; ++on)
        {
            PluginProcessor p; setParam (p, "monosafe", (float) on);
            prepare (p, 1, sr, 256);
            auto out = render (p, in, 256);
            (on ? b : a).assign (out.getReadPointer (0), out.getReadPointer (0) + n);
        }
        check (a == b, "Mono Safe with one output changes nothing, bit for bit");
    }
    // Bypass stays the exact dry signal.
    {
        PluginProcessor p; setParam (p, "monosafe", 1.0f); setParam (p, "bypass", 1.0f);
        prepareMonoToStereo (p, sr, 256);
        auto out = render (p, in, 256);
        double worst = 0;
        for (int i = skip; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch) worst = std::max (worst, (double) std::abs (out.getSample (ch, i) - in[(size_t) i]));
        check (worst == 0.0, "Mono Safe with Bypass on: exact dry on both sides, largest difference", worst);
    }
    // Off, which is how every session saved so far loads, is the sound as it was: covered bit for bit by the null test.
}

// Match Level: the lead and the doubles turned down together, so the output is as loud as the input.
static void testMatchLevel()
{
    const double sr = 48000.0;
    const int n = 48000 * 4, skip = 9600;
    std::vector<float> in ((size_t) n); noise (in.data(), n, 0.2);
    double inPower = 0; for (int i = skip; i < n; ++i) inPower += (double) in[(size_t) i] * in[(size_t) i];
    PluginProcessor names;
    for (int preset = 0; preset < names.getNumPrograms(); ++preset)
    {
        double level[2] = {};
        juce::AudioBuffer<float> outs[2];
        for (int on = 0; on < 2; ++on)
        {
            PluginProcessor p; p.setCurrentProgram (preset); setParam (p, "matchlevel", (float) on);
            prepareMonoToStereo (p, sr, 256);
            outs[on] = render (p, in, 256);
            double e = 0;
            for (int i = skip; i < n; ++i) e += 0.5 * ((double) outs[on].getSample (0, i) * outs[on].getSample (0, i) + (double) outs[on].getSample (1, i) * outs[on].getSample (1, i));
            level[on] = 10.0 * std::log10 (e / inPower);
        }
        const auto name = names.getProgramName (preset).toStdString();
        check (std::abs (level[1]) <= 0.5, "Match Level, " + name + ": left and right together are as loud as the input within 0.5 dB (off: " + std::to_string (level[0]).substr (0, 5) + " dB)", level[1], "dB");
        // The same sound at another level: every sample is the off render times one factor.
        const double k = std::pow (10.0, (level[1] - level[0]) / 20.0);
        double worst = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = skip; i < n; ++i) worst = std::max (worst, std::abs ((double) outs[1].getSample (ch, i) - k * outs[0].getSample (ch, i)));
        check (worst <= 2.0e-4, "Match Level, " + name + ": the same sound turned down, nothing else changed, largest difference", worst);
    }
    // One output.
    {
        PluginProcessor p; setParam (p, "matchlevel", 1.0f); prepare (p, 1, sr, 256);
        auto out = render (p, in, 256);
        double e = 0; for (int i = skip; i < n; ++i) e += (double) out.getSample (0, i) * out.getSample (0, i);
        check (std::abs (10.0 * std::log10 (e / inPower)) <= 0.5, "Match Level with one output: as loud as the input within 0.5 dB", 10.0 * std::log10 (e / inPower), "dB");
    }
    // Bypass stays the exact dry signal, and with everything off there is nothing to match.
    {
        PluginProcessor p; setParam (p, "matchlevel", 1.0f); setParam (p, "bypass", 1.0f);
        prepareMonoToStereo (p, sr, 256);
        auto out = render (p, in, 256);
        double worst = 0;
        for (int i = skip; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch) worst = std::max (worst, (double) std::abs (out.getSample (ch, i) - in[(size_t) i]));
        check (worst == 0.0, "Match Level with Bypass on: exact dry on both sides, largest difference", worst);
        Setup s; s.doubles = 2; // lead and doubles all off
        PluginProcessor q; apply (q, s); setParam (q, "matchlevel", 1.0f); prepareMonoToStereo (q, sr, 256);
        auto silent = render (q, in, 256);
        check (countNonFinite (silent) == 0 && peak (silent.getReadPointer (0) + skip, n - skip) == 0.0, "Match Level with every voice off: silence, nothing blows up");
    }
    // Mono Safe and Match Level together: left plus right is still the lead alone, at the matched level.
    {
        PluginProcessor p; setParam (p, "matchlevel", 1.0f); setParam (p, "monosafe", 1.0f);
        prepareMonoToStereo (p, sr, 256);
        auto out = render (p, in, 256);
        const double k = 1.0 / std::sqrt (1.0 + 2.0 * std::pow (10.0, -6.0 / 10.0)); // the default: lead 0 dB, two doubles at -6 dB
        double left = 0, whole = 0;
        for (int i = skip; i < n; ++i) { const double m = 0.5 * ((double) out.getSample (0, i) + out.getSample (1, i)), x = k * in[(size_t) i]; left += (m - x) * (m - x); whole += x * x; }
        check (10.0 * std::log10 ((left + 1.0e-40) / whole) <= -100.0, "Mono Safe with Match Level: left plus right is the lead alone at the matched level, what is left", 10.0 * std::log10 ((left + 1.0e-40) / whole), "dB");
    }
}

// Sessions saved before 0.4.0 have no entry for the three new controls. They keep the vibrato they were saved with.
static void testOlderSessions()
{
    const auto stateWithout04 = [] (PluginProcessor& from)
    {
        juce::MemoryBlock full; from.getStateInformation (full);
        auto xml = juce::AudioProcessor::getXmlFromBinary (full.getData(), (int) full.getSize());
        for (int i = xml->getNumChildElements(); --i >= 0;)
        {
            const auto id = xml->getChildElement (i)->getStringAttribute ("id");
            if (id == "precisevibrato" || id == "monosafe" || id == "matchlevel") xml->removeChildElement (xml->getChildElement (i), true);
        }
        juce::MemoryBlock old; juce::AudioProcessor::copyXmlToBinary (*xml, old);
        return old;
    };
    const auto flag = [] (PluginProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load() >= 0.5f; };

    PluginProcessor fresh;
    check (flag (fresh, "precisevibrato") && ! flag (fresh, "monosafe") && flag (fresh, "matchlevel"), "a new Duo opens with Precise Vibrato on, Mono Safe off and Match Level on");

    PluginProcessor saved; setParam (saved, "vibrato", 60.0f); setParam (saved, "tightness", 55.0f); setParam (saved, "monosafe", 1.0f); setParam (saved, "matchlevel", 1.0f);
    const auto old = stateWithout04 (saved);
    PluginProcessor loaded; loaded.setStateInformation (old.getData(), (int) old.getSize());
    check (! flag (loaded, "precisevibrato") && ! flag (loaded, "monosafe") && ! flag (loaded, "matchlevel"), "a session saved before 0.4.0 loads with Precise Vibrato, Mono Safe and Match Level off");
    check (std::abs (loaded.apvts.getRawParameterValue ("vibrato")->load() - 60.0f) < 0.06f && std::abs (loaded.apvts.getRawParameterValue ("tightness")->load() - 55.0f) < 0.06f,
           "and keeps its other settings");

    juce::MemoryBlock now; saved.getStateInformation (now);
    PluginProcessor again; again.setStateInformation (now.getData(), (int) now.getSize());
    check (flag (again, "precisevibrato") && flag (again, "monosafe") && flag (again, "matchlevel"), "a session saved by 0.4.0 keeps all three switches as saved");

    for (int i = 0; i < fresh.getNumPrograms(); ++i)
    {
        PluginProcessor q; setParam (q, "monosafe", 1.0f); setParam (q, "matchlevel", i == 0 ? 0.0f : 1.0f); q.setCurrentProgram (i);
        check (flag (q, "precisevibrato") && ! flag (q, "monosafe") && flag (q, "matchlevel") == (i == 0),
               "preset " + q.getProgramName (i).toStdString() + (i == 0 ? ": Precise Vibrato on, Mono Safe off, Match Level on" : ": Precise Vibrato on, Mono Safe and Match Level off"));
    }

    // The sound of an old session: Wide Stack's settings arriving as a pre 0.4.0 state give the same samples as the preset with
    // Precise Vibrato switched off, which is how the preset was until its vibrato was moved to the precise law.
    {
        const double sr = 48000.0;
        const int n = 48000 * 3;
        const auto x = vibratoNote (n, sr, 196.0, 5.2, 40.0);
        PluginProcessor a; a.setCurrentProgram (3); setParam (a, "precisevibrato", 0.0f);
        PluginProcessor source; source.setCurrentProgram (3);
        const auto wide = stateWithout04 (source);
        PluginProcessor b; b.setStateInformation (wide.getData(), (int) wide.getSize());
        prepareMonoToStereo (a, sr, 256); prepareMonoToStereo (b, sr, 256);
        auto ya = render (a, x, 256), yb = render (b, x, 256);
        bool same = true;
        for (int ch = 0; ch < 2 && same; ++ch) same = std::memcmp (ya.getReadPointer (ch), yb.getReadPointer (ch), sizeof (float) * (size_t) n) == 0;
        check (same, "an old session with Wide Stack's settings sounds as the preset did before it used Precise Vibrato, bit for bit");
        setParam (b, "precisevibrato", 1.0f);
        prepareMonoToStereo (b, sr, 256);
        auto yc = render (b, x, 256);
        bool differs = false;
        for (int i = 0; i < n && ! differs; ++i) differs = ya.getSample (0, i) != yc.getSample (0, i);
        check (differs, "switching Precise Vibrato on changes what Vibrato Variation does");
    }
}

// What a later window will read agrees with the audio.
static void testReadings()
{
    const double sr = 48000.0;
    const auto in = wholeCycleSine (48000 * 3, 218);
    Setup s = solo (1, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.extra = 10.0f;
    PluginProcessor p; apply (p, s); prepare (p, 1, sr, 256);
    render (p, in, 256);
    const auto ms = trackImpulses (runSolo (impulseTrain (48000 * 3, 24000), sr, s), sr, 24000);
    check (std::abs (p.getDelayMs (1) - ms.back()) <= 0.1, "delay reading matches the delay measured from the audio, reading", p.getDelayMs (1), "ms");
    check (std::abs (p.getPitchHz() - sr / 218.0) <= 1.0, "pitch reading matches a 220 Hz note, reading", p.getPitchHz(), "Hz");
}

static void testPresets()
{
    const double sr = 48000.0;
    const int n = 48000 * 3, skip = 9600;
    std::vector<float> in ((size_t) n); noise (in.data(), n, 0.1);
    PluginProcessor p;
    juce::MemoryBlock fresh; p.getStateInformation (fresh);
    p.setCurrentProgram (0);
    juce::MemoryBlock first; p.getStateInformation (first);
    check (fresh == first, "first preset equals the default state");
    for (int i = 0; i < p.getNumPrograms(); ++i)
    {
        PluginProcessor q; q.setCurrentProgram (i);
        prepareMonoToStereo (q, sr, 512);
        auto out = render (q, in, 512);
        const double l = rmsDb (out, 0, skip, n - skip), r = rmsDb (out, 1, skip, n - skip);
        const auto name = q.getProgramName (i).toStdString();
        check (countNonFinite (out) == 0 && l > -30.0 && r > -30.0 && std::abs (l - r) <= 1.0, "preset " + name + " sounds, balanced, left", l, "dB");
    }
}

static void testCpu()
{
    const double sr = 96000.0;
    const int n = (int) sr * 10;
    std::vector<float> in ((size_t) n); noise (in.data(), n, 0.25);
    Setup s; s.doubles = 3; s.loose = 100.0f; s.width = 50.0f; s.lead = 0.0f; s.d1 = s.d2 = s.d3 = -6.0f;
    PluginProcessor p; apply (p, s); prepare (p, 2, sr, 256);
    const auto start = std::chrono::steady_clock::now();
    auto out = render (p, in, 256);
    const double took = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    check (took / 10.0 <= 0.1, "worst case at 96 kHz, 3 doubles with split heads, share of one core", took / 10.0);
}

// ---------------------------------------------------------------- 0.4.3: the Phase 2 bug check

// The bend Precise Vibrato puts on double 1, in cents, read every 64 samples from the delay the plug-in reports, with the double held
// still otherwise. t is when each reading was taken. The engine's slow leak (1 s) pulls any bend back towards the double's mark, which
// by itself reads as up to 1731 x 3 ms / 1 s = 5.2 cents; vibrato is the bend with that taken out.
struct BendTrace { std::vector<double> t, cents, vibrato, delay; };

static BendTrace bendTrace (const std::vector<float>& x, double sr, float loose, const std::function<void (PluginProcessor&, double)>& atTime = {})
{
    Setup s = solo (0, loose); s.timing = 0.0f; s.pitch = 0.0f; s.vibrato = 100.0f;
    PluginProcessor q; apply (q, s); setParam (q, "precisevibrato", 1.0f); prepare (q, 1, sr, 64);
    const double mark = DuoEngine::restDelayMs (0, loose, 0.0, 0.0, 0.0);
    BendTrace tr;
    double last = 0; bool have = false;
    render (q, x, 64, nullptr, [&] (int idx)
    {
        const double t = idx * 64.0 / sr, d = q.getDelayMs (0);
        tr.t.push_back (t);
        tr.cents.push_back (have ? -1731.2 * (d - last) * 0.001 / (64.0 / sr) : 0.0);
        tr.vibrato.push_back (tr.cents.back() - 1731.2 * (d - mark) * 0.001 / 1.0);
        tr.delay.push_back (d);
        last = d; have = true;
        if (atTime) atTime (q, t);
    });
    return tr;
}

static double worstBend (const BendTrace& tr, double from, double to, bool leakTakenOut = false)
{
    double worst = 0;
    for (size_t i = 0; i < tr.t.size(); ++i)
        if (tr.t[i] >= from && tr.t[i] < to) worst = std::max (worst, std::abs (leakTakenOut ? tr.vibrato[i] : tr.cents[i]));
    return worst;
}

static double delayAt (const BendTrace& tr, double t)
{
    for (size_t i = 0; i < tr.t.size(); ++i)
        if (tr.t[i] >= t) return tr.delay[i];
    return tr.delay.back();
}

// A sung line written directly: cents (from 220 Hz) for each moment, five harmonics.
static std::vector<float> sungLine (int n, double sr, const std::function<double (double)>& centsAt)
{
    std::vector<float> x ((size_t) n);
    double phase = 0;
    for (int i = 0; i < n; ++i)
    {
        phase += 220.0 * std::pow (2.0, centsAt (i / sr) / 1200.0) / sr;
        phase -= std::floor (phase);
        double v = 0; for (int h = 1; h <= 5; ++h) v += std::sin (twoPi * h * phase) / h;
        x[(size_t) i] = (float) (0.3 * v);
    }
    return x;
}

// duo-r1-1. Vibrato into a note change or a slide, with Precise Vibrato on. The old note's vibrato belongs to the old note: once double 1
// plays the new note it gets no bend, and a fit that was refused is never applied. Limit from the straight-part test above: 5 cents.
static void testPreciseVibratoNoteChanges()
{
    const double sr = 48000.0;
    const int n = 48000 * 12;
    const double cycle = 1.1, jumpAt = 0.8;   // 0.8 s of a 50 cent 5.5 Hz vibrato on 220 Hz, then 330 Hz held straight; the jump lands on a new phase each time
    const auto jumps = sungLine (n, sr, [&] (double t)
    {
        return std::fmod (t, cycle) < jumpAt ? 50.0 * std::sin (twoPi * 5.5 * t) : 702.0;
    });
    for (float loose : { 30.0f, 85.0f })
    {
        const auto tr = bendTrace (jumps, sr, loose);
        double worst = 0, walk = 0;
        for (double at = cycle + jumpAt; at + 0.3 < n / sr; at += cycle)
        {
            worst = std::max (worst, worstBend (tr, at + 0.06, at + 0.29));
            walk = std::max (walk, std::abs (delayAt (tr, at + 0.29) - delayAt (tr, at)));
        }
        const auto tag = "Precise Vibrato, Tightness " + std::to_string ((int) loose) + ", vibrato then a jump up a fifth: ";
        check (worst <= 5.0, tag + "once double 1 sings the new note it gets no bend (60 to 290 ms after), worst", worst, "cents");
        info (tag + "largest delay walk over the 290 ms after the jump", walk, "ms");
    }

    // The same jump with Precise Vibrato switched off 0.3 s before it and on again 0.1 s after it, on the new straight note.
    {
        const auto tr = bendTrace (jumps, sr, 30.0f, [&] (PluginProcessor& q, double t)
        {
            const double inCycle = std::fmod (t, cycle);
            if (t > cycle && std::abs (inCycle - (jumpAt - 0.3)) < 0.0007) setParam (q, "precisevibrato", 0.0f);
            if (t > cycle && std::abs (inCycle - (jumpAt + 0.1)) < 0.0007) setParam (q, "precisevibrato", 1.0f);
        });
        double worst = 0;
        for (double at = cycle + jumpAt; at + 0.3 < n / sr; at += cycle)
            worst = std::max (worst, worstBend (tr, at + 0.1, at + 0.29));
        check (worst <= 5.0, "Precise Vibrato switched back on while a straight note plays: no bend from before it was switched off, worst", worst, "cents");
    }

    // A consonant in a vibrato: 50 ms of silence every 1.1 s, the vibrato going on in step across it. Too little of the window is voiced
    // to fit, and 0.4.2 then read the old sinusoid at one frozen moment, a one-sided push. Double 1 plays what the lead sang one delay
    // ago, so its vibrato and the bend should cancel; the bend must at least never leave more than doing nothing would (1.0).
    {
        const double gapCycle = 1.1, gapAt = 0.8, gapLength = 0.05;
        auto gaps = sungLine (n, sr, [&] (double t) { return 50.0 * std::sin (twoPi * 5.5 * t); });
        for (int i = 0; i < n; ++i)
        {
            const double u = std::fmod (i / sr, gapCycle);
            if (u >= gapAt && u < gapAt + gapLength) gaps[(size_t) i] = 0.0f;
        }
        const auto tr = bendTrace (gaps, sr, 30.0f);
        double left = 0, whole = 0;
        for (size_t i = 0; i < tr.t.size(); ++i)
        {
            const double t = tr.t[i], u = std::fmod (t, gapCycle);
            if (t < gapCycle || u < gapAt + gapLength + 0.036 || u > gapAt + gapLength + 0.25) continue;   // from when the double plays the voice again
            const double vib = 50.0 * std::sin (twoPi * 5.5 * (t - tr.delay[i] * 0.001));
            left += (vib + tr.cents[i]) * (vib + tr.cents[i]);
            whole += vib * vib;
        }
        const double share = std::sqrt (left / std::max (whole, 1.0e-9));
        check (share <= 1.0, "Precise Vibrato, a 50 ms consonant in a vibrato: in the 250 ms after it, double 1 keeps no more vibrato than with no bend", share);
    }

    // Vibrato, then an S shaped slide of 200 cents over 200 ms, then the note held straight. A double that only lost its vibrato
    // correction can be bent at most by that vibrato (50 cents) on the slide, and not at all once it holds the new note.
    {
        const double slideAt = 0.8, slideLength = 0.2, slideCycle = 1.4;
        const auto slides = sungLine (n, sr, [&] (double t)
        {
            const double u = std::fmod (t, slideCycle);
            if (u < slideAt) return 50.0 * std::sin (twoPi * 5.5 * t);
            const double a = juce::jlimit (0.0, 1.0, (u - slideAt) / slideLength);
            return 200.0 * a * a * (3.0 - 2.0 * a);
        });
        const auto tr = bendTrace (slides, sr, 30.0f);
        double onSlide = 0, after = 0;
        for (double at = slideCycle + slideAt; at + 0.55 < n / sr; at += slideCycle)
        {
            onSlide = std::max (onSlide, worstBend (tr, at + 0.06, at + slideLength + 0.06));
            after = std::max (after, worstBend (tr, at + slideLength + 0.06, at + 0.59, true));
        }
        check (onSlide <= 55.0, "Precise Vibrato, vibrato then a slide: on the slide double 1 is bent no more than its old vibrato, worst", onSlide, "cents");
        check (after <= 5.0, "Precise Vibrato, vibrato then a slide: once double 1 holds the new note it gets no vibrato bend (the leak taken out), worst", after, "cents");
    }
}

// duo-r1-2. The host's Reset (Logic sends one at the start of playback and before a bounce) clears what prepareToPlay clears.
static void testReset()
{
    const double sr = 48000.0;
    Setup s; s.doubles = 3; s.loose = 60.0f; s.width = 80.0f; s.lead = 0.0f; s.d1 = s.d2 = s.d3 = -6.0f; s.vibrato = 60.0f; s.timbre = 50.0f;
    const auto a = vibratoNote (48000 * 5, sr, 220.0, 5.5, 50.0), b = vibratoNote (48000 * 3, sr, 196.0, 5.2, 40.0);
    const auto largest = [] (const juce::AudioBuffer<float>& x, const juce::AudioBuffer<float>& y)
    {
        double worst = 0;
        for (int ch = 0; ch < x.getNumChannels(); ++ch)
            for (int i = 0; i < x.getNumSamples(); ++i) worst = std::max (worst, (double) std::abs (x.getSample (ch, i) - y.getSample (ch, i)));
        return worst;
    };

    {
        PluginProcessor fresh; apply (fresh, s); prepare (fresh, 2, sr, 512);
        const auto want = render (fresh, b, 512);
        PluginProcessor p; apply (p, s); prepare (p, 2, sr, 512);
        render (p, a, 512);
        p.reset();
        const double worst = largest (render (p, b, 512), want);
        check (worst == 0.0, "play, reset, play: the second part equals a new instance playing it, largest difference", worst);
    }
    {
        PluginProcessor p; apply (p, s); prepare (p, 2, sr, 512);
        std::vector<float> nz ((size_t) sr); noise (nz.data(), (int) sr, 0.25);
        render (p, nz, 512);
        p.reset();
        const auto out = render (p, std::vector<float> ((size_t) (sr * 0.3), 0.0f), 512);
        const double peak = std::max (out.getMagnitude (0, 0, out.getNumSamples()), out.getMagnitude (1, 0, out.getNumSamples()));
        check (peak == 0.0, "after a reset, silence in gives silence out: largest sample", peak);
        const long before = testkit::allocs.load();
        p.reset();
        const long made = testkit::allocs.load() - before;
        check (made == 0, "reset allocates nothing, allocations", (double) made);
    }
    {
        PluginProcessor never;
        never.reset();
        check (true, "a reset before the first prepare is harmless");
    }

    // JUCE's AU wrapper resets straight after the host turns its own bypass on. Clearing then would cut the doubles off at once instead
    // of the 20 ms fade to dry, so that one reset is left out.
    {
        std::vector<float> nz ((size_t) sr); noise (nz.data(), (int) sr, 0.25);
        PluginProcessor p, q; apply (p, s); apply (q, s); prepare (p, 2, sr, 256); prepare (q, 2, sr, 256);
        const auto withReset = render (p, nz, 256, nullptr, [&] (int idx) { if (idx == 100) { setParam (p, "bypass", 1.0f); p.reset(); } });
        const auto without = render (q, nz, 256, nullptr, [&] (int idx) { if (idx == 100) setParam (q, "bypass", 1.0f); });
        const double worst = largest (withReset, without);
        check (worst == 0.0, "the reset that comes with host bypass switching on changes nothing: the fade to dry goes on, largest difference", worst);
    }
    // A later reset while bypassed does clear: the doubles come back from empty lines when bypass goes off.
    {
        std::vector<float> nz ((size_t) sr); noise (nz.data(), (int) sr, 0.25);
        PluginProcessor p, q; apply (p, s); apply (q, s); prepare (p, 2, sr, 256); prepare (q, 2, sr, 256);
        const auto step = [&] (PluginProcessor& x, int idx, bool resetToo)
        {
            if (idx == 50) setParam (x, "bypass", 1.0f);
            if (idx == 100 && resetToo) x.reset();
            if (idx == 101) setParam (x, "bypass", 0.0f);
        };
        const auto withReset = render (p, nz, 256, nullptr, [&] (int idx) { step (p, idx, true); });
        const auto without = render (q, nz, 256, nullptr, [&] (int idx) { step (q, idx, false); });
        check (largest (withReset, without) > 0.0, "a reset later on while bypassed is not left out");
    }
}

// duo-r2-1. The Timbre spectrum frame is about 11 ms at every rate Logic offers, so its smoothers and lifter mean the same thing.
static void testHighRateTimbre()
{
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        const double ms = DuoEngine::spectrumFrameSamples (rate) * 1000.0 / rate;
        check (ms >= 10.5 && ms <= 12.0, "Timbre frame at " + std::to_string ((int) rate) + " Hz is 10.5 to 12 ms", ms, "ms");
    }
    for (double rate : { 176400.0, 192000.0 })
    {
        const int len = (int) rate * 6;
        const auto x = formantNoise (len, 2000.0, rate);
        const double before = spectrumPeakHz (x, 0, rate, 1000.0, 4500.0);
        for (int v = 0; v < 2; ++v)
        {
            Setup s = solo (v, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.timbre = 100.0f;
            const double measured = spectrumPeakHz (runSolo (x, rate, s), (int) (0.2 * rate), rate, 1000.0, 4500.0) / before;
            const double wanted = std::pow (2.0, DuoEngine::timbreSemitones[(size_t) v] / 12.0);
            check (std::abs (measured - wanted) <= 0.04, "Timbre Variation 100 at " + std::to_string ((int) rate) + " Hz, double " + std::to_string (v + 1) + ": formant moves by " + std::to_string (wanted).substr (0, 5) + " within 0.04, measured", measured);
        }
    }
    // Short-term level of double 1 on a steady voice shape, at 48 and 192 kHz. Only shown: the frame check above is the test.
    for (double rate : { 48000.0, 192000.0 })
    {
        const int len = (int) rate * 4, win = (int) (rate * 0.02);
        const auto saw = formantSaw (len, 180.0, rate);
        Setup s = solo (0, 0.0f); s.timing = 0.0f; s.pitch = 0.0f; s.timbre = 100.0f;
        const auto out = runSolo (saw, rate, s);
        std::vector<double> dbs;
        for (int at = (int) rate; at + win <= len; at += win) dbs.push_back (toDb (rms (out.data() + at, win)));
        double mean = 0; for (auto d : dbs) mean += d; mean /= (double) dbs.size();
        double var = 0; for (auto d : dbs) var += (d - mean) * (d - mean);
        info ("Timbre Variation 100 at " + std::to_string ((int) rate) + " Hz, double 1: level ripple over 20 ms windows", std::sqrt (var / (double) dbs.size()), "dB");
    }
}

// duo-r2-3. The chosen preset is saved with the session, only to name it.
static juce::File baselineFile (const juce::String& name) { return juce::File (juce::String (DUO_BASELINE)).getChildFile (name); }

static std::vector<float> readFloats (const juce::String& name)
{
    juce::MemoryBlock m;
    if (! baselineFile (name).loadFileAsData (m)) return {};
    const auto* f = static_cast<const float*> (m.getData());
    return { f, f + m.getSize() / sizeof (float) };
}

static void testSavedPreset()
{
    const auto programAfterReload = [] (PluginProcessor& from)
    {
        juce::MemoryBlock st; from.getStateInformation (st);
        PluginProcessor to; to.setStateInformation (st.getData(), (int) st.getSize());
        return to.getCurrentProgram();
    };
    {
        PluginProcessor a;
        check (a.getCurrentProgram() == 0, "a new Duo names Natural Pair, which is its default state", a.getCurrentProgram());
        a.setCurrentProgram (4);
        check (programAfterReload (a) == 4, "a session saved after choosing Loose Gang reloads naming it", programAfterReload (a));
        setParam (a, "width", 20.0f);
        juce::MemoryBlock st; a.getStateInformation (st);
        PluginProcessor b; b.setStateInformation (st.getData(), (int) st.getSize());
        check (std::abs (b.apvts.getRawParameterValue ("width")->load() - 20.0f) < 0.06f, "the preset is not applied again on reload: a Width changed after choosing it stays");
        check (! b.apvts.state.hasProperty ("program"), "the preset number stays out of the sound's value tree");
    }
    for (const juce::String bad : { "99", "-5", "x" })
    {
        PluginProcessor a; a.setCurrentProgram (2);
        juce::MemoryBlock st; a.getStateInformation (st);
        auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        xml->setAttribute ("program", bad);
        juce::MemoryBlock damaged; juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
        PluginProcessor b; b.setStateInformation (damaged.getData(), (int) damaged.getSize());
        check (juce::isPositiveAndBelow (b.getCurrentProgram(), b.getNumPrograms()), ("a damaged preset number (" + bad + ") is kept in range").toStdString(), b.getCurrentProgram());
    }

    // What the installed 0.4.2 really saved, read out of its Audio Unit through the host API (kAudioUnitProperty_ClassInfo, after
    // kAudioUnitProperty_PresentPreset for the two presets), and what that same Audio Unit played with it: tests/baseline/capture.mm.
    // It names no preset, since 0.4.2 did not save one, and sounds as it did.
    const auto in = readFloats ("input.f32");
    check (in.size() == 96000, "the 0.4.2 input file loads", (double) in.size());
    // Loose Gang uses Precise Vibrato, whose note change fix (duo-r1-1) changes what it plays on a sung line, so it is held to 0.4.2
    // with Precise Vibrato switched off in the state (a state made from the one 0.4.2 saved, played by 0.4.2), and only shown with it on.
    for (const juce::String name : { "v0.4.2-default", "v0.4.2-slapback-double", "v0.4.2-loose-gang-precise-off", "v0.4.2-loose-gang" })
    {
        juce::MemoryBlock state;
        const bool found = baselineFile (name + ".state").loadFileAsData (state);
        const auto want = readFloats (name + ".f32");
        PluginProcessor p;
        p.setStateInformation (state.getData(), (int) state.getSize());
        check (found && p.getCurrentProgram() < 0, (name + ": names no preset, since 0.4.2 did not save which one").toStdString(), p.getCurrentProgram());
        prepare (p, 2, 48000.0, 512);
        const auto out = render (p, in, 512);
        double worst = 0;
        const int n = (int) in.size();
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n && want.size() == (size_t) (2 * n); ++i)
                worst = std::max (worst, (double) std::abs (out.getSample (ch, i) - want[(size_t) (ch * n + i)]));
        const double db = want.size() == (size_t) (2 * n) ? 20.0 * std::log10 (worst + 1.0e-12) : 0.0;
        const auto what = (name + ": the session the installed 0.4.2 saved, against what 0.4.2 played with it, largest difference").toStdString();
        if (name == "v0.4.2-loose-gang")
            info (what + " (Precise Vibrato on, changed by the note change fix)", db, "dBFS");
        else
            check (db <= -90.0, what + " (limit -90)", db, "dBFS");
        info ((name + ": bit for bit the same (1 yes)").toStdString(), worst == 0.0 ? 1.0 : 0.0);
    }
}

// ---------------------------------------------------------------- 0.4.4: a vibrato that stops

// A held note on 220 Hz: 0.5 s straight, then a 50 cent 5.5 Hz vibrato (coming in over 0.15 s) for 11 to 14 half cycles, so it
// stops on the note's own pitch, going up on some notes and going down on others, then straight until the next one 2 s later.
static std::vector<float> vibratoThatStops (int n, double sr, std::vector<double>& stops)
{
    const double cycle = 2.0, startAt = 0.5;
    for (int c = 0; c * cycle < n / sr; ++c) stops.push_back (c * cycle + startAt + (11 + c % 4) / 11.0);
    return sungLine (n, sr, [&] (double t)
    {
        const int c = (int) (t / cycle);
        const double u = t - c * cycle - startAt, length = (11 + c % 4) / 11.0;
        if (u < 0.0 || u >= length) return 0.0;
        return 50.0 * juce::jmin (1.0, u / 0.15) * std::sin (twoPi * 5.5 * u);
    });
}

// What Precise Vibrato plays on vibratos that keep going, for the check that the end detection leaves them alone: the presets
// that use it (and the default with Vibrato Variation 100), a steady vibrato and one whose speed and depth wander, one output
// hash each (FNV-1a over the float bits, both channels).
static std::vector<std::pair<std::string, unsigned long long>> vibratoKeepsGoingHashes()
{
    const double sr = 48000.0;
    const int n = 48000 * 6;
    std::vector<std::pair<std::string, std::vector<float>>> inputs;
    inputs.push_back ({ "steady 5.5 Hz 50 cents", vibratoNote (n, sr, 220.0, 5.5, 50.0) });
    {
        std::vector<float> x ((size_t) n);
        double phase = 0, vphase = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            vphase += (5.5 + 0.6 * std::sin (twoPi * 0.23 * t)) / sr;
            const double cents = (45.0 + 15.0 * std::sin (twoPi * 0.31 * t)) * std::sin (twoPi * vphase);
            phase += 196.0 * std::pow (2.0, cents / 1200.0) / sr; phase -= std::floor (phase);
            double v = 0; for (int h = 1; h <= 5; ++h) v += std::sin (twoPi * h * phase) / h;
            x[(size_t) i] = (float) (0.3 * v);
        }
        inputs.push_back ({ "wandering 4.9 to 6.1 Hz, 30 to 60 cents", x });
    }
    std::vector<std::pair<std::string, unsigned long long>> out;
    for (const auto& [what, x] : inputs)
        for (int preset : { 0, 3, 4, 6 })
        {
            PluginProcessor p; p.setCurrentProgram (preset);
            if (preset == 0) setParam (p, "vibrato", 100.0f);
            prepare (p, 2, sr, 512);
            const auto y = render (p, x, 512);
            unsigned long long h = 1469598103934665603ULL;
            for (int ch = 0; ch < y.getNumChannels(); ++ch)
                for (int i = 0; i < y.getNumSamples(); ++i)
                {
                    const float f = y.getSample (ch, i);
                    std::uint32_t bits; std::memcpy (&bits, &f, 4);
                    for (int b = 0; b < 4; ++b) { h ^= (bits >> (8 * b)) & 0xffu; h *= 1099511628211ULL; }
                }
            out.push_back ({ p.getProgramName (preset).toStdString() + (preset == 0 ? " with Vibrato Variation 100" : "") + ", " + what, h });
        }
    return out;
}

// Printed by "PluginTest capture043" from the 0.4.3 source (commit 5c385f4, before the end detection), 2026-11-13.
static const std::vector<unsigned long long> vibratoKeepsGoing043 {
    10295906936432306570ULL, // Natural Pair with Vibrato Variation 100, steady 5.5 Hz 50 cents
    5450399555060282522ULL, // Wide Stack, steady 5.5 Hz 50 cents
    11879424939074985233ULL, // Loose Gang, steady 5.5 Hz 50 cents
    15467143996770683446ULL, // Two Singers, steady 5.5 Hz 50 cents
    4625644131910350435ULL, // Natural Pair with Vibrato Variation 100, wandering 4.9 to 6.1 Hz, 30 to 60 cents
    1952615143203945123ULL, // Wide Stack, wandering 4.9 to 6.1 Hz, 30 to 60 cents
    13056926286586896305ULL, // Loose Gang, wandering 4.9 to 6.1 Hz, 30 to 60 cents
    5103625773018715077ULL, // Two Singers, wandering 4.9 to 6.1 Hz, 30 to 60 cents
};

// Found in Phase 2 (duo-REPORT.md): when a vibrato stops on a held note, the fit window (1.5 cycles) went on accepting the vibrato
// until enough straight pitch had filled it, so double 1 was bent by a vibrato the lead no longer sang: 33 cents on 0.4.2 and 0.4.3.
// Limit from the straight-part test above: 5 cents, from 60 ms after the stop (as for a note change) to 0.6 s after.
static void testVibratoStops()
{
    const double sr = 48000.0;
    const int n = 48000 * 12;
    std::vector<double> stops;
    const auto x = vibratoThatStops (n, sr, stops);
    for (float loose : { 30.0f, 85.0f })
    {
        const auto tr = bendTrace (x, sr, loose);
        double worst = 0;
        for (double at : stops)
            if (at > 2.0 && at + 0.6 < n / sr) worst = std::max (worst, worstBend (tr, at + 0.06, at + 0.6));
        const auto tag = "Precise Vibrato, Tightness " + std::to_string ((int) loose) + ", a 50 cent 5.5 Hz vibrato that stops on a held note: ";
        check (worst <= 5.0, tag + "on the straight part after it double 1 gets no bend (60 ms to 0.6 s after), worst", worst, "cents");
    }
    const auto now = vibratoKeepsGoingHashes();
    for (size_t i = 0; i < now.size(); ++i)
        check (i < vibratoKeepsGoing043.size() && now[i].second == vibratoKeepsGoing043[i],
               "a vibrato that keeps going plays bit for bit as on 0.4.3: " + now[i].first);
}

static void testPhase2()
{
    testPreciseVibratoNoteChanges();
    testReset();
    testHighRateTimbre();
    testSavedPreset();
}

// The link (0.5.0), for Switchboard. With SMOOTH_LINK_DIR on a fresh scratch folder, the card lists every parameter
// by its ID, and a text command on a main control moves it and lands in done. The folder is removed afterwards and SMOOTH_LINK_DIR
// goes back to what it was, so the real link folder is never used.
static void testLink()
{
    const char* had = std::getenv ("SMOOTH_LINK_DIR");
    const std::string saved = had != nullptr ? had : "";
    const char* tmp = std::getenv ("TMPDIR");
    std::string folder = (tmp != nullptr && tmp[0] == '/') ? tmp : "/tmp";
    if (folder.back() != '/') folder += '/';
    folder += "duo-link.XXXXXX";
    const bool made = mkdtemp (folder.data()) != nullptr;
    setenv ("SMOOTH_LINK_DIR", folder.c_str(), 1);
    {
        PluginProcessor p;
        const auto& params = p.getParameters();
        const juce::String name = "1794430230000-test-a1b2c3";
        p.link.inbox().getChildFile (name + ".json").replaceWithText (R"({ "param": "tightness", "text": "60" })");
        p.link.pollNow();
        std::string bytes;
        juce::var card;
        const bool read = smooth::linkdetail::readFile (p.link.card().getFullPathName().toStdString(), bytes)
                          && juce::JSON::parse (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), card).wasOk();
        auto* listed = card["params"].getArray();
        bool ids = made && p.link.enabled() && read && listed != nullptr && listed->size() == params.size();
        juce::StringArray mains;
        for (int i = 0; ids && i < params.size(); ++i)
        {
            auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (params[i]);
            ids = hosted != nullptr && hosted->getParameterID().isNotEmpty() && (*listed)[i]["id"].toString() == hosted->getParameterID();
            if ((bool) (*listed)[i]["main"])
                mains.add ((*listed)[i]["id"].toString());
        }
        mains.sort (false);
        check (ids && mains == juce::StringArray { "double1", "double2", "double3", "lead", "tightness", "width" },
               "link: the card lists all " + std::to_string (params.size()) + " parameters by their IDs, in order, with Tightness, Width and the four levels as the main controls");
        juce::var done;
        if (auto* list = card["done"].getArray())
            for (auto& d : *list)
                if (d["cmd"].toString() == name)
                    done = d;
        const float tightness = p.apvts.getRawParameterValue ("tightness")->load();
        check (std::abs (tightness - 60.0f) < 1.0e-3f && (bool) done["ok"] && p.link.inbox().getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0,
               "link: the text command \"60\" on Tightness sets it and lands in done; Tightness now", tightness, "% loose");
    }
    if (had != nullptr) setenv ("SMOOTH_LINK_DIR", saved.c_str(), 1);
    else unsetenv ("SMOOTH_LINK_DIR");
    if (made) juce::File (folder).deleteRecursively();
}

#include "WindowTests.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc > 1 && juce::String (argv[1]) == "fingerprints")
    {
        nulltest::printFingerprints();
        return 0;
    }
    if (argc > 1 && juce::String (argv[1]) == "phase2")   // only the 0.4.3 bug check tests
    {
        testPhase2();
        testWindowPhase2();
        return finish();
    }
    if (argc > 1 && juce::String (argv[1]) == "round3")   // only the 0.4.4 check
    {
        testVibratoStops();
        return finish();
    }
    if (argc > 1 && juce::String (argv[1]) == "capture043")   // prints the hashes vibratoKeepsGoing043 holds; run once on 0.4.3
    {
        for (const auto& [what, h] : vibratoKeepsGoingHashes()) std::printf ("    %lluULL, // %s\n", h, what.c_str());
        return 0;
    }
    GenericOptions opt;
    opt.minPresets = 4;
    runGenericChecks ([] { return std::make_unique<PluginProcessor>(); }, opt);
    testPresets();
    testLatencyAndBypass();
    testLayouts();
    testDoublesCount();
    testDriftRanges();
    testLevels();
    testTightnessGlide();
    testTimingAndPitchAmounts();
    testExtraDelay();
    testPans();
    testTimbre();
    testVibrato();
    testPreciseVibrato();
    testMonoSafe();
    testMatchLevel();
    testOlderSessions();
    testReadings();
    testWidth();
    testDecorrelation();
    testArtifacts();
    testMethodChoice();
    testSampleRates();
    testRepeatable();
    testCpu();
    testPhase2();
    testVibratoStops();
    testWindow();
    testWindowPhase2();
    testLink();
    return finish();
}
