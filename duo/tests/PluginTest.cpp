// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <chrono>

using namespace testkit;

static constexpr double twoPi = juce::MathConstants<double>::twoPi;

// ---------------------------------------------------------------- set up

struct Setup
{
    int doubles = 1;
    float loose = 30.0f, width = 0.0f, lead = -60.0f, d1 = -60.0f, d2 = -60.0f, d3 = -60.0f;
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
    setParam (p, "bypass", 0.0f);
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
// noise and any zipper show up here. Returns dB against the carrier and the level ripple in dB, peak to peak.
struct Residual { double db = 0, rippleDb = 0; };
static Residual sineResidual (const std::vector<float>& x, double sr, int period, double skipSeconds = 0.2)
{
    // 1 ms phase track, a whole number of cycles per block.
    const int cycles = juce::jmax (1, (int) std::round (sr * 0.001 / period));
    auto t = trackSine (x, sr, period, cycles, skipSeconds);
    const double f = sr / period;
    const int hop = period * cycles;
    const int start0 = (int) (skipSeconds * sr) / period * period;
    // Savitzky-Golay smoothing keeps a curve's slope and bend, so a moving pitch is followed without error.
    static constexpr double sg[11] = { -36, 9, 44, 69, 84, 89, 84, 69, 44, 9, -36 };
    std::vector<double> smooth (t.delayMs.size(), 0.0);
    for (size_t k = 5; k + 5 < t.delayMs.size(); ++k)
    {
        double s = 0; for (int j = -5; j <= 5; ++j) s += sg[j + 5] * t.delayMs[k + (size_t) (j + 5) - 5];
        smooth[k] = s / 429.0;
    }
    // The block sum reports the phase at the centre of its block.
    const auto delayAt = [&] (double hopPos)
    {
        const auto k = (size_t) hopPos;
        const double u = hopPos - (double) k;
        const double p0 = smooth[k - 1], p1 = smooth[k], p2 = smooth[k + 1], p3 = smooth[k + 2];
        return p1 + 0.5 * u * (p2 - p0 + u * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + u * (3.0 * (p1 - p2) + p3 - p0)));
    };
    const int firstK = 8, lastK = (int) t.delayMs.size() - 9;
    const int from = start0 + firstK * hop + hop / 2, to = start0 + lastK * hop + hop / 2;
    const int n = to - from;
    std::vector<double> ref ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double hopPos = (double) (from + i - start0 - hop / 2) / hop;
        // Whole cycle phase keeps precision over long runs.
        ref[(size_t) i] = std::sin (twoPi * ((from + i) % period) / period - twoPi * f * delayAt (hopPos) * 0.001);
    }
    double xr = 0, rr = 0;
    for (int i = 0; i < n; ++i) { xr += (double) x[(size_t) (from + i)] * ref[(size_t) i]; rr += ref[(size_t) i] * ref[(size_t) i]; }
    const double a = xr / rr;
    double e = 0;
    for (int i = 0; i < n; ++i) { const double d = (double) x[(size_t) (from + i)] - a * ref[(size_t) i]; e += d * d; }
    Residual r;
    r.db = 10.0 * std::log10 (std::max (e / (a * a * rr), 1.0e-20));
    // Level, 5 ms at a time, measured against the reference so the moving pitch does not count as ripple.
    const int win = (int) (sr * 0.005) / period * period;
    double lo = 1.0e9, hi = 0;
    for (int w = 0; w + win <= n; w += win)
    {
        double s = 0, q = 0; for (int i = 0; i < win; ++i) { s += (double) x[(size_t) (from + w + i)] * ref[(size_t) (w + i)]; q += ref[(size_t) (w + i)] * ref[(size_t) (w + i)]; }
        const double level = s / q; lo = std::min (lo, level); hi = std::max (hi, level);
    }
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
        const auto shifted = sineResidual (twoHeadShifter (in, sr, 10.0, 40.0), sr, period);
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
    check (std::abs (PluginProcessor().getTailLengthSeconds() - 0.1) < 1.0e-9, "reports a 100 ms tail");
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

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    GenericOptions opt;
    opt.minPresets = 4;
    runGenericChecks ([] { return std::make_unique<PluginProcessor>(); }, opt);
    testPresets();
    testLatencyAndBypass();
    testLayouts();
    testDoublesCount();
    testDriftRanges();
    testLevels();
    testWidth();
    testDecorrelation();
    testArtifacts();
    testMethodChoice();
    testSampleRates();
    testRepeatable();
    testCpu();
    return finish();
}
