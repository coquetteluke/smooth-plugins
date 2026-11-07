#pragma once
// Vocal tract envelope, tracked over time, in two versions: as it is, and as it should be after Formant Shift.
//
// A frame every eighth of a window: Hann window, power spectrum, a memory over frames that is long on held sounds and none on changing
// ones, then an all-pole fit as reflection coefficients. On a voiced sound with separate harmonics the fit answers to the harmonics
// alone. The second version comes from the same spectrum with its frequency axis scaled by the formant ratio, so it is the same
// vocal tract, longer or shorter. Reflection coefficients stay stable when interpolated.
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
    static constexpr int maxOrder = 72;
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
        order = std::clamp ((int) std::lround (sr / 1000.0), 16, 72);

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
            // Not for a Robot: its one note lands on the envelope anywhere, and a fit that follows the harmonics of a voice in motion
            // moves with them, which at that note shows as wobble.
            const bool useLines = pitchHz >= lowestLinesHz && sharpen == 1.0 && findLines (pitchHz);
            if (useLines)
                fitToLines (1.0, lags.data(), 1.0);
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
            if (useLines)
                fitToLines (ratio, f.wantedLags.data(), sharpen);
            // Squeezing a spectrum squeezes its peaks too, and a very narrow peak is what turns small moves in the fit into pitch wobble.
            const double squeeze = std::max (1.0 / ratio - 1.0, 0.0);
            const double wide = wideAmount * squeeze * squeeze;
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

    // A window on a high voice sees separate harmonics, and an ordinary all-pole fit, which also has to explain the gaps between them,
    // leans towards whichever harmonic is nearest a formant. When the harmonics then move, the lean shows as formants moved. So on a
    // voiced frame the fit answers to the harmonics alone: discrete all-pole modelling (El-Jaroudi and Makhoul), which Smooth Throat
    // also uses. The whitening filter then flattens the harmonics exactly, and no ghost of the old formant stays behind.
    bool findLines (double pitchHz) noexcept
    {
        const int half = N / 2;
        const double binHz = sr / N;
        numLines = 0;
        for (int h = 1; h <= maxLines && h * pitchHz < std::min (22800.0, 0.95 * sr * 0.5); ++h)
        {
            const double at = h * pitchHz / binHz;
            int bin = std::clamp ((int) std::lround (at), 1, half - 1);
            // Tuned on the pitch from the tracker, within half a main lobe.
            for (int i = std::max (1, bin - 1); i <= std::min (half - 1, bin + 1); ++i)
                if (held[(size_t) i] > held[(size_t) bin]) bin = i;
            const double a = held[(size_t) bin - 1], b = held[(size_t) bin], c = held[(size_t) bin + 1];
            const double bend = a - 2.0 * b + c;
            const double step = bend < 0.0 ? std::clamp (0.5 * (a - c) / bend, -0.5, 0.5) : 0.0;
            lineFrequency[(size_t) numLines] = 2.0 * M_PI * (bin + step) / N;
            linePower[(size_t) numLines] = std::max (b - 0.25 * (a - c) * step, 1.0e-30);
            ++numLines;
        }
        return numLines >= 8;
    }

    // Replaces lags with those of the all-pole curve fitted through the lines, whose frequencies are scaled by ratio.
    // Cosines come from a rotating pair rather than a call each, and the matrix is Toeplitz, so its recursion runs once and every
    // round only needs its stored steps. Measured at 20% of a core before that, on the same fit.
    void fitToLines (double ratio, double* lags, double sharpen) noexcept
    {
        const int p = order;
        std::array<double, maxOrder + 1> r {};
        int used = 0;
        double top = 1.0e-30;
        for (int k = 0; k < numLines; ++k)
            top = std::max (top, linePower[(size_t) k]);

        const auto addLine = [&] (double w, double power)
        {
            lineFitFrequency[(size_t) used] = w;
            lineFitCos[(size_t) used] = std::cos (w);
            lineFitSin[(size_t) used] = std::sin (w);
            const double cw = lineFitCos[(size_t) used], sw = lineFitSin[(size_t) used];
            double c = 1.0, s = 0.0;
            for (int l = 0; l <= p; ++l)
            {
                r[(size_t) l] += power * c;
                const double next = c * cw - s * sw;
                s = s * cw + c * sw;
                c = next;
            }
            ++used;
        };

        for (int k = 0; k < numLines; ++k)
        {
            const double w = lineFrequency[(size_t) k] * ratio;
            if (w >= M_PI) break;
            addLine (w, sharpen != 1.0 ? top * std::pow (linePower[(size_t) k] / top, sharpen) : linePower[(size_t) k]);
        }
        // Squeezed lines stop short of the top of the band. Carry on at their level, or the fit is free to do anything up there.
        if (used >= 2 && used == numLines)
        {
            const double spacing = lineFitFrequency[(size_t) used - 1] - lineFitFrequency[(size_t) used - 2];
            // One line up there is mostly noise, so take the level from several.
            double logLast = 0.0;
            const int tail = std::min (used, 12);
            for (int k = used - tail; k < used; ++k)
                logLast += std::log (linePower[(size_t) k]);
            const double level = std::exp (logLast / tail);
            const double last = sharpen != 1.0 ? top * std::pow (level / top, sharpen) : level;
            double w = lineFitFrequency[(size_t) used - 1] + spacing;
            while (w < M_PI && used < maxFitLines)
            {
                addLine (w, last);
                w += spacing;
            }
        }
        if (used <= p + 2 || ! (r[0] > 0.0))
            return; // too few harmonics, or the top of the voice would go over the top of the band: keep the ordinary fit
        for (double& v : r) v /= used;
        r[0] *= 1.0 + 1.0e-4;

        // The recursion over the matrix, with every step kept.
        double error = r[0];
        steps[0][0] = 1.0;
        errors[0] = error;
        for (int m = 1; m <= p; ++m)
        {
            double acc = 0.0;
            for (int j = 0; j < m; ++j)
                acc += steps[(size_t) m - 1][(size_t) j] * r[(size_t) (m - j)];
            const double k = -acc / error;
            if (! std::isfinite (k) || std::abs (k) >= 1.0)
                return;
            steps[(size_t) m][0] = 1.0;
            for (int j = 1; j < m; ++j)
                steps[(size_t) m][(size_t) j] = steps[(size_t) m - 1][(size_t) j] + k * steps[(size_t) m - 1][(size_t) (m - j)];
            steps[(size_t) m][(size_t) m] = k;
            error *= 1.0 - k * k;
            errors[(size_t) m] = error;
        }

        // Start from the ordinary fit to the lines, a little widened, on the unit gain scale the rounds use.
        std::array<double, maxOrder + 1> a {}, target {}, startLags = r, startK {}, startA {};
        const double startError = levinson (startLags.data(), startK.data());
        if (! (startError > 0.0))
            return;
        a[0] = 1.0;
        for (int i = 1; i <= p; ++i)
        {
            startA = a;
            for (int j = 1; j < i; ++j)
                a[(size_t) j] = startA[(size_t) j] + startK[(size_t) i - 1] * startA[(size_t) (i - j)];
            a[(size_t) i] = startK[(size_t) i - 1];
        }
        const double gain = 1.0 / std::sqrt (startError);
        for (int i = 0; i <= p; ++i)
            a[(size_t) i] *= gain;

        for (int round = 0; round < fitRounds; ++round)
        {
            // Right hand side: the model's own response read at the lines.
            target.fill (0.0);
            for (int k = 0; k < used; ++k)
            {
                const double cw = lineFitCos[(size_t) k], sw = lineFitSin[(size_t) k];
                double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
                for (int i = 0; i <= p; ++i)
                {
                    re += a[(size_t) i] * c;
                    im -= a[(size_t) i] * s;
                    const double next = c * cw - s * sw;
                    s = s * cw + c * sw;
                    c = next;
                }
                const double d = re * re + im * im + 1.0e-30;
                const double hr = re / d, hi = -im / d;
                c = 1.0; s = 0.0;
                for (int i = 0; i <= p; ++i)
                {
                    target[(size_t) i] += hr * c + hi * s;
                    const double next = c * cw - s * sw;
                    s = s * cw + c * sw;
                    c = next;
                }
            }
            for (int i = 0; i <= p; ++i) target[(size_t) i] /= used;

            // Solve with the stored steps.
            a[0] = target[0] / r[0];
            for (int m = 1; m <= p; ++m)
            {
                double acc = target[(size_t) m];
                for (int j = 0; j < m; ++j)
                    acc -= a[(size_t) j] * r[(size_t) (m - j)];
                const double q = acc / errors[(size_t) m];
                for (int j = 0; j < m; ++j)
                    a[(size_t) j] += q * steps[(size_t) m][(size_t) (m - j)];
                a[(size_t) m] = q;
            }
            if (! std::isfinite (a[0]) || a[0] <= 0.0)
                return;
        }

        // Back to lags: walk down to reflection coefficients and refuse anything unstable, then walk up to the curve's autocorrelation.
        std::array<double, maxOrder + 1> mono {}, previous {}, ks {};
        const double g = 1.0 / a[0];
        for (int i = 0; i <= p; ++i) mono[(size_t) i] = a[(size_t) i] * g;
        for (int m = p; m >= 1; --m)
        {
            const double kk = mono[(size_t) m];
            if (std::abs (kk) >= 0.9999)
                return;
            ks[(size_t) m] = kk;
            const double den = 1.0 - kk * kk;
            previous = mono;
            for (int j = 1; j < m; ++j)
                mono[(size_t) j] = (previous[(size_t) j] - kk * previous[(size_t) (m - j)]) / den;
        }
        std::array<double, maxOrder + 1> out {}, up {};
        out[0] = lags[0];
        double e = out[0];
        up[0] = 1.0;
        for (int m = 1; m <= p; ++m)
        {
            double acc = 0.0;
            for (int j = 1; j < m; ++j) acc += up[(size_t) j] * out[(size_t) (m - j)];
            out[(size_t) m] = -ks[(size_t) m] * e - acc;
            previous = up;
            for (int j = 1; j < m; ++j) up[(size_t) j] = previous[(size_t) j] + ks[(size_t) m] * previous[(size_t) (m - j)];
            up[(size_t) m] = ks[(size_t) m];
            e *= 1.0 - ks[(size_t) m] * ks[(size_t) m];
        }
        for (int l = 0; l <= p; ++l) lags[l] = out[(size_t) l];
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
    static constexpr int maxLines = 192;
    static constexpr int maxFitLines = 2 * maxLines;
    std::array<double, maxLines> lineFrequency {}, linePower {};
    std::array<double, maxFitLines> lineFitFrequency {}, lineFitCos {}, lineFitSin {};
    std::array<std::array<double, maxOrder + 1>, maxOrder + 1> steps {};
    std::array<double, maxOrder + 1> errors {};
    int numLines = 0;
    static constexpr int fitRounds = 3;
    static constexpr double lowestLinesHz = 150.0; // below this a 21 ms window does not see separate harmonics
    int lead = 0;
    static constexpr double floorAmount = 1.0e-5, wideAmount = 120.0;
    static constexpr double longMemory = 0.985, steadyDb = 3.0, changingDb = 6.0;
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
