// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
// Pass limits were written down before the first run. Each one says what it holds the plug-in to.
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <complex>

using namespace testkit;

namespace
{
    struct Settings
    {
        float rate = 0.25f; bool sync = false; int division = 3;
        float depth = 2.5f, feedback = 0.0f, saturation = 0.0f, tone = 0.0f, spread = 0.0f, mix = 100.0f;
        bool bypass = false;
    };

    void setParam (PluginProcessor& p, const char* id, float plain)
    {
        auto* prm = p.apvts.getParameter (id);
        prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
    }

    std::unique_ptr<PluginProcessor> makeProc (const Settings& s, double sr = 48000.0, int channels = 2, int block = 512)
    {
        auto p = std::make_unique<PluginProcessor>();
        setParam (*p, "rate", s.rate);
        setParam (*p, "sync", s.sync ? 1.0f : 0.0f);
        setParam (*p, "division", (float) s.division);
        setParam (*p, "depth", s.depth);
        setParam (*p, "feedback", s.feedback);
        setParam (*p, "saturation", s.saturation);
        setParam (*p, "tone", s.tone);
        setParam (*p, "spread", s.spread);
        setParam (*p, "mix", s.mix);
        setParam (*p, "bypass", s.bypass ? 1.0f : 0.0f);
        prepare (*p, channels, sr, block);
        return p;
    }

    std::vector<float> noiseSignal (int n, double amp = 0.1, unsigned seed = 11)
    {
        std::vector<float> x ((size_t) n);
        noise (x.data(), n, amp, seed);
        return x;
    }

    // Noise with most of its energy under about 1.5 kHz. Its correlation peak is wide enough to read a moving delay
    // without the reading jumping between lags. White noise leaves a one sample wide peak that a sweep smears flat.
    std::vector<float> softNoiseSignal (int n, double sr, double amp = 0.1, unsigned seed = 17)
    {
        auto x = noiseSignal (n, amp, seed);
        const float c = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 1500.0 / sr));
        float a = 0.0f, b = 0.0f;
        for (auto& v : x) { a += c * (v - a); b += c * (a - b); v = b; }
        return x;
    }

    std::vector<float> sineSignal (int n, double freq, double sr, double amp)
    {
        std::vector<float> x ((size_t) n);
        sine (x.data(), n, freq, sr, amp);
        return x;
    }

    // Black box reading of the wet to dry delay: cross correlate one frame of the wet only output against the input.
    // Positive means the wet comes after the dry path, negative means it comes before it.
    double relDelayMs (const std::vector<float>& in, const float* out, int start, int frame, int latency, int maxLag, double sr)
    {
        double best = -1.0e30; int bestLag = 0;
        std::vector<double> c ((size_t) (2 * maxLag + 1));
        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            double s = 0;
            const float* y = out + start + latency + lag;
            const float* x = in.data() + start;
            for (int i = 0; i < frame; ++i) s += (double) x[i] * y[i];
            c[(size_t) (lag + maxLag)] = s;
            if (s > best) { best = s; bestLag = lag; }
        }
        double frac = 0;
        if (bestLag > -maxLag && bestLag < maxLag)
        {
            const double a = c[(size_t) (bestLag + maxLag - 1)], b = c[(size_t) (bestLag + maxLag)], d = c[(size_t) (bestLag + maxLag + 1)];
            const double den = a - 2.0 * b + d;
            if (std::abs (den) > 1.0e-12) frac = 0.5 * (a - d) / den;
        }
        return ((double) bestLag + frac) * 1000.0 / sr;
    }

    double medianOf (std::vector<double> v)
    {
        if (v.empty()) return 0.0;
        std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
        return v[v.size() / 2];
    }

    struct Series { std::vector<double> t, rel; double minV = 1e9, maxV = -1e9; };

    Series relSeries (const std::vector<float>& in, const float* out, int n, int latency, double sr, double depthMs, int frame, int hop, int from = 0)
    {
        Series s;
        const int maxLag = (int) std::ceil ((depthMs + 0.5) * 0.001 * sr);
        for (int start = from + maxLag; start + frame + latency + maxLag < n; start += hop)
        {
            const double r = relDelayMs (in, out, start, frame, latency, maxLag, sr);
            s.t.push_back ((start + frame * 0.5) / sr);
            s.rel.push_back (r);
            s.minV = std::min (s.minV, r);
            s.maxV = std::max (s.maxV, r);
        }
        return s;
    }

    // Rate from the rising zero crossings of a delay series. Readings go through a median of 5, and a crossing only counts
    // once the series has been well below zero and comes well above it, so one stray frame cannot add a crossing.
    double crossingRate (const Series& s, int* crossings = nullptr)
    {
        const size_t count = s.rel.size();
        std::vector<double> med (count);
        for (size_t i = 0; i < count; ++i)
        {
            const size_t a = i < 2 ? 0 : i - 2, b = std::min (count, i + 3);
            med[i] = medianOf (std::vector<double> (s.rel.begin() + (long) a, s.rel.begin() + (long) b));
        }
        const double swing = 0.25 * std::max (s.maxV, -s.minV);
        std::vector<double> times;
        bool armed = false;
        size_t lastNegative = 0;
        for (size_t i = 0; i + 1 < count; ++i)
        {
            if (med[i] < -swing) armed = true;
            if (med[i] < 0.0) lastNegative = i;
            if (armed && med[i] > swing)
            {
                const size_t k = lastNegative;
                times.push_back (s.t[k] + (s.t[k + 1] - s.t[k]) * (-med[k]) / (med[k + 1] - med[k]));
                armed = false;
            }
        }
        if (crossings != nullptr) *crossings = (int) times.size();
        if (times.size() < 2) return 0.0;
        return (double) (times.size() - 1) / (times.back() - times.front());
    }

    // Phase of the delay series at a known rate, in degrees
    double seriesPhaseDeg (const Series& s, double rate)
    {
        // Whole cycles only, or the unfinished part of a cycle pulls the answer away
        const double cycles = std::floor ((s.t.back() - s.t.front()) * rate);
        std::complex<double> sum = 0;
        for (size_t i = 0; i < s.rel.size() && s.t[i] - s.t.front() < cycles / rate; ++i)
            sum += s.rel[i] * std::polar (1.0, -juce::MathConstants<double>::twoPi * rate * s.t[i]);
        return std::arg (sum) * 180.0 / juce::MathConstants<double>::pi;
    }

    // Transfer function by averaging cross and power spectra over a few overlapping frames. Returns dB for each bin.
    std::vector<double> transferDb (const std::vector<float>& in, const float* out, int start, int latency, int order, int frames)
    {
        const int N = 1 << order, hop = N / 2;
        juce::dsp::FFT fft (order);
        std::vector<std::complex<double>> sxy ((size_t) (N / 2 + 1));
        std::vector<double> sxx ((size_t) (N / 2 + 1), 0.0);
        std::vector<float> a ((size_t) (2 * N)), b ((size_t) (2 * N));
        for (int f = 0; f < frames; ++f)
        {
            std::fill (a.begin(), a.end(), 0.0f);
            std::fill (b.begin(), b.end(), 0.0f);
            for (int i = 0; i < N; ++i)
            {
                const float w = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (N - 1)));
                a[(size_t) i] = w * in[(size_t) (start + f * hop + i)];
                b[(size_t) i] = w * out[start + f * hop + i + latency];
            }
            fft.performRealOnlyForwardTransform (a.data());
            fft.performRealOnlyForwardTransform (b.data());
            for (int k = 0; k <= N / 2; ++k)
            {
                const std::complex<double> X (a[(size_t) (2 * k)], a[(size_t) (2 * k + 1)]), Y (b[(size_t) (2 * k)], b[(size_t) (2 * k + 1)]);
                sxy[(size_t) k] += Y * std::conj (X);
                sxx[(size_t) k] += std::norm (X);
            }
        }
        std::vector<double> db ((size_t) (N / 2 + 1));
        for (int k = 0; k <= N / 2; ++k)
            db[(size_t) k] = toDb (std::abs (sxy[(size_t) k]) / std::max (sxx[(size_t) k], 1.0e-20));
        return db;
    }

    // First notch: the first dip above minHz that goes at least 10 dB under the median level. The whole dip is walked to its
    // deepest bin, so a ripple on the way down is not taken for the bottom.
    double firstNotchHz (const std::vector<double>& db, double binHz, double minHz, double maxHz)
    {
        const int top = (int) (maxHz / binHz);
        const double med = medianOf (std::vector<double> (db.begin() + 1, db.begin() + top));
        for (int k = std::max (2, (int) (minHz / binHz)); k < top; ++k)
            if (db[(size_t) k] < med - 10.0)
            {
                int deepest = k;
                for (int j = k; j < top && db[(size_t) j] < med - 6.0; ++j)
                    if (db[(size_t) j] < db[(size_t) deepest]) deepest = j;
                const double a = db[(size_t) deepest - 1], b = db[(size_t) deepest], c = db[(size_t) deepest + 1], den = a - 2.0 * b + c;
                return (deepest + (std::abs (den) > 1.0e-9 ? 0.5 * (a - c) / den : 0.0)) * binHz;
            }
        return -1.0;
    }

    double thdPercent (const float* x, int n, double f0, double sr)
    {
        const double fund = std::pow (10.0, toneDb (x, n, f0, sr) / 20.0);
        double sum = 0;
        for (int h = 2; h <= 9; ++h)
            if (f0 * h < sr * 0.45)
            {
                const double a = std::pow (10.0, toneDb (x, n, f0 * h, sr) / 20.0);
                sum += a * a;
            }
        return 100.0 * std::sqrt (sum) / std::max (fund, 1.0e-12);
    }

    double nullDb (const float* a, const float* b, int n, double refRms)
    {
        double s = 0;
        for (int i = 0; i < n; ++i) { const double d = (double) a[i] - b[i]; s += d * d; }
        return toDb (std::sqrt (s / std::max (n, 1)) / std::max (refRms, 1.0e-12));
    }

    struct TestHead : juce::AudioPlayHead
    {
        double bpm = 120.0, ppq = 0.0; bool playing = true, tempoKnown = true;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            if (tempoKnown) info.setBpm (bpm);
            info.setPpqPosition (ppq);
            info.setIsPlaying (playing);
            return info;
        }
    };

    // Renders while a playhead walks forward with the audio
    juce::AudioBuffer<float> renderWithHead (PluginProcessor& p, TestHead& head, const std::vector<float>& in, int block, double sr, double startPpq)
    {
        head.ppq = startPpq;
        p.setPlayHead (&head);
        auto out = render (p, in, block, nullptr, [&] (int idx) { head.ppq = startPpq + idx * (double) block / sr * head.bpm / 60.0; });
        p.setPlayHead (nullptr);
        return out;
    }
}

// Reported latency equals the measured delay of the dry path, at every rate, with Mix at 0 and with Bypass on.
static void testLatency()
{
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        const auto tag = " at " + std::to_string ((int) sr) + " Hz";
        for (bool bypassed : { false, true })
        {
            Settings s; s.mix = bypassed ? 100.0f : 0.0f; s.bypass = bypassed;
            auto p = makeProc (s, sr);
            const int n = (int) sr;
            auto in = noiseSignal (n);
            // Let the bypass settle, then read the delay
            auto out = render (*p, in, 512);
            const int latency = p->getLatencySamples();
            const double measuredMs = relDelayMs (in, out.getReadPointer (0), n / 2, n / 4, latency, 64, sr);
            const double measured = latency + measuredMs * 0.001 * sr;
            // Limit: within a twentieth of a sample
            check (std::abs (measured - latency) <= 0.05, std::string (bypassed ? "bypassed" : "Mix 0") + " dry delay" + tag + ", reported " + std::to_string (latency) + ", measured", measured, "samples");
        }
        Settings s;
        auto p = makeProc (s, sr);
        const double ms = 1000.0 * p->getLatencySamples() / sr;
        // Limit: 5.5 ms fixed delay plus at most 2 ms for the oversampling filters, the same in time at every rate
        check (ms >= 5.5 && ms <= 7.5, "latency in time" + tag, ms, "ms");
    }
    PluginProcessor fresh;
    prepare (fresh, 2, 48000.0, 512);
    const int at48 = fresh.getLatencySamples();
    prepare (fresh, 2, 96000.0, 64);
    const int at96 = fresh.getLatencySamples();
    prepare (fresh, 2, 48000.0, 512);
    check (fresh.getLatencySamples() == at48 && at96 != at48, "latency follows a second prepare", at48, "samples");
}

// The wet to dry delay truly passes through zero and goes negative.
static void testThroughZero()
{
    const double sr = 48000.0;
    Settings s; s.rate = 0.5f; s.depth = 3.0f;
    auto p = makeProc (s, sr);
    const int n = (int) (sr * 6.0);
    auto in = softNoiseSignal (n, sr);
    auto out = render (*p, in, 512);
    const auto series = relSeries (in, out.getReadPointer (0), n, p->getLatencySamples(), sr, 3.0, 1024, 480);
    // Limits: reaches within 5% of both ends, on both sides of zero
    check (series.maxV >= 2.85, "wet after dry, latest", series.maxV, "ms");
    check (series.minV <= -2.85, "wet before dry, earliest (negative is through zero)", series.minV, "ms");
    int crossings = 0;
    crossingRate (series, &crossings);
    // Limit: one rising crossing per cycle, 3 whole cycles rendered
    check (crossings >= 2, "rising zero crossings of the wet to dry delay", crossings);

    // Inverted wet at the crossing must cancel the dry path: this only happens if the two are truly aligned.
    Settings c; c.rate = 0.05f; c.depth = 0.2f; c.feedback = -0.1f; c.mix = 50.0f;
    auto q = makeProc (c, sr);
    const int m = (int) (sr * 25.0);
    auto x = noiseSignal (m);
    auto y = render (*q, x, 512);
    const double dryLevel = rms (x.data(), m);
    double deepest = 1000.0, shallowest = -1000.0;
    const int frame = 480;
    for (int start = 48000; start + frame + q->getLatencySamples() < m; start += frame)
    {
        const double db = toDb (rms (y.getReadPointer (0) + start + q->getLatencySamples(), frame) / dryLevel);
        deepest = std::min (deepest, db);
        shallowest = std::max (shallowest, db);
    }
    // Limits: at least 30 dB of broadband cancellation at the crossing, and well away from it the level comes back
    check (deepest <= -30.0, "inverted wet cancels dry at the crossing", deepest, "dB");
    check (shallowest >= -12.0, "away from the crossing the signal returns", shallowest, "dB");
}

// Notches move with the LFO and land where the measured delay says they should, on both sides of zero.
static void testNotches()
{
    const double sr = 48000.0;
    Settings wetS; wetS.rate = 0.01f; wetS.depth = 5.0f; wetS.mix = 100.0f;
    Settings mixS = wetS; mixS.mix = 50.0f;
    const int n = (int) (sr * 102.0);
    auto in = noiseSignal (n);          // white, to see notches across the whole band
    auto soft = softNoiseSignal (n, sr); // to read the delay itself
    auto pw = makeProc (wetS, sr);
    auto wet = render (*pw, soft, 512);
    auto pm = makeProc (mixS, sr);
    auto mixed = render (*pm, in, 512);
    const int latency = pm->getLatencySamples();

    const int order = 12, N = 1 << order;
    const int maxLag = (int) (5.5 * 0.001 * sr);
    std::vector<double> errPos, errNeg;
    double lowest = 1.0e9, highest = 0.0;
    for (int start = maxLag + 2048; start + 4 * N + latency + maxLag < n; start += 4800)
    {
        const int span = 2 * N;
        const double rel = relDelayMs (soft, wet.getReadPointer (0), start, span, latency, maxLag, sr);
        if (std::abs (rel) < 0.3)
            continue;
        // A longer frame for the wide delays: their first notch sits low, where a short frame cannot resolve it.
        // The delay barely moves out there, so the longer frame costs nothing.
        const int useOrder = std::abs (rel) >= 2.0 ? order + 1 : order;
        const auto db = transferDb (in, mixed.getReadPointer (0), start, latency, useOrder, 3);
        const double expected = 1000.0 / (2.0 * std::abs (rel));
        const double found = firstNotchHz (db, sr / (1 << useOrder), 40.0, 12000.0);
        const double err = found > 0 ? std::abs (found - expected) / expected * 100.0 : 100.0;
        (rel > 0 ? errPos : errNeg).push_back (err);
        if (found > 0) { lowest = std::min (lowest, found); highest = std::max (highest, found); }
    }
    const auto report = [] (std::vector<double>& e, const char* side)
    {
        const double worst = e.empty() ? 100.0 : *std::max_element (e.begin(), e.end());
        info (std::string ("frames measured, ") + side, (double) e.size());
        // Limits: typical notch within 2% of 1 / (2 x delay), worst frame within 8%
        check (! e.empty() && medianOf (e) <= 2.0, std::string ("first notch against measured delay, median error, ") + side, medianOf (e), "%");
        check (worst <= 8.0, std::string ("first notch against measured delay, worst error, ") + side, worst, "%");
    };
    report (errPos, "wet after dry");
    report (errNeg, "wet before dry");
    // Limit: depth 5 ms gives a notch at 100 Hz at the ends of the sweep, and over 1 kHz near zero
    check (lowest >= 80.0 && lowest <= 125.0, "lowest first notch in the sweep", lowest, "Hz");
    check (highest >= 900.0, "highest first notch in the sweep", highest, "Hz");

    // At the crossing (50 s into a 100 s cycle) the two paths line up: no notch in the band, the level is flat.
    // A short frame, because the delay keeps moving.
    double closest = 1.0e9; int bestStart = 0;
    for (int start = (int) (49.0 * sr); start < (int) (51.0 * sr); start += 960)
    {
        const double rel = relDelayMs (soft, wet.getReadPointer (0), start, 2048, latency, maxLag, sr);
        if (std::abs (rel) < closest) { closest = std::abs (rel); bestStart = start; }
    }
    info ("delay in the frame closest to the crossing", closest, "ms");
    const auto flat = transferDb (in, mixed.getReadPointer (0), bestStart, latency, 10, 3);
    double flatWorst = 0;
    const double flatBin = sr / 1024.0;
    for (int k = (int) (100.0 / flatBin); k < (int) (6000.0 / flatBin); ++k) flatWorst = std::max (flatWorst, std::abs (flat[(size_t) k]));
    // Limit: within 1 dB from 100 Hz to 6 kHz with the paths lined up
    check (flatWorst <= 1.0, "flat at the crossing, worst deviation", flatWorst, "dB");
}

static void testRate()
{
    const auto measure = [] (const Settings& s, double sr, double seconds, TestHead* head = nullptr)
    {
        auto p = makeProc (s, sr);
        const int n = (int) (sr * seconds);
        auto in = softNoiseSignal (n, sr);
        auto out = head != nullptr ? renderWithHead (*p, *head, in, 512, sr, 0.0) : render (*p, in, 512);
        const int frame = (int) (0.006 * sr), hop = (int) (0.003 * sr);
        return crossingRate (relSeries (in, out.getReadPointer (0), n, p->getLatencySamples(), sr, s.depth, frame, hop));
    };

    // Free running. Limit: within 1% of the set value.
    for (float hz : { 0.25f, 1.0f, 5.0f })
    {
        Settings s; s.rate = hz; s.depth = 2.0f;
        const double got = measure (s, 48000.0, std::max (3.5 / hz, 1.5));
        check (std::abs (got - hz) / hz <= 0.01, "Rate " + std::to_string (hz).substr (0, 4) + " Hz measures", got, "Hz");
    }
    {
        Settings s; s.rate = 1.0f; s.depth = 2.0f;
        const double got = measure (s, 96000.0, 4.0);
        check (std::abs (got - 1.0) <= 0.01, "Rate 1 Hz at 96 kHz measures", got, "Hz");
        const double got44 = measure (s, 44100.0, 4.0);
        check (std::abs (got44 - 1.0) <= 0.01, "Rate 1 Hz at 44.1 kHz measures", got44, "Hz");
    }

    // Synced to a host tempo. Limit: within 1%.
    struct Case { double bpm; int division; double beats; const char* name; };
    for (const auto& c : { Case { 100.0, 8, 1.0, "1/4 at 100 BPM" }, Case { 140.0, 10, 0.75, "1/8 dotted at 140 BPM" }, Case { 90.0, 3, 4.0, "1 bar at 90 BPM" }, Case { 120.0, 9, 2.0 / 3.0, "1/4 triplet at 120 BPM" } })
    {
        Settings s; s.sync = true; s.division = c.division; s.depth = 2.0f; s.rate = 7.0f; // Rate must be ignored
        TestHead head; head.bpm = c.bpm;
        const double expected = c.bpm / (60.0 * c.beats);
        const double got = measure (s, 48000.0, std::max (3.5 / expected, 1.5), &head);
        check (std::abs (got - expected) / expected <= 0.01, std::string ("synced ") + c.name + ", expected " + std::to_string (expected).substr (0, 5) + " Hz, measures", got, "Hz");
    }

    // No playhead, and a playhead with no tempo: 120 BPM. 1 bar at 120 is 0.5 Hz, 1/4 is 2 Hz.
    {
        Settings s; s.sync = true; s.division = 3; s.depth = 2.0f;
        const double got = measure (s, 48000.0, 7.0);
        check (std::abs (got - 0.5) <= 0.005, "synced 1 bar with no playhead falls back to 120 BPM", got, "Hz");
        s.division = 8;
        TestHead head; head.tempoKnown = false;
        const double got2 = measure (s, 48000.0, 2.5, &head);
        check (std::abs (got2 - 2.0) <= 0.02, "synced 1/4 with a playhead that gives no tempo", got2, "Hz");
    }

    // Phase locks to the bar. Start half way through a 1 bar cycle and compare with the expected sweep.
    {
        const double sr = 48000.0;
        Settings s; s.sync = true; s.division = 3; s.depth = 2.0f;
        auto p = makeProc (s, sr);
        const int n = (int) (sr * 4.0);
        auto in = softNoiseSignal (n, sr);
        TestHead head;
        auto out = renderWithHead (*p, head, in, 512, sr, 2.0);
        const auto series = relSeries (in, out.getReadPointer (0), n, p->getLatencySamples(), sr, 2.0, 512, 960);
        double sum = 0;
        for (size_t i = 0; i < series.t.size(); ++i)
        {
            const double expected = 2.0 * std::sin (juce::MathConstants<double>::twoPi * (2.0 + series.t[i] * 2.0) / 4.0);
            sum += (series.rel[i] - expected) * (series.rel[i] - expected);
        }
        const double err = std::sqrt (sum / (double) series.t.size());
        // Limit: rms error under 5% of the depth
        check (err <= 0.1, "sweep locks to the host bar position, rms error", err, "ms");
    }

    // A transport jump is slewed, not a click. Limit: step under 0.05 on a -18 dBFS 220 Hz sine.
    {
        const double sr = 48000.0;
        Settings s; s.sync = true; s.division = 8; s.depth = 5.0f; s.mix = 100.0f;
        auto p = makeProc (s, sr, 2, 256);
        auto in = sineSignal ((int) sr * 2, 220.0, sr, 0.125);
        TestHead head;
        p->setPlayHead (&head);
        auto out = render (*p, in, 256, nullptr, [&] (int idx) { head.ppq = idx * 256.0 / sr * 2.0 + (idx >= 180 ? 0.37 : 0.0); });
        p->setPlayHead (nullptr);
        const double j = maxJump (out.getReadPointer (0), out.getNumSamples());
        check (j <= 0.05, "transport jump makes no click, biggest step", j);
    }

    // Offline bounce: same output with a playhead or without one
    for (bool synced : { false, true })
    {
        const double sr = 48000.0;
        Settings s; s.sync = synced; s.depth = 3.0f; s.feedback = 40.0f; s.saturation = 30.0f; s.mix = 50.0f; s.spread = 90.0f;
        auto in = noiseSignal ((int) sr * 3);
        auto a = makeProc (s, sr);
        auto outA = render (*a, in, 512);
        auto b = makeProc (s, sr);
        TestHead head;
        auto outB = renderWithHead (*b, head, in, 512, sr, 0.0);
        const double db = nullDb (outA.getReadPointer (1), outB.getReadPointer (1), outA.getNumSamples(), rms (outA.getReadPointer (1), outA.getNumSamples()));
        // Limit: the difference is 80 dB under the signal
        check (db <= -80.0, std::string ("same output with a playhead at 120 BPM or none, ") + (synced ? "synced" : "free"), db, "dB");
    }
}

static void testDepth()
{
    for (double sr : { 48000.0, 96000.0, 44100.0 })
        for (float depth : { 0.5f, 1.0f, 2.5f, 5.0f })
        {
            Settings s; s.rate = 0.5f; s.depth = depth;
            auto p = makeProc (s, sr);
            const int n = (int) (sr * 2.5);
            auto in = softNoiseSignal (n, sr);
            auto out = render (*p, in, 512);
            const auto series = relSeries (in, out.getReadPointer (0), n, p->getLatencySamples(), sr, depth, (int) (0.02 * sr), (int) (0.02 * sr));
            const double range = series.maxV - series.minV;
            // Limit: total sweep is twice Depth, within 4%
            check (std::abs (range - 2.0 * depth) / (2.0 * depth) <= 0.04, "Depth " + std::to_string (depth).substr (0, 4) + " ms at " + std::to_string ((int) sr) + " Hz, sweep range", range, "ms");
        }
    // Depth 0 is a static wet path lined up with the dry path
    Settings s; s.depth = 0.0f;
    auto p = makeProc (s, 48000.0);
    auto in = noiseSignal (48000);
    auto out = render (*p, in, 512);
    const double rel = relDelayMs (in, out.getReadPointer (0), 12000, 12000, p->getLatencySamples(), 64, 48000.0);
    check (std::abs (rel) <= 0.001, "Depth 0 sits at zero delay", rel, "ms");
}

static void testFeedback()
{
    const double sr = 48000.0;
    // At a quarter of the LFO cycle the delay stands still: a good place to read the static comb.
    const double L = std::sqrt (2.5 * 2.5 + 0.25 * 0.25); // ms, loop length when the wet is 2.5 ms from the dry path
    const double spacing = 1000.0 / L;

    struct Reading { double peak; };
    const auto read = [&] (float fb, double seconds, float mix)
    {
        Settings s; s.rate = 0.05f; s.depth = 2.5f; s.feedback = fb; s.mix = mix;
        auto p = makeProc (s, sr);
        const int n = (int) (sr * (seconds + 1.0));
        auto in = noiseSignal (n, 0.03);
        auto out = render (*p, in, 512);
        const int order = 13, N = 1 << order;
        const auto db = transferDb (in, out.getReadPointer (0), (int) (seconds * sr) - N, p->getLatencySamples(), order, 3);
        const double binHz = sr / N;
        const int lo = (int) (150.0 / binHz), hi = (int) (4000.0 / binHz);
        const double med = medianOf (std::vector<double> (db.begin() + lo, db.begin() + hi));
        std::vector<double> top;
        for (int k = lo; k < hi; ++k)
            if (db[(size_t) k] > db[(size_t) k - 1] && db[(size_t) k] >= db[(size_t) k + 1])
                top.push_back (db[(size_t) k]);
        std::sort (top.rbegin(), top.rend());
        double avg = 0; const int count = std::min ((int) top.size(), 5);
        for (int i = 0; i < count; ++i) avg += top[(size_t) i];
        return Reading { (count > 0 ? avg / count : 0.0) - med };
    };

    // Both sides of zero (wet after dry at 5 s, wet before dry at 15 s), both signs of Feedback.
    for (double when : { 5.0, 15.0 })
    {
        const auto side = std::string (when < 10.0 ? "wet after dry" : "wet before dry");
        for (float sign : { 1.0f, -1.0f })
        {
            const auto name = std::string (sign > 0 ? "Feedback + " : "Feedback - ");

            // Wet only shows the loop by itself. Limits: each step up lifts the peaks at least 1.5 dB, 90% stands 15 dB or more clear.
            double previous = -100.0;
            for (float amount : { 0.0f, 30.0f, 60.0f, 90.0f })
            {
                const auto r = read (sign * amount, when, 100.0f);
                if (amount > 0.0f)
                    check (r.peak >= previous + 1.5, name + std::to_string ((int) amount) + "% lifts the comb peaks, Mix 100, " + side, r.peak, "dB above median");
                if (amount == 90.0f)
                {
                    check (r.peak >= 15.0, name + "90% peak height, Mix 100, " + side, r.peak, "dB");
                    // Limit: peaks land on the comb within 4%. Positive: whole multiples of the spacing. Negative: odd halves.
                    // A longer frame than above, and the search looks only around each expected tooth, so ripple on a skirt is not read as a peak.
                    Settings s; s.rate = 0.05f; s.depth = 2.5f; s.feedback = sign * 90.0f; s.mix = 100.0f;
                    auto p = makeProc (s, sr);
                    const int n = (int) (sr * (when + 1.0));
                    auto in = noiseSignal (n, 0.03);
                    auto out = render (*p, in, 512);
                    const int order = 14, N = 1 << order;
                    const auto db = transferDb (in, out.getReadPointer (0), (int) (when * sr) - N, p->getLatencySamples(), order, 3);
                    const double binHz = sr / N;
                    double worst = 0; int used = 0;
                    for (double tooth = sign > 0 ? 1.0 : 0.5; tooth * spacing < 3000.0; tooth += 1.0)
                    {
                        const double expected = tooth * spacing;
                        int bestBin = (int) ((expected - 0.3 * spacing) / binHz);
                        for (int k = bestBin; k <= (int) ((expected + 0.3 * spacing) / binHz); ++k)
                            if (db[(size_t) k] > db[(size_t) bestBin]) bestBin = k;
                        const double a = db[(size_t) bestBin - 1], b = db[(size_t) bestBin], c = db[(size_t) bestBin + 1], den = a - 2.0 * b + c;
                        const double f = (bestBin + (std::abs (den) > 1.0e-9 ? 0.5 * (a - c) / den : 0.0)) * binHz;
                        worst = std::max (worst, std::abs (f - expected) / expected * 100.0);
                        ++used;
                    }
                    check (used >= 3 && worst <= 4.0, name + "90% peak positions against the loop length, worst error, " + side, worst, "%");
                }
                previous = r.peak;
            }

            // At Mix 50 the dry path joins in. Wet before dry, the first echo lands on the dry path, which raises the median as well
            // as the peaks, so the rise at 30% is small there: 0.7 dB from the formula for this structure.
            // First limit was 1.5 dB per step on both sides. It is wrong for that case, so here: every step must still rise,
            // by at least 0.3 dB, and 90% must stand 9 dB clear (unchanged).
            previous = -100.0;
            for (float amount : { 0.0f, 30.0f, 60.0f, 90.0f })
            {
                const auto r = read (sign * amount, when, 50.0f);
                if (amount > 0.0f)
                    check (r.peak >= previous + 0.3, name + std::to_string ((int) amount) + "% lifts the comb peaks, Mix 50, " + side, r.peak, "dB above median");
                if (amount == 90.0f)
                    check (r.peak >= 9.0, name + "90% peak height, Mix 50, " + side, r.peak, "dB");
                previous = r.peak;
            }
        }
    }

    // Stays bounded at both extremes, with loud input, at the slowest and fastest sweeps, with and without Saturation.
    for (float fb : { 95.0f, -95.0f })
        for (float sat : { 0.0f, 100.0f })
            for (float rate : { 0.1f, 10.0f })
                for (int kind : { 0, 1 })
                {
                    Settings s; s.rate = rate; s.depth = 5.0f; s.feedback = fb; s.saturation = sat; s.mix = 100.0f;
                    auto p = makeProc (s, sr);
                    const int n = (int) (sr * 8.0), sounding = (int) (sr * 5.0);
                    std::vector<float> in ((size_t) n, 0.0f);
                    if (kind == 0) noise (in.data(), sounding, 1.0, 5); else sine (in.data(), sounding, 397.0, sr, 1.0);
                    auto out = render (*p, in, 512);
                    double pk = 0; for (int ch = 0; ch < 2; ++ch) pk = std::max (pk, peak (out.getReadPointer (ch), n));
                    const auto tag = "Feedback " + std::to_string ((int) fb) + ", Saturation " + std::to_string ((int) sat) + ", Rate " + std::to_string (rate).substr (0, 4) + (kind == 0 ? ", noise" : ", sine");
                    // Limit: never over +12 dBFS with full scale input
                    check (countNonFinite (out) == 0 && pk <= 4.0, "bounded: " + tag + ", peak", toDb (pk), "dBFS");
                    // Limit: rings out to under -80 dBFS within 3 s of the input stopping
                    const double tail = rmsDb (out, 0, n - (int) (0.1 * sr));
                    check (tail <= -80.0, "rings out: " + tag, tail, "dBFS");
                }
}

static void testSaturation()
{
    const double sr = 48000.0;
    const int n = (int) sr * 2;
    double previous = -1.0, thd100 = 0.0;
    auto in = sineSignal (n, 1000.0, sr, 0.25);
    for (float sat : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
    {
        Settings s; s.depth = 0.0f; s.saturation = sat;
        auto p = makeProc (s, sr);
        auto out = render (*p, in, 512);
        const double thd = thdPercent (out.getReadPointer (0) + n / 2, n / 2, 1000.0, sr);
        const auto name = "THD at Saturation " + std::to_string ((int) sat) + "%, 1 kHz at -12 dBFS";
        // Limits: clean at 0, and every step adds distortion
        if (sat == 0.0f) check (thd <= 0.01, name, thd, "%");
        else check (thd > previous * 1.2, name, thd, "%");
        previous = thd;
        if (sat == 100.0f) thd100 = thd;
    }
    // Limit: at the top setting at least 5% on a -12 dBFS sine
    check (thd100 >= 5.0, "THD at full Saturation is plainly audible", thd100, "%");

    // Level at -18 dBFS stays put. Limit: within 0.5 dB.
    {
        auto x = sineSignal (n, 1000.0, sr, 0.125);
        Settings a; a.depth = 0.0f; Settings b = a; b.saturation = 100.0f;
        auto pa = makeProc (a, sr); auto oa = render (*pa, x, 512);
        auto pb = makeProc (b, sr); auto ob = render (*pb, x, 512);
        const double diff = rmsDb (ob, 0, n / 2) - rmsDb (oa, 0, n / 2);
        check (std::abs (diff) <= 0.5, "full Saturation keeps -18 dBFS level", diff, "dB");
    }

    // Aliasing at full Saturation. Limit: nothing inharmonic above -60 dB under the fundamental.
    {
        const int order = 16, N = 1 << order;
        const double f0 = 7300.0;
        Settings s; s.depth = 0.0f; s.saturation = 100.0f;
        auto p = makeProc (s, sr);
        auto x = sineSignal (N * 2, f0, sr, 0.25);
        auto out = render (*p, x, 512);
        juce::dsp::FFT fft (order);
        std::vector<float> a ((size_t) (2 * N), 0.0f);
        double norm = 0;
        for (int i = 0; i < N; ++i)
        {
            const float w = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (N - 1)));
            a[(size_t) i] = w * out.getSample (0, N + i);
            norm += w;
        }
        fft.performFrequencyOnlyForwardTransform (a.data());
        const double binHz = sr / N;
        double fund = 0, worst = 0;
        for (int k = 8; k < N / 2; ++k)
        {
            const double f = k * binHz;
            const double harmonic = std::round (f / f0);
            const double mag = 2.0 * a[(size_t) k] / norm;
            if (harmonic == 1.0 && std::abs (f - f0) < 8 * binHz) fund = std::max (fund, mag);
            if (harmonic >= 1.0 && std::abs (f - harmonic * f0) < 8 * binHz) continue;
            worst = std::max (worst, mag);
        }
        const double db = toDb (worst / std::max (fund, 1.0e-12));
        check (db <= -60.0, "largest inharmonic product at full Saturation, 7.3 kHz", db, "dB");
    }
}

static void testTone()
{
    const double sr = 48000.0;
    const int n = (int) sr;
    const auto level = [&] (float tone, double f)
    {
        Settings s; s.depth = 0.0f; s.tone = tone;
        auto p = makeProc (s, sr);
        auto in = sineSignal (n, f, sr, 0.1);
        auto out = render (*p, in, 512);
        return toneDb (out.getReadPointer (0) + n / 2, n / 2, f, sr);
    };
    const double flatHigh = level (0.0f, 12000.0), flatLow = level (0.0f, 200.0);
    const double cut = level (-12.0f, 12000.0) - flatHigh, mid = level (-6.0f, 12000.0) - flatHigh, boost = level (12.0f, 12000.0) - flatHigh;
    // Limits: the top end follows the control within 1.5 dB at 12 kHz
    check (std::abs (cut + 12.0) <= 1.5, "Tone -12 dB at 12 kHz", cut, "dB");
    check (std::abs (mid + 6.0) <= 1.5, "Tone -6 dB at 12 kHz", mid, "dB");
    check (std::abs (boost - 12.0) <= 1.5, "Tone +12 dB at 12 kHz", boost, "dB");
    // Limit: the low end does not move by more than 0.3 dB
    const double lowCut = level (-12.0f, 200.0) - flatLow, lowBoost = level (12.0f, 200.0) - flatLow;
    check (std::abs (lowCut) <= 0.3, "Tone -12 dB at 200 Hz", lowCut, "dB");
    check (std::abs (lowBoost) <= 0.3, "Tone +12 dB at 200 Hz", lowBoost, "dB");
    // Limit: Tone at 0 leaves 12 kHz within 0.1 dB of the input
    check (std::abs (flatHigh - toDb (0.1)) <= 0.1, "Tone 0 is flat at 12 kHz", flatHigh - toDb (0.1), "dB");
}

static void testSpread()
{
    const double sr = 48000.0;
    const float rate = 1.0f;
    for (float spread : { 0.0f, 45.0f, 90.0f, 135.0f, 180.0f })
    {
        Settings s; s.rate = rate; s.depth = 2.0f; s.spread = spread;
        auto p = makeProc (s, sr);
        const int n = (int) (sr * 4.2);
        auto in = softNoiseSignal (n, sr);
        auto out = render (*p, in, 512);
        const int latency = p->getLatencySamples();
        const auto left = relSeries (in, out.getReadPointer (0), n, latency, sr, 2.0, 512, 240);
        const auto right = relSeries (in, out.getReadPointer (1), n, latency, sr, 2.0, 512, 240);
        double diff = seriesPhaseDeg (right, rate) - seriesPhaseDeg (left, rate);
        while (diff < -1.0) diff += 360.0;
        while (diff > 359.0) diff -= 360.0;
        // Limit: within 3 degrees of the setting
        check (std::abs (diff - spread) <= 3.0, "Spread " + std::to_string ((int) spread) + " degrees, right against left sweep", diff, "deg");
        if (spread == 0.0f)
        {
            const double db = nullDb (out.getReadPointer (0), out.getReadPointer (1), n, rms (out.getReadPointer (0), n));
            check (db <= -120.0, "Spread 0 leaves left and right identical", db, "dB");
        }
    }
}

static void testMix()
{
    const double sr = 48000.0;
    const int n = (int) sr * 2;
    auto in = noiseSignal (n);
    Settings base; base.rate = 1.0f; base.depth = 3.0f; base.feedback = 40.0f; base.saturation = 30.0f; base.tone = -3.0f; base.spread = 60.0f;
    Settings dryS = base; dryS.mix = 0.0f;
    Settings wetS = base; wetS.mix = 100.0f;
    auto pd = makeProc (dryS, sr); auto dry = render (*pd, in, 512);
    auto pw = makeProc (wetS, sr); auto wet = render (*pw, in, 512);
    const double ref = rms (dry.getReadPointer (0), n);
    for (float m : { 25.0f, 50.0f, 75.0f })
    {
        Settings s = base; s.mix = m;
        auto p = makeProc (s, sr);
        auto out = render (*p, in, 512);
        std::vector<float> blend ((size_t) n);
        for (int i = 0; i < n; ++i) blend[(size_t) i] = (1.0f - m * 0.01f) * dry.getSample (0, i) + m * 0.01f * wet.getSample (0, i);
        const double db = nullDb (out.getReadPointer (0), blend.data(), n, ref);
        // Limit: output equals the straight blend of the two ends to 80 dB
        check (db <= -80.0, "Mix " + std::to_string ((int) m) + "% against a straight blend of dry and wet", db, "dB");
    }

    // Mix 0 is the input, delayed. Limits: level within 0.05 dB in band, null 50 dB down at each test tone.
    for (double f : { 100.0, 1000.0, 10000.0, 18000.0 })
    {
        Settings s; s.mix = 0.0f; s.saturation = 100.0f; s.feedback = 90.0f;
        auto p = makeProc (s, sr);
        auto x = sineSignal (n, f, sr, 0.25);
        auto out = render (*p, x, 512);
        const int latency = p->getLatencySamples();
        const double change = toneDb (out.getReadPointer (0) + n / 2, n / 2, f, sr) - toneDb (x.data() + n / 2, n / 2, f, sr);
        check (std::abs (change) <= 0.05, "Mix 0 level at " + std::to_string ((int) f) + " Hz", change, "dB");
        const double db = nullDb (out.getReadPointer (0) + n / 2 + latency, x.data() + n / 2, n / 2 - latency, rms (x.data(), n));
        check (db <= -50.0, "Mix 0 nulls against the delayed input at " + std::to_string ((int) f) + " Hz", db, "dB");
    }

    // Bypass is the same as Mix 0, same delay. Limit: 80 dB null once the fade is done.
    {
        Settings s = base; s.bypass = true;
        auto p = makeProc (s, sr);
        auto out = render (*p, in, 512);
        const double db = nullDb (out.getReadPointer (0) + n / 2, dry.getReadPointer (0) + n / 2, n / 2, ref);
        check (db <= -80.0, "Bypass against Mix 0", db, "dB");

        auto q = makeProc (base, sr);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> capture (2, n);
        for (int pos = 0; pos + 512 <= n; pos += 512)
        {
            for (int ch = 0; ch < 2; ++ch) buf.copyFrom (ch, 0, in.data() + pos, 512);
            q->processBlockBypassed (buf, midi);
            capture.copyFrom (0, pos, buf, 0, 0, 512);
        }
        const double db2 = nullDb (capture.getReadPointer (0) + n / 2, dry.getReadPointer (0) + n / 2, (n - n / 2) / 512 * 512, ref);
        check (db2 <= -80.0, "host bypass call against Mix 0", db2, "dB");
    }
}

static void testGeneralBehaviour()
{
    const double sr = 48000.0;
    // Limit: default settings are not silent, within 6 dB of the input on noise
    {
        PluginProcessor p;
        prepare (p, 2, sr, 512);
        auto in = noiseSignal ((int) sr * 2);
        auto out = render (p, in, 512);
        const double change = rmsDb (out, 0, (int) sr) - toDb (rms (in.data() + (int) sr, (int) sr));
        check (std::abs (change) <= 6.0, "default settings, level against input", change, "dB");
    }
    // Limit: block size does not change the sound, 90 dB null
    {
        Settings s; s.rate = 2.0f; s.depth = 3.0f; s.feedback = 50.0f; s.saturation = 40.0f; s.tone = -4.0f; s.spread = 90.0f; s.mix = 60.0f;
        auto in = noiseSignal ((int) sr * 2);
        auto a = makeProc (s, sr, 2, 1024); auto oa = render (*a, in, 1024);
        auto b = makeProc (s, sr, 2, 1024); auto ob = render (*b, in, 64);
        const double db = nullDb (oa.getReadPointer (0), ob.getReadPointer (0), (int) sr * 2, rms (oa.getReadPointer (0), (int) sr * 2));
        check (db <= -90.0, "block size 64 against 1024", db, "dB");

        // A second prepare wipes the state: output matches a fresh instance. Limit: 90 dB null.
        auto c = makeProc (s, 96000.0, 2, 256);
        render (*c, in, 256);
        prepare (*c, 2, sr, 1024);
        auto oc = render (*c, in, 1024);
        const double db2 = nullDb (oa.getReadPointer (0), oc.getReadPointer (0), (int) sr * 2, rms (oa.getReadPointer (0), (int) sr * 2));
        check (db2 <= -90.0, "second prepare matches a fresh start", db2, "dB");

        // A host block longer than the prepared size. Limit: same 90 dB null.
        auto e = makeProc (s, sr, 2, 256);
        auto oe = render (*e, in, 1024);
        const double db3 = nullDb (oa.getReadPointer (0), oe.getReadPointer (0), (int) sr * 2, rms (oa.getReadPointer (0), (int) sr * 2));
        check (db3 <= -90.0, "host block longer than prepared", db3, "dB");
    }
    // Mono runs and follows the left channel of stereo. Limit: 90 dB null.
    {
        Settings s; s.rate = 1.0f; s.depth = 3.0f; s.feedback = 30.0f; s.spread = 120.0f;
        auto in = noiseSignal ((int) sr);
        auto st = makeProc (s, sr, 2); auto os = render (*st, in, 512);
        auto mo = makeProc (s, sr, 1); auto om = render (*mo, in, 512);
        check (om.getNumChannels() == 1, "mono layout gives one channel");
        const double db = nullDb (os.getReadPointer (0), om.getReadPointer (0), (int) sr, rms (os.getReadPointer (0), (int) sr));
        check (db <= -90.0, "mono against the left of stereo", db, "dB");
    }
    // Every preset makes sound and stays bounded
    {
        PluginProcessor p;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            prepare (p, 2, sr, 512);
            auto in = noiseSignal ((int) sr * 2, 0.25);
            auto out = render (p, in, 512);
            const double lvl = rmsDb (out, 0, (int) sr);
            double pk = peak (out.getReadPointer (0), out.getNumSamples());
            // Limit: within 12 dB of the input rms and under +6 dBFS
            const double inLvl = toDb (rms (in.data(), (int) sr));
            check (countNonFinite (out) == 0 && std::abs (lvl - inLvl) <= 12.0 && pk <= 2.0, "preset '" + p.getProgramName (i).toStdString() + "', level against input", lvl - inLvl, "dB");
        }
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    GenericOptions opt;
    opt.minPresets = 4;
    runGenericChecks ([] { return std::make_unique<PluginProcessor>(); }, opt);
    testLatency();
    testThroughZero();
    testNotches();
    testRate();
    testDepth();
    testFeedback();
    testSaturation();
    testTone();
    testSpread();
    testMix();
    testGeneralBehaviour();
    return finish();
}
