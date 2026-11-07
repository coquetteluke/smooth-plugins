#pragma once
// Vocal tract envelope, tracked over time, in two versions: as it is, and as it should be after Formant Shift.
//
// A frame every eighth of a window: Hann window, power spectrum, inverse transform to autocorrelation, Levinson recursion to
// reflection coefficients. The second set comes from the same power spectrum with its frequency axis scaled by the formant
// ratio, so it is the same vocal tract, longer or shorter. Reflection coefficients stay stable when interpolated.
//
// The whitening filter (analysis) and the putting-back filter (synthesis) are lattices that use these coefficients. If nothing
// has been changed between them, they are exact inverses of each other at every sample, whatever the coefficients are.
// Everything is allocated in prepare(), so push() is real time safe.
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace smoothmorph
{
class EnvelopeEngine
{
public:
    static constexpr int maxOrder = 64;
    static constexpr int updateEvery = 8; // samples between refreshed filter coefficients
    static constexpr int numBands = 64;   // readout for a later window: dB per band, up to 12 kHz

    using Stage = std::array<double, maxOrder>;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        logN = 10;
        while ((1 << logN) < (int) std::lround (sr * 0.0213)) ++logN;
        N = 1 << logN;
        hop = N / 8;
        order = std::clamp ((int) std::lround (sr / 1000.0), 16, maxOrder);

        if (setup != nullptr) vDSP_destroy_fftsetupD (setup);
        setup = vDSP_create_fftsetupD ((vDSP_Length) logN, kFFTRadix2);
        const auto n = (size_t) N;
        input.assign (n, 0.0);
        frame.assign (n, 0.0);
        window.resize (n);
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / N);
        real.assign (n / 2, 0.0);
        imag.assign (n / 2, 0.0);
        power.assign (n / 2 + 1, 0.0);
        warped.assign (n / 2 + 1, 0.0);
        held.assign (n / 2 + 1, 0.0);
        if (const char* e = std::getenv ("KEEP")) longMemory = std::atof (e);
        if (const char* e = std::getenv ("DLOW")) steadyDb = std::atof (e);
        if (const char* e = std::getenv ("DHIGH")) changingDb = std::atof (e);
        if (const char* e = std::getenv ("WIDE")) wideAmount = std::atof (e);
        if (const char* e = std::getenv ("BASE")) baseWide = std::atof (e);
        lagWindow.resize ((size_t) order + 1);
        for (int l = 0; l <= order; ++l)
        {
            const double a = 2.0 * M_PI * 40.0 * l / sr;
            lagWindow[(size_t) l] = std::exp (-0.5 * a * a);
        }
        reset();
    }

    ~EnvelopeEngine() { if (setup != nullptr) vDSP_destroy_fftsetupD (setup); }

    void reset() noexcept
    {
        std::fill (input.begin(), input.end(), 0.0);
        for (auto& f : frames) { f.analysis.fill (0.0); f.synthesis.fill (0.0); f.wantedLags.fill (0.0); f.gain = 1.0; f.level = 0.0; f.leftover = 1.0; f.plain = true; }
        std::fill (held.begin(), held.end(), 0.0);
        count = 0;
    }

    // How many samples behind the whitening filter the putting-back filter runs. Sets how far its frames can be averaged.
    void setSynthesisDelay (int samples) noexcept { lead = std::clamp (samples / hop, 0, numFrames / 4); }

    int getOrder() const noexcept { return order; }
    int getHop() const noexcept { return hop; }
    int getWindow() const noexcept { return N; }

    // How far ahead of the point being filtered the newest sample has to be: half a window and a hop, for interpolating.
    int lookahead() const noexcept { return N / 2 + hop; }

    // One mono sample, already pre-emphasised. ratio scales formants on frequency. sharpen above 1 makes peaks stand further
    // above valleys. pitchHz is the pitch if the sound is voiced, otherwise 0: it is what keeps harmonics out of the envelope.
    void push (float x, double ratio, double sharpen, double pitchHz, bool shifted) noexcept
    {
        input[(size_t) (count & (long long) (N - 1))] = (double) x;
        ++count;
        if (count >= N && ((count - N) & (long long) (hop - 1)) == 0)
            analyse ((count - N) / hop, ratio, sharpen, pitchHz, shifted);
    }

    // Filter coefficients for one input index. Both use the same index grid, so a matched pair really does cancel.
    void analysisAt (long long index, double* k) const noexcept { blend (index, false, k, nullptr); }
    void synthesisAt (long long index, double* k, double& gain) const noexcept { blend (index, true, k, &gain); }

    // The readout, safe from any thread: last frame's envelope before and after shifting, dB per band up to 12 kHz.
    float bandBefore (int band) const noexcept { return bandsBefore[(size_t) band].load (std::memory_order_relaxed); }
    float bandAfter (int band) const noexcept { return bandsAfter[(size_t) band].load (std::memory_order_relaxed); }

private:
    struct Frame
    {
        Stage analysis {}, synthesis {};
        std::array<double, maxOrder + 1> wantedLags {};
        double gain = 1.0, level = 0.0, leftover = 1.0;
        bool plain = true;
    };
    static constexpr int numFrames = 64;

    void blend (long long index, bool synth, double* k, double* gain) const noexcept
    {
        const long long at = index & ~(long long) (updateEvery - 1);
        const long long firstCentre = N / 2;
        if (at < firstCentre - hop)
        {
            std::fill (k, k + order, 0.0);
            if (gain != nullptr) *gain = 1.0;
            return;
        }
        const long long j = (at - firstCentre + hop) / hop - 1; // -1 is the empty frame before the first
        const double t = (double) (at - (firstCentre + j * hop)) / hop;
        const Frame& a = frames[(size_t) (j & (numFrames - 1))];
        const Frame& b = frames[(size_t) ((j + 1) & (numFrames - 1))];
        const double* ka = synth ? a.synthesis.data() : a.analysis.data();
        const double* kb = synth ? b.synthesis.data() : b.analysis.data();
        for (int i = 0; i < order; ++i)
            k[i] = ka[i] + t * (kb[i] - ka[i]);
        if (gain != nullptr)
            *gain = a.gain + t * (b.gain - a.gain);
    }

    // Autocorrelation of a power spectrum. Returns lags 0 to order in lags.
    void toLags (const std::vector<double>& p, double* lags) noexcept
    {
        const int half = N / 2;
        for (int i = 0; i < half; ++i) { real[(size_t) i] = p[(size_t) i]; imag[(size_t) i] = 0.0; }
        imag[0] = p[(size_t) half];
        DSPDoubleSplitComplex z { real.data(), imag.data() };
        vDSP_fft_zripD (setup, &z, 1, (vDSP_Length) logN, kFFTDirection_Inverse);
        for (int l = 0; l <= order; ++l)
            lags[l] = (l & 1) == 0 ? real[(size_t) (l / 2)] : imag[(size_t) (l / 2)];
    }

    // Levinson recursion to reflection coefficients. Returns the prediction error, in the units of the lags.
    double levinson (double* lags, double* k) const noexcept
    {
        std::array<double, maxOrder + 1> a {}, previous {};
        std::fill (k, k + order, 0.0);
        if (! (lags[0] > 0.0) || ! std::isfinite (lags[0]))
            return 0.0;
        for (int l = 1; l <= order; ++l)
            lags[l] *= lagWindow[(size_t) l];
        lags[0] *= 1.0 + floorAmount; // a floor, 50 dB under the total, so a pure tone cannot make an endless resonance
        double error = lags[0];
        a[0] = 1.0;
        for (int i = 1; i <= order; ++i)
        {
            double acc = lags[i];
            for (int j = 1; j < i; ++j)
                acc += a[(size_t) j] * lags[i - j];
            double r = -acc / error;
            if (! std::isfinite (r) || std::abs (r) >= 1.0)
                break; // numerically done, the rest stay 0
            r = std::clamp (r, -0.9995, 0.9995);
            previous = a;
            for (int j = 1; j < i; ++j)
                a[(size_t) j] = previous[(size_t) j] + r * previous[(size_t) (i - j)];
            a[(size_t) i] = r;
            k[i - 1] = r;
            error *= 1.0 - r * r;
        }
        return error;
    }

    void analyse (long long frameIndex, double ratio, double sharpen, double pitchHz, bool shifted) noexcept
    {
        Frame& f = frames[(size_t) (frameIndex & (numFrames - 1))];
        const long long start = count - N;
        double energy = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double v = input[(size_t) ((start + i) & (long long) (N - 1))] * window[(size_t) i];
            frame[(size_t) i] = v;
            energy += v * v;
        }
        const int half = N / 2;
        if (energy < 1.0e-11 * N)
        {
            // Silence: no filter at all, so nothing can ring and nothing decays into denormals.
            f.analysis.fill (0.0);
            f.wantedLags.fill (0.0);
            f.leftover = 1.0;
            f.level = 0.0;
            f.plain = true;
            std::fill (held.begin(), held.end(), 0.0);
        }
        else
        {
            for (int i = 0; i < half; ++i)
            {
                real[(size_t) i] = frame[(size_t) (2 * i)];
                imag[(size_t) i] = frame[(size_t) (2 * i + 1)];
            }
            DSPDoubleSplitComplex z { real.data(), imag.data() };
            vDSP_fft_zripD (setup, &z, 1, (vDSP_Length) logN, kFFTDirection_Forward);
            power[0] = real[0] * real[0];
            power[(size_t) half] = imag[0] * imag[0];
            for (int i = 1; i < half; ++i)
                power[(size_t) i] = real[(size_t) i] * real[(size_t) i] + imag[(size_t) i] * imag[(size_t) i];

            // Held vowels get a long memory, because the fit is touchy near a sharp resonance and its small moves turn into pitch
            // wobble. Anything that really changes gets none, so consonants and vowel changes are followed at once.
            const double keep = memoryFor (spectrumChange());
            for (int i = 0; i <= half; ++i)
                held[(size_t) i] = keep * held[(size_t) i] + (1.0 - keep) * power[(size_t) i];
            std::array<double, maxOrder + 1> lags {};
            toLags (held, lags.data());
            const double heldLevel = lags[0];
            const double errorHeld = levinson (lags.data(), f.analysis.data());
            f.leftover = heldLevel > 0.0 ? errorHeld / (heldLevel * (1.0 + floorAmount)) : 1.0;
            f.level = heldLevel;

            // The filter that goes back on: this frame's spectrum with its frequency axis scaled.
            const double scale = 1.0 / ratio;
            for (int i = 0; i <= half; ++i)
            {
                const double at = std::min (i * scale, (double) half);
                const int low = std::min ((int) at, half - 1);
                const double t = at - low;
                warped[(size_t) i] = held[(size_t) low] + t * (held[(size_t) low + 1] - held[(size_t) low]);
            }
            if (std::abs (sharpen - 1.0) >= 1.0e-6)
            {
                double top = 1.0e-30;
                for (int i = 0; i <= half; ++i) top = std::max (top, warped[(size_t) i]);
                const double inverse = 1.0 / top;
                for (int i = 0; i <= half; ++i)
                    warped[(size_t) i] = top * std::pow (std::max (warped[(size_t) i] * inverse, 1.0e-12), sharpen);
            }
            toLags (warped, f.wantedLags.data());
            const double wide = wideAmount * std::max (1.0 / ratio - 1.0, 0.0) + baseWide;
            for (int l = 0; l <= order; ++l)
            {
                const double a = 2.0 * M_PI * wide * l / sr;
                f.wantedLags[(size_t) l] *= std::exp (-0.5 * a * a);
            }
            // Consonants with formants left alone must come out as they went in. So must everything when nothing is asked for.
            const bool unmoved = std::abs (ratio - 1.0) < 1.0e-6 && std::abs (sharpen - 1.0) < 1.0e-6;
            f.plain = unmoved && (! shifted || pitchHz <= 0.0);

            if ((frameIndex & 7) == 0)
                publish (f);
        }
        finishFrame (frameIndex - lead);
    }

    // How far this frame's spectrum is from the remembered one: RMS difference in dB over bands, with overall level taken out.
    double spectrumChange() const noexcept
    {
        const int half = N / 2;
        const int bandBins = std::max (1, (int) std::lround (250.0 / (sr / N)));
        constexpr int numChangeBands = 24;
        std::array<double, numChangeBands> difference {};
        double mean = 0.0;
        for (int b = 0; b < numChangeBands; ++b)
        {
            double now = 1.0e-20, before = 1.0e-20;
            const int from = b * bandBins + 1;
            for (int i = from; i < from + bandBins && i <= half; ++i)
            {
                now += power[(size_t) i];
                before += held[(size_t) i];
            }
            difference[(size_t) b] = 10.0 * std::log10 (now / before);
            mean += difference[(size_t) b];
        }
        mean /= numChangeBands;
        double e = 0.0;
        for (double d : difference)
            e += (d - mean) * (d - mean);
        return std::sqrt (e / numChangeBands);
    }

    double memoryFor (double changeDb) const noexcept
    {
        const double t = std::clamp ((changeDb - steadyDb) / (changingDb - steadyDb), 0.0, 1.0);
        return longMemory * (1.0 - t);
    }

    // The filter that goes back on is applied later than the whitening one, by the shifter's delay, so its frames can be averaged
    // both ways with no lag. It sets its shape from several frames and its level from this one, so attacks stay sharp while the
    // small frame to frame moves, which a sharp resonance turns into pitch wobble, go.
    void finishFrame (long long m) noexcept
    {
        if (m < 0)
            return;
        Frame& f = frames[(size_t) (m & (numFrames - 1))];
        if (f.plain)
        {
            f.synthesis = f.analysis;
            f.gain = 1.0;
            return;
        }
        std::array<double, maxOrder + 1> sum {};
        for (long long k = -lead; k <= lead; ++k)
        {
            const long long at = m + k;
            if (at < 0) continue;
            const Frame& other = frames[(size_t) (at & (numFrames - 1))];
            const double weight = (double) (lead + 1 - std::abs (k));
            for (int l = 0; l <= order; ++l)
                sum[(size_t) l] += weight * other.wantedLags[(size_t) l];
        }
        const double total = sum[0];
        const double error = levinson (sum.data(), f.synthesis.data());
        const double leftoverOut = total > 0.0 ? error / (total * (1.0 + floorAmount)) : 1.0;
        const double wanted = std::max (f.wantedLags[0], 0.0);
        f.gain = f.level > 0.0 && f.leftover > 0.0 ? std::clamp (std::sqrt (leftoverOut * wanted / (f.leftover * f.level)), 0.1, 10.0) : 1.0;
    }

    void publish (const Frame&) noexcept
    {
        const int half = N / 2;
        const double top = std::min (12000.0 / (sr * 0.5), 1.0) * half;
        for (int b = 0; b < numBands; ++b)
        {
            const int from = (int) (top * b / numBands), to = std::max (from + 1, (int) (top * (b + 1) / numBands));
            double p = 0.0, q = 0.0;
            for (int i = from; i < to && i <= half; ++i) { p = std::max (p, power[(size_t) i]); q = std::max (q, warped[(size_t) i]); }
            bandsBefore[(size_t) b].store ((float) (10.0 * std::log10 (p + 1.0e-20)), std::memory_order_relaxed);
            bandsAfter[(size_t) b].store ((float) (10.0 * std::log10 (q + 1.0e-20)), std::memory_order_relaxed);
        }
    }

    double sr = 48000.0;
    int logN = 10, N = 1024, hop = 128, order = 48;
    long long count = 0;
    FFTSetupD setup = nullptr;
    std::vector<double> input, frame, window, real, imag, power, warped, held, lagWindow;
    int lead = 0;
    double floorAmount = 1.0e-5, wideAmount = 60.0, baseWide = 0.0;
    double longMemory = 0.95, steadyDb = 2.0, changingDb = 6.0;
    std::array<Frame, numFrames> frames {};
    std::array<std::atomic<float>, numBands> bandsBefore {}, bandsAfter {};
};

// The whitening filter. Takes the voice to its excitation.
inline double latticeAnalyse (double x, const double* k, double* state, int order) noexcept
{
    double f = x, previous = x;
    for (int m = 0; m < order; ++m)
    {
        const double old = state[m];
        const double b = k[m] * f + old;
        f += k[m] * old;
        state[m] = previous;
        previous = b;
    }
    return f;
}

// The filter that puts a vocal tract back. Takes excitation to voice.
inline double latticeSynth (double e, const double* k, double* state, int order) noexcept
{
    double f = e;
    for (int m = order - 1; m >= 0; --m)
    {
        const double old = state[m];
        f -= k[m] * old;
        const double b = k[m] * f + old;
        if (m + 1 < order)
            state[m + 1] = b;
    }
    state[0] = f;
    return f;
}
} // namespace smoothmorph
