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
    static constexpr int maxOrder = 80;
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
        fitCos.assign ((size_t) maxFitLines * rowStride, 0.0);
        fitSin.assign ((size_t) maxFitLines * rowStride, 0.0);
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
        gainMean = 1.0;
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

    // One step up of the Levinson recursion, in place: a[j] = a[j] + k a[i - j] for 0 < j < i, every one from the old values. The same
    // sums as working from a copy of a, without the copy (1.2.4).
    static void stepUp (double* a, int i, double k) noexcept
    {
        for (int j = 1; j + j <= i; ++j)
        {
            const double low = a[j], high = a[i - j];
            a[j] = low + k * high;
            if (j + j < i)
                a[i - j] = high + k * low;
        }
    }

    // Levinson recursion to reflection coefficients. Returns the prediction error, in the units of the lags.
    double levinson (double* lags, double* k) const noexcept
    {
        std::array<double, maxOrder + 1> a {};
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
            stepUp (a.data(), i, r);
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
        // Only true silence (about -200 dBFS): a fixed threshold at -110 dBFS made the quiet gaps of a take quieter by 12 dB be
        // treated differently from the same take at its own level (1.2.1).
        if (energy < 1.0e-20 * N)
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
                fitToLines (1.0, lags.data(), 1.0, true);
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
                double top = 1.0e-30, before = 0.0, after = 0.0;
                for (int i = 0; i <= half; ++i) { top = std::max (top, warped[(size_t) i]); before += warped[(size_t) i]; }
                const double inverse = 1.0 / top;
                for (int i = 0; i <= half; ++i)
                {
                    warped[(size_t) i] = top * std::pow (std::max (warped[(size_t) i] * inverse, 1.0e-12), sharpen);
                    after += warped[(size_t) i];
                }
                // Sharpening deepens the valleys, which took the voice's level down with it: 15 dB at Robot on a real take, which the
                // level match then had to win back over 40 ms at every start and every change (1.2.2). The shape is sharpened, the
                // power stays what it was.
                if (after > 0.0)
                {
                    const double keep = before / after;
                    for (int i = 0; i <= half; ++i) warped[(size_t) i] *= keep;
                }
            }
            toLags (warped, f.wantedLags.data());
            if (useLines)
                fitToLines (ratio, f.wantedLags.data(), sharpen, false);
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
    // Each line's rotating pair is walked once per fit and kept in a table (fitCos, fitSin), and every round reads it from there:
    // the same numbers in the same order as walking it again in each round. That alone took the engine from 24.9% to 10.2% of a core at 96 kHz (1.2.4).
    // findPlaces: this is the frame's first fit (the whitening filter), which looks for places where the fit stands too tall between
    // two harmonics and holds them down. The second fit of the frame (the filter that goes back on) is handed the same places.
    void fitToLines (double ratio, double* lags, double sharpen, bool findPlaces) noexcept
    {
        const int p = order;
        if (findPlaces)
            numHeld = 0;
        std::array<double, maxOrder + 1> r {};
        int used = 0;
        double top = 1.0e-30;
        for (int k = 0; k < numLines; ++k)
            top = std::max (top, linePower[(size_t) k]);

        const auto addLine = [&] (double w, double power)
        {
            lineFitFrequency[(size_t) used] = w;
            lineFitPower[(size_t) used] = power;
            lineFitCos[(size_t) used] = std::cos (w);
            lineFitSin[(size_t) used] = std::sin (w);
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
        walkLines (0, used);
        addLines (r.data(), 0, used);
        if (used <= p + 2 || ! (r[0] > 0.0))
            return; // too few harmonics, or the top of the voice would go over the top of the band: keep the ordinary fit
        const int voiceLines = used;
        const std::array<double, maxOrder + 1> sums = r;
        std::array<double, maxOrder + 1> a {}, target {}, startLags {}, startK {};
        if (! findPlaces)
            for (int k = 0; k < numHeld && used < maxFitLines; ++k)
            {
                const double w = heldAt[(size_t) k] * ratio;
                if (w >= M_PI) continue;
                lineFitFrequency[(size_t) used] = w;
                lineFitPower[(size_t) used] = heldPower[(size_t) k];
                lineFitCos[(size_t) used] = std::cos (w);
                lineFitSin[(size_t) used] = std::sin (w);
                ++used;
            }
        int walked = used;
        walkLines (voiceLines, used);
        // Twice at most, and only for the first fit of a frame: the fit, then, if it stands too tall between two harmonics, the fit
        // again with those places held down.
        for (int pass = 0; pass < 2; ++pass)
        {
        r = sums;
        if (walked < used)
        {
            walkLines (walked, used);
            walked = used;
        }
        addLines (r.data(), voiceLines, used);
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

        // Start from the ordinary fit to the lines, a little widened, on the unit gain scale the rounds use. The second time round
        // the first fit is the start, and it is already close, so it takes fewer rounds.
        if (pass == 0)
        {
            a.fill (0.0);
            startLags = r;
            const double startError = levinson (startLags.data(), startK.data());
            if (! (startError > 0.0))
                return;
            a[0] = 1.0;
            for (int i = 1; i <= p; ++i)
            {
                stepUp (a.data(), i, startK[(size_t) i - 1]);
                a[(size_t) i] = startK[(size_t) i - 1];
            }
            const double gain = 1.0 / std::sqrt (startError);
            for (int i = 0; i <= p; ++i)
                a[(size_t) i] *= gain;
        }

        for (int round = 0; round < (pass == 0 ? fitRounds : heldRounds); ++round)
        {
            // Right hand side: the model's own response read at the lines.
            target.fill (0.0);
            modelAtLines (a.data(), target.data(), used);
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

        // Between two harmonics the fit is free, and where a formant sits between them it can draw a peak 20 dB over both (measured
        // on a real take: a soft note with two strong harmonics). Move the pitch and a harmonic lands on that peak, and the note
        // comes out that much louder. No vocal tract stands that far over the harmonics either side of it, so the fit may be no
        // more than peakAllowed over the higher of the two. Where it is, that place is held down and the fit is made once more.
        if (pass > 0 || ! findPlaces)
            break;
        int added = 0;
        const auto modelAt = [&] (double w)
        {
            const double cw = std::cos (w), sw = std::sin (w);
            double c = 1.0, sn = 0.0, re = 0.0, im = 0.0;
            for (int i = 0; i <= p; ++i)
            {
                re += a[(size_t) i] * c;
                im -= a[(size_t) i] * sn;
                const double next = c * cw - sn * sw;
                sn = sn * cw + c * sw;
                c = next;
            }
            return 1.0 / (re * re + im * im + 1.0e-300);
        };
        double atLine = modelAt (lineFitFrequency[0]);
        const double holdTop = 2.0 * M_PI * holdTopHz / sr;
        for (int k = 0; k + 1 < voiceLines && used < maxFitLines && numHeld < maxHeld; ++k)
        {
            const double w0 = lineFitFrequency[(size_t) k], w1 = lineFitFrequency[(size_t) k + 1];
            if (w1 > holdTop)
                break; // a voice's power, and so a burst, is under this. Above it the check would only cost.
            const double atNext = modelAt (w1);
            // Over the harmonics either side, as the voice had them and as the fit itself draws them: a place where the fit is
            // only smoother than a rough top end is left alone.
            const double ceiling = peakAllowed * std::max (std::max (lineFitPower[(size_t) k], lineFitPower[(size_t) k + 1]), std::max (atLine, atNext));
            atLine = atNext;
            double worst = 0.0, worstAt = 0.0;
            for (double t : { 0.25, 0.5, 0.75 })
            {
                const double w = w0 + t * (w1 - w0);
                const double model = modelAt (w);
                if (model > worst) { worst = model; worstAt = w; }
            }
            if (worst > ceiling)
            {
                lineFitFrequency[(size_t) used] = worstAt;
                lineFitPower[(size_t) used] = ceiling;
                lineFitCos[(size_t) used] = std::cos (worstAt);
                lineFitSin[(size_t) used] = std::sin (worstAt);
                heldAt[(size_t) numHeld] = worstAt / ratio;
                heldPower[(size_t) numHeld] = ceiling;
                ++numHeld;
                ++used;
                ++added;
            }
        }
        if (added == 0)
            break;
        }

        // Back to lags: walk down to reflection coefficients and refuse anything unstable, then walk up to the curve's autocorrelation.
        std::array<double, maxOrder + 1> mono {}, ks {};
        const double g = 1.0 / a[0];
        for (int i = 0; i <= p; ++i) mono[(size_t) i] = a[(size_t) i] * g;
        for (int m = p; m >= 1; --m)
        {
            const double kk = mono[(size_t) m];
            if (std::abs (kk) >= 0.9999)
                return;
            ks[(size_t) m] = kk;
            const double den = 1.0 - kk * kk;
            for (int j = 1; j + j <= m; ++j)
            {
                const double low = mono[(size_t) j], high = mono[(size_t) (m - j)];
                mono[(size_t) j] = (low - kk * high) / den;
                if (j + j < m)
                    mono[(size_t) (m - j)] = (high - kk * low) / den;
            }
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
            stepUp (up.data(), m, ks[(size_t) m]);
            up[(size_t) m] = ks[(size_t) m];
            e *= 1.0 - ks[(size_t) m] * ks[(size_t) m];
        }
        for (int l = 0; l <= p; ++l) lags[l] = out[(size_t) l];
    }

    // Each line's rotating pair, cos and sin of l times its frequency for l = 0 to order, as the fit has always walked it. Eight lines
    // at a time, so the walks overlap; each walk is the same steps in the same order as one on its own.
    void walkLines (int from, int to) noexcept
    {
        const int p = order;
        int k = from;
        for (; k + 8 <= to; k += 8)
        {
            double* c0 = fitCos.data() + (size_t) k * rowStride;
            double* s0 = fitSin.data() + (size_t) k * rowStride;
            double* c1 = c0 + rowStride;
            double* s1 = s0 + rowStride;
            double* c2 = c1 + rowStride;
            double* s2 = s1 + rowStride;
            double* c3 = c2 + rowStride;
            double* s3 = s2 + rowStride;
            double* c4 = c3 + rowStride;
            double* s4 = s3 + rowStride;
            double* c5 = c4 + rowStride;
            double* s5 = s4 + rowStride;
            double* c6 = c5 + rowStride;
            double* s6 = s5 + rowStride;
            double* c7 = c6 + rowStride;
            double* s7 = s6 + rowStride;
            const double cw0 = lineFitCos[(size_t) (k + 0)], sw0 = lineFitSin[(size_t) (k + 0)];
            const double cw1 = lineFitCos[(size_t) (k + 1)], sw1 = lineFitSin[(size_t) (k + 1)];
            const double cw2 = lineFitCos[(size_t) (k + 2)], sw2 = lineFitSin[(size_t) (k + 2)];
            const double cw3 = lineFitCos[(size_t) (k + 3)], sw3 = lineFitSin[(size_t) (k + 3)];
            const double cw4 = lineFitCos[(size_t) (k + 4)], sw4 = lineFitSin[(size_t) (k + 4)];
            const double cw5 = lineFitCos[(size_t) (k + 5)], sw5 = lineFitSin[(size_t) (k + 5)];
            const double cw6 = lineFitCos[(size_t) (k + 6)], sw6 = lineFitSin[(size_t) (k + 6)];
            const double cw7 = lineFitCos[(size_t) (k + 7)], sw7 = lineFitSin[(size_t) (k + 7)];
            double x0 = 1.0, y0 = 0.0, x1 = 1.0, y1 = 0.0, x2 = 1.0, y2 = 0.0, x3 = 1.0, y3 = 0.0, x4 = 1.0, y4 = 0.0, x5 = 1.0, y5 = 0.0, x6 = 1.0, y6 = 0.0, x7 = 1.0, y7 = 0.0;
            for (int l = 0; l <= p; ++l)
            {
                c0[l] = x0; s0[l] = y0; c1[l] = x1; s1[l] = y1; c2[l] = x2; s2[l] = y2; c3[l] = x3; s3[l] = y3; c4[l] = x4; s4[l] = y4; c5[l] = x5; s5[l] = y5; c6[l] = x6; s6[l] = y6; c7[l] = x7; s7[l] = y7;
                { const double next = x0 * cw0 - y0 * sw0; y0 = y0 * cw0 + x0 * sw0; x0 = next; }
                { const double next = x1 * cw1 - y1 * sw1; y1 = y1 * cw1 + x1 * sw1; x1 = next; }
                { const double next = x2 * cw2 - y2 * sw2; y2 = y2 * cw2 + x2 * sw2; x2 = next; }
                { const double next = x3 * cw3 - y3 * sw3; y3 = y3 * cw3 + x3 * sw3; x3 = next; }
                { const double next = x4 * cw4 - y4 * sw4; y4 = y4 * cw4 + x4 * sw4; x4 = next; }
                { const double next = x5 * cw5 - y5 * sw5; y5 = y5 * cw5 + x5 * sw5; x5 = next; }
                { const double next = x6 * cw6 - y6 * sw6; y6 = y6 * cw6 + x6 * sw6; x6 = next; }
                { const double next = x7 * cw7 - y7 * sw7; y7 = y7 * cw7 + x7 * sw7; x7 = next; }
            }
        }
        for (; k < to; ++k)
        {
            double* cr = fitCos.data() + (size_t) k * rowStride;
            double* sr0 = fitSin.data() + (size_t) k * rowStride;
            const double cw = lineFitCos[(size_t) k], sw = lineFitSin[(size_t) k];
            double c = 1.0, s = 0.0;
            for (int l = 0; l <= p; ++l)
            {
                cr[l] = c; sr0[l] = s;
                const double next = c * cw - s * sw;
                s = s * cw + c * sw;
                c = next;
            }
        }
    }

    // r[l] += power * cos (l w) for the lines from, up to but not including, to, in line order.
    void addLines (double* r, int from, int to) const noexcept
    {
        const int p = order;
        for (int k = from; k < to; ++k)
        {
            const double power = lineFitPower[(size_t) k];
            const double* c = fitCos.data() + (size_t) k * rowStride;
            for (int l = 0; l <= p; ++l)
                r[l] += power * c[l];
        }
    }

    // One round's right hand side: for each line in order, the model's response there, added back over the lags. Eight lines at a
    // time, so their sums overlap; each line's sum is still taken in lag order, and the lags get the lines' parts in line order.
    void modelAtLines (const double* a, double* target, int used) const noexcept
    {
        const int p = order;
        int k = 0;
        for (; k + 8 <= used; k += 8)
        {
            const double* c0 = fitCos.data() + (size_t) k * rowStride;
            const double* s0 = fitSin.data() + (size_t) k * rowStride;
            const double* c1 = c0 + rowStride;
            const double* s1 = s0 + rowStride;
            const double* c2 = c1 + rowStride;
            const double* s2 = s1 + rowStride;
            const double* c3 = c2 + rowStride;
            const double* s3 = s2 + rowStride;
            const double* c4 = c3 + rowStride;
            const double* s4 = s3 + rowStride;
            const double* c5 = c4 + rowStride;
            const double* s5 = s4 + rowStride;
            const double* c6 = c5 + rowStride;
            const double* s6 = s5 + rowStride;
            const double* c7 = c6 + rowStride;
            const double* s7 = s6 + rowStride;
            double re0 = 0.0, im0 = 0.0, re1 = 0.0, im1 = 0.0, re2 = 0.0, im2 = 0.0, re3 = 0.0, im3 = 0.0, re4 = 0.0, im4 = 0.0, re5 = 0.0, im5 = 0.0, re6 = 0.0, im6 = 0.0, re7 = 0.0, im7 = 0.0;
            for (int i = 0; i <= p; ++i)
            {
                re0 += a[i] * c0[i]; im0 -= a[i] * s0[i];
                re1 += a[i] * c1[i]; im1 -= a[i] * s1[i];
                re2 += a[i] * c2[i]; im2 -= a[i] * s2[i];
                re3 += a[i] * c3[i]; im3 -= a[i] * s3[i];
                re4 += a[i] * c4[i]; im4 -= a[i] * s4[i];
                re5 += a[i] * c5[i]; im5 -= a[i] * s5[i];
                re6 += a[i] * c6[i]; im6 -= a[i] * s6[i];
                re7 += a[i] * c7[i]; im7 -= a[i] * s7[i];
            }
            const double d0 = re0 * re0 + im0 * im0 + 1.0e-30;
            const double hr0 = re0 / d0, hi0 = -im0 / d0;
            const double d1 = re1 * re1 + im1 * im1 + 1.0e-30;
            const double hr1 = re1 / d1, hi1 = -im1 / d1;
            const double d2 = re2 * re2 + im2 * im2 + 1.0e-30;
            const double hr2 = re2 / d2, hi2 = -im2 / d2;
            const double d3 = re3 * re3 + im3 * im3 + 1.0e-30;
            const double hr3 = re3 / d3, hi3 = -im3 / d3;
            const double d4 = re4 * re4 + im4 * im4 + 1.0e-30;
            const double hr4 = re4 / d4, hi4 = -im4 / d4;
            const double d5 = re5 * re5 + im5 * im5 + 1.0e-30;
            const double hr5 = re5 / d5, hi5 = -im5 / d5;
            const double d6 = re6 * re6 + im6 * im6 + 1.0e-30;
            const double hr6 = re6 / d6, hi6 = -im6 / d6;
            const double d7 = re7 * re7 + im7 * im7 + 1.0e-30;
            const double hr7 = re7 / d7, hi7 = -im7 / d7;
            for (int i = 0; i <= p; ++i)
            {
                target[i] += hr0 * c0[i] + hi0 * s0[i];
                target[i] += hr1 * c1[i] + hi1 * s1[i];
                target[i] += hr2 * c2[i] + hi2 * s2[i];
                target[i] += hr3 * c3[i] + hi3 * s3[i];
                target[i] += hr4 * c4[i] + hi4 * s4[i];
                target[i] += hr5 * c5[i] + hi5 * s5[i];
                target[i] += hr6 * c6[i] + hi6 * s6[i];
                target[i] += hr7 * c7[i] + hi7 * s7[i];
            }
        }
        for (; k < used; ++k)
        {
            const double* c = fitCos.data() + (size_t) k * rowStride;
            const double* s = fitSin.data() + (size_t) k * rowStride;
            double re = 0.0, im = 0.0;
            for (int i = 0; i <= p; ++i)
            {
                re += a[i] * c[i];
                im -= a[i] * s[i];
            }
            const double d = re * re + im * im + 1.0e-30;
            const double hr = re / d, hi = -im / d;
            for (int i = 0; i <= p; ++i)
                target[i] += hr * c[i] + hi * s[i];
        }
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
            gainMean += 0.25 * (1.0 - gainMean);
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
        // The gain is worked out from how well each fit says it explains its frame. When the long memory lets go (a held note that
        // moves), one frame's own fit and the average around it disagree for a moment, and the gain of that one frame jumped to
        // 2.7 and 6 times on made up and real voices: a burst of one frame (1.2.0). A voice's level is carried by the frame's
        // level, not by this ratio, so the ratio may stand no more than gainRise over its own recent average (about 10 ms). The
        // small frame to frame moves of a steady voice are well inside that and are left exactly as they were.
        if (m > 0 && f.gain > gainRise * gainMean)
            f.gain = gainRise * gainMean;
        gainMean += 0.25 * (f.gain - gainMean);
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
    std::array<double, maxFitLines> lineFitFrequency {}, lineFitCos {}, lineFitSin {}, lineFitPower {};
    static constexpr int rowStride = maxOrder + 1;
    std::vector<double> fitCos, fitSin; // per line, its rotating pair: maxFitLines rows of rowStride
    static constexpr double peakAllowed = 4.0; // 6 dB, as power
    static constexpr double holdTopHz = 4000.0;
    static constexpr int maxHeld = 32;
    std::array<double, maxHeld> heldAt {}, heldPower {};
    int numHeld = 0;
    std::array<std::array<double, maxOrder + 1>, maxOrder + 1> steps {};
    std::array<double, maxOrder + 1> errors {};
    int numLines = 0;
    static constexpr int fitRounds = 3;
    static constexpr int heldRounds = 1;
    static constexpr double lowestLinesHz = 150.0; // below this a 21 ms window does not see separate harmonics
    static constexpr double gainRise = 2.0;         // 6 dB over the recent average
    double gainMean = 1.0;
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

// Both filters for two channels at once: each channel's chain is exactly the one-channel filter's, and the two run side by side
// (1.2.4: at 96 kHz each sample waits on 80 steps in a row, so two at a time take about as long as one).
inline void latticeAnalyse2 (double x0, double x1, const double* k, double* state0, double* state1, int order, double& out0, double& out1) noexcept
{
    double f0 = x0, previous0 = x0, f1 = x1, previous1 = x1;
    for (int m = 0; m < order; ++m)
    {
        const double old0 = state0[m], old1 = state1[m];
        const double b0 = k[m] * f0 + old0;
        const double b1 = k[m] * f1 + old1;
        f0 += k[m] * old0;
        f1 += k[m] * old1;
        state0[m] = previous0;
        state1[m] = previous1;
        previous0 = b0;
        previous1 = b1;
    }
    out0 = f0;
    out1 = f1;
}

inline void latticeSynth2 (double e0, double e1, const double* k, double* state0, double* state1, int order, double& out0, double& out1) noexcept
{
    double f0 = e0, f1 = e1;
    for (int m = order - 1; m >= 0; --m)
    {
        const double old0 = state0[m], old1 = state1[m];
        f0 -= k[m] * old0;
        f1 -= k[m] * old1;
        const double b0 = k[m] * f0 + old0;
        const double b1 = k[m] * f1 + old1;
        if (m + 1 < order)
        {
            state0[m + 1] = b0;
            state1[m + 1] = b1;
        }
    }
    state0[0] = f0;
    state1[0] = f1;
    out0 = f0;
    out1 = f1;
}
} // namespace smoothmorph
