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
        for (auto& f : frames) { f.analysis.fill (0.0); f.synthesis.fill (0.0); f.gain = 1.0; }
        count = 0;
    }

    int getOrder() const noexcept { return order; }
    int getHop() const noexcept { return hop; }
    int getWindow() const noexcept { return N; }

    // How far ahead of the point being filtered the newest sample has to be: half a window and a hop, for interpolating.
    int lookahead() const noexcept { return N / 2 + hop; }

    // One mono sample. ratio scales formants on frequency. sharpen above 1 makes peaks stand further above valleys.
    void push (float x, double ratio, double sharpen) noexcept
    {
        input[(size_t) (count & (long long) (N - 1))] = (double) x;
        ++count;
        if (count >= N && ((count - N) & (long long) (hop - 1)) == 0)
            analyse ((count - N) / hop, ratio, sharpen);
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
        double gain = 1.0;
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
        lags[0] *= 1.0 + 1.0e-5; // a floor, 50 dB under the total, so a pure tone cannot make an endless resonance
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

    void analyse (long long frameIndex, double ratio, double sharpen) noexcept
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
        if (energy < 1.0e-11 * N)
        {
            // Silence: no filter at all, so nothing can ring and nothing decays into denormals.
            f.analysis.fill (0.0);
            f.synthesis.fill (0.0);
            f.gain = 1.0;
            return;
        }

        const int half = N / 2;
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

        std::array<double, maxOrder + 1> lags {};
        toLags (power, lags.data());
        const double errorIn = levinson (lags.data(), f.analysis.data());

        const bool same = std::abs (ratio - 1.0) < 1.0e-6 && std::abs (sharpen - 1.0) < 1.0e-6;
        if (same)
        {
            f.synthesis = f.analysis;
            f.gain = 1.0;
        }
        else
        {
            const double scale = 1.0 / ratio;
            for (int i = 0; i <= half; ++i)
            {
                const double at = std::min (i * scale, (double) half);
                const int low = std::min ((int) at, half - 1);
                const double t = at - low;
                warped[(size_t) i] = power[(size_t) low] + t * (power[(size_t) low + 1] - power[(size_t) low]);
            }
            if (std::abs (sharpen - 1.0) >= 1.0e-6)
            {
                double top = 1.0e-30;
                for (int i = 0; i <= half; ++i) top = std::max (top, warped[(size_t) i]);
                const double inverse = 1.0 / top;
                for (int i = 0; i <= half; ++i)
                    warped[(size_t) i] = top * std::pow (std::max (warped[(size_t) i] * inverse, 1.0e-12), sharpen);
            }
            toLags (warped, lags.data());
            const double errorOut = levinson (lags.data(), f.synthesis.data());
            f.gain = errorIn > 0.0 && errorOut > 0.0 ? std::clamp (std::sqrt (errorOut / errorIn), 0.1, 10.0) : 1.0;
        }

        if ((frameIndex & 7) == 0)
            publish (f);
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
    std::vector<double> input, frame, window, real, imag, power, warped, lagWindow;
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
