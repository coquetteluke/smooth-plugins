#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

// Formant analysis of one spectrum. Reusable on its own: no plug-in code and no knowledge of what happens to the result.
//
// Hand it a power spectrum and it finds:
//   - the fundamental and how voiced the frame is, from the autocorrelation
//   - the envelope: a curve resting on the voice's harmonics, in nepers per bin. It says how strong the sound is at each frequency.
//   - the formant model: a smooth all-pole curve fitted to the envelope, with the voice source's fall taken out. It says where the tract
//     resonates. Move this one to move formants: the gain model(new place) - model(here) flattens the old resonance and makes the new one.
//
// On a voiced frame the envelope runs exactly through the tips of the harmonics, found one by one up from the fundamental. A formant
// narrower than the harmonic spacing peaks between two harmonics, so the skirts on both sides are used to put that peak back.
// On an unvoiced frame there are no harmonics to rest on, so it is a "true envelope": the log spectrum is smoothed by cutting high
// quefrencies, then every bin that pokes out above the result is put back, and that repeats. Frames in between blend the two, so a wrong
// voicing call moves the result gently.
//
// All-pole because a tract is close to all-pole, and because such a curve can have a peak as narrow as a real formant without having to
// follow individual harmonics. Smoothing can only do one of those. Everything is real time safe after prepare().
namespace formant
{
struct PitchRange { double minHz = 60.0, maxHz = 1000.0; };

class SpectralEnvelope
{
public:
    static constexpr int maxPeaks = 5, maxTips = 256;
    static constexpr int modelFftOrder = 9, modelBins = 256, modelOrder = 18, minTipsToFit = 14;

    void prepare (double sampleRate, int fftOrder)
    {
        sr = sampleRate;
        size = 1 << fftOrder;
        half = size / 2;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);
        modelFft = std::make_unique<juce::dsp::FFT> (modelFftOrder);
        modelWork.assign ((size_t) 2 << modelFftOrder, 0.0f);
        model.assign ((size_t) half + 1, 0.0f);
        rawModel.assign ((size_t) half + 1, 0.0f);
        slope.assign ((size_t) half + 1, 0.0f);
        flatEnv.assign ((size_t) half + 1, 0.0f);
        tipCos.assign ((size_t) (maxTips * (modelOrder + 1)), 0.0);
        tipSin.assign ((size_t) (maxTips * (modelOrder + 1)), 0.0);
        tipPower.assign ((size_t) maxTips, 0.0);
        work.assign ((size_t) size * 2, 0.0f);
        logSpectrum.assign ((size_t) half + 1, 0.0f);
        shaped.assign ((size_t) half + 1, 0.0f);
        env.assign ((size_t) half + 1, 0.0f);
        scratch.assign ((size_t) half + 1, 0.0f);
        tipCurve.assign ((size_t) half + 1, 0.0f);
        autocorr.assign ((size_t) half + 1, 0.0f);

        // A windowed frame's autocorrelation falls with lag just because the window does. This is that fall, so it can be divided out.
        // Only lags up to 35% of the frame are used, where it is still large enough to divide by.
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            work[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) size));
        fft->performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= half; ++k)
        {
            const float re = work[(size_t) (2 * k)], im = work[(size_t) (2 * k + 1)];
            work[(size_t) (2 * k)] = re * re + im * im;
            work[(size_t) (2 * k + 1)] = 0.0f;
        }
        std::fill (work.begin() + size + 2, work.end(), 0.0f);
        fft->performRealOnlyInverseTransform (work.data());
        windowLag.assign ((size_t) half + 1, 1.0f);
        for (int i = 0; i <= half; ++i)
            windowLag[(size_t) i] = work[(size_t) i] / std::max (work[0], 1.0e-20f);
        maxLag = (int) (0.35 * size);
        reset();
    }

    void reset() noexcept
    {
        hasHistory = false;
        pitchHz = 0.0f;
        voicedAmount = 0.0f;
        cutSamples = (float) (0.001 * sr);
        peakCount = 0;
        std::fill (env.begin(), env.end(), 0.0f);
        std::fill (model.begin(), model.end(), 0.0f);
        std::fill (flatEnv.begin(), flatEnv.end(), 0.0f);
    }

    // power: half + 1 bins. peakWideningHz: how much the formant model's peaks are widened. More is steadier, less follows narrower formants.
    void analyse (const float* power, PitchRange range, float peakWideningHz) noexcept
    {
        trackPitch (power, range);

        // Log magnitude, with a floor so silence and deep notches between harmonics do not dominate the cepstrum.
        float biggest = 0.0f;
        for (int k = 0; k <= half; ++k)
            biggest = std::max (biggest, power[k]);
        const float floorPower = std::max (biggest * 1.0e-9f, 1.0e-16f);
        for (int k = 0; k <= half; ++k)
            logSpectrum[(size_t) k] = 0.5f * std::log (std::max (power[k], floorPower));

        const float voicedCut = pitchHz > 0.0f ? 0.5f * (float) sr / pitchHz : 0.001f * (float) sr;
        const float unvoicedCut = 0.001f * (float) sr;
        const float steadier = 1.0f / (1.0f + 0.01f * std::max (0.0f, peakWideningHz - 10.0f));
        const float wanted = juce::jlimit (0.0005f * (float) sr, 0.006f * (float) sr,
                                           steadier * unvoicedCut + voicedAmount * (voicedCut - steadier * unvoicedCut));
        cutSamples += 0.35f * (wanted - cutSamples);

        // A fixed number of rounds, so the cost and the character of the result are the same in every frame. Each round overshoots a
        // little, which reaches the tops of the harmonics in far fewer rounds than the plain method needs.
        std::copy (logSpectrum.begin(), logSpectrum.end(), shaped.begin());
        for (int round = 0; round < 4; ++round)
        {
            lifter (shaped.data(), cutSamples, scratch.data());
            for (int k = 0; k <= half; ++k)
            {
                const float above = logSpectrum[(size_t) k] - scratch[(size_t) k];
                shaped[(size_t) k] = above > 0.0f ? scratch[(size_t) k] + 1.6f * above : scratch[(size_t) k];
            }
        }

        const float harmonicShare = smoothStep (0.45f, 0.85f, voicedAmount);
        tipCount = 0;
        if (harmonicShare > 0.0f && pitchHz > 0.0f)
            restOnHarmonics (harmonicShare);

        // Below the fundamental there is no voice, so hold the level there. Otherwise rumble decides how the lowest harmonics are treated.
        const float holdHz = voicedAmount > 0.3f && pitchHz > 0.0f ? 0.9f * pitchHz : 70.0f;
        const int holdBin = juce::jlimit (1, half, (int) (holdHz * (float) size / (float) sr));
        for (int k = 0; k < holdBin; ++k)
            scratch[(size_t) k] = scratch[(size_t) holdBin];

        // Stays steady on a held sound and on noise, and follows at once when the sound really changes.
        const int lowBin = binAt (200.0), highBin = binAt (10000.0);
        float change = 0.0f;
        for (int k = lowBin; k < highBin; ++k)
            change += std::abs (scratch[(size_t) k] - env[(size_t) k]);
        change /= (float) std::max (1, highBin - lowBin);
        const float steady = 0.25f + 0.35f * harmonicShare;
        const float amount = hasHistory ? steady + (1.0f - steady) * smoothStep (0.35f, 1.0f, change) : 1.0f;
        fitModel (peakWideningHz, harmonicShare);
        for (int k = 0; k <= half; ++k)
        {
            env[(size_t) k] += amount * (scratch[(size_t) k] - env[(size_t) k]);
            model[(size_t) k] += amount * (rawModel[(size_t) k] - model[(size_t) k]);
            flatEnv[(size_t) k] += amount * (slope[(size_t) k] - flatEnv[(size_t) k]);
        }
        hasHistory = true;

        findPeaks();
    }

    // Keeps quefrencies below cut, in samples, with a short smooth fall at the end. in and out hold half + 1 values and may be the same.
    void lifter (const float* in, float cut, float* out) noexcept
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int k = 0; k <= half; ++k)
            work[(size_t) (2 * k)] = in[k];
        fft->performRealOnlyInverseTransform (work.data());
        const float flat = 0.75f * cut;
        for (int q = 0; q <= half; ++q)
        {
            float keep = 1.0f;
            if ((float) q >= cut) keep = 0.0f;
            else if ((float) q > flat) keep = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * ((float) q - flat) / (cut - flat));
            work[(size_t) q] *= keep;
            if (q > 0 && q < half) work[(size_t) (size - q)] *= keep;
        }
        std::fill (work.begin() + size, work.end(), 0.0f);
        fft->performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= half; ++k)
            out[k] = work[(size_t) (2 * k)];
    }

    const float* envelope() const noexcept   { return env.data(); }
    const float* formantModel() const noexcept { return model.data(); }
    const float* flatEnvelope() const noexcept { return flatEnv.data(); } // The envelope, on the formant model's scale: harmonic tips with the source's fall off
    float fundamentalHz() const noexcept     { return pitchHz; }
    float voicing() const noexcept           { return voicedAmount; }
    int numBins() const noexcept             { return half + 1; }
    int fftSize() const noexcept             { return size; }
    double binHz() const noexcept            { return sr / (double) size; }
    int numPeaks() const noexcept            { return peakCount; }
    float peakHz (int i) const noexcept      { return peaks[(size_t) juce::jlimit (0, maxPeaks - 1, i)]; }

private:
    static float smoothStep (float a, float b, float x) noexcept
    {
        const float t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
        return t * t * (3.0f - 2.0f * t);
    }

    int binAt (double hz) const noexcept { return juce::jlimit (0, half, (int) (hz * (double) size / sr)); }

    void trackPitch (const float* power, PitchRange range) noexcept
    {
        // Magnitude rather than power: one strong harmonic on a formant must not pass itself off as the pitch.
        std::fill (work.begin(), work.end(), 0.0f);
        for (int k = 0; k <= half; ++k)
            work[(size_t) (2 * k)] = std::sqrt (power[k]);
        fft->performRealOnlyInverseTransform (work.data());
        const float zero = work[0];
        if (zero <= 1.0e-14f)
        {
            voicedAmount *= 0.8f;
            return;
        }
        for (int i = 0; i <= maxLag; ++i)
            autocorr[(size_t) i] = work[(size_t) i] / (zero * windowLag[(size_t) i]);

        const int low = juce::jlimit (2, maxLag - 1, (int) (sr / range.maxHz));
        const int high = juce::jlimit (low + 1, maxLag - 1, (int) (sr / range.minHz));
        int best = -1;
        float bestValue = 0.0f;
        for (int i = low; i <= high; ++i)
            if (autocorr[(size_t) i] > bestValue && autocorr[(size_t) i] >= autocorr[(size_t) i - 1] && autocorr[(size_t) i] >= autocorr[(size_t) i + 1])
            {
                best = i;
                bestValue = autocorr[(size_t) i];
            }
        if (best < 0)
        {
            voicedAmount *= 0.8f;
            return;
        }

        // A period is also found at two and three times its length. Take the shortest that is nearly as strong: a period judged too
        // short only makes the envelope smoother, while one judged too long would let harmonics into it.
        for (int divide : { 4, 3, 2 })
        {
            const int centre = (int) std::round ((float) best / (float) divide);
            if (centre < low) continue;
            int found = centre;
            float value = autocorr[(size_t) centre];
            for (int i = centre - 2; i <= centre + 2; ++i)
                if (i >= low && i <= maxLag - 1 && autocorr[(size_t) i] > value)
                {
                    found = i;
                    value = autocorr[(size_t) i];
                }
            if (value >= 0.93f * bestValue)
            {
                best = found;
                bestValue = value;
                break;
            }
        }

        const float y0 = autocorr[(size_t) best - 1], y1 = autocorr[(size_t) best], y2 = autocorr[(size_t) best + 1];
        const float bend = y0 - 2.0f * y1 + y2;
        const float lag = (float) best + (std::abs (bend) > 1.0e-9f ? juce::jlimit (-1.0f, 1.0f, 0.5f * (y0 - y2) / bend) : 0.0f);
        const float heard = (float) sr / std::max (lag, 1.0f);

        const float strength = juce::jlimit (0.0f, 1.0f, (bestValue - 0.4f) / 0.3f);
        voicedAmount += 0.4f * (strength - voicedAmount);
        if (strength > 0.2f)
        {
            if (pitchHz <= 0.0f || std::abs (std::log (heard / pitchHz)) > 0.12f)
                pitchHz = heard; // a new note
            else
                pitchHz *= std::exp (0.5f * std::log (heard / pitchHz));
        }
    }

    // Replaces the smooth envelope in scratch with one through the harmonics' tips, by the given share.
    void restOnHarmonics (float share) noexcept
    {
        const float spacing = pitchHz * (float) size / (float) sr;
        if (spacing < 2.5f) return;
        const int topBin = binAt (9000.0);
        const auto& l = logSpectrum;

        float next = spacing;
        int tips = 0;
        while (next < (float) topBin && tips < maxTips)
        {
            const int from = std::max (1, (int) std::ceil (next - 0.35f * spacing)), to = std::min (half - 1, (int) std::floor (next + 0.35f * spacing));
            int best = from;
            for (int k = from; k <= to; ++k)
                if (l[(size_t) k] > l[(size_t) best]) best = k;
            // The parabola also corrects the height, since a harmonic between two bins reads a little low.
            const float y0 = l[(size_t) best - 1], y1 = l[(size_t) best], y2 = l[(size_t) best + 1];
            const float bend = y0 - 2.0f * y1 + y2;
            const float offset = bend < -1.0e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (y0 - y2) / bend) : 0.0f;
            tipBin[(size_t) tips] = (float) best + offset;
            tipLevel[(size_t) tips] = y1 - 0.25f * (y0 - y2) * offset;
            next = tipBin[(size_t) tips] + spacing;
            ++tips;
        }
        tipCount = tips;
        if (tips == 0) return;

        // A formant narrower than the harmonic spacing peaks between two harmonics, higher than either. The skirts on both sides say
        // where: carry the line through the two tips on the left forward and the line through the two tips on the right back, and where
        // they meet is the peak. Without this the peak's real height is lost whenever no harmonic lands on it, and that is most of the time.
        int points = 0;
        const int apexTop = binAt (5500.0);
        for (int t = 0; t < tips && points < maxTips * 2 - 1; ++t)
        {
            curveAt[(size_t) points] = tipBin[(size_t) t];
            curveLevel[(size_t) points] = tipLevel[(size_t) t];
            ++points;
            if (t < 1 || t + 2 >= tips || tipBin[(size_t) t] > (float) apexTop) continue;
            const float x1 = tipBin[(size_t) t], x2 = tipBin[(size_t) t + 1];
            const float rise = (tipLevel[(size_t) t] - tipLevel[(size_t) t - 1]) / std::max (x1 - tipBin[(size_t) t - 1], 1.0f);
            const float fall = (tipLevel[(size_t) t + 2] - tipLevel[(size_t) t + 1]) / std::max (tipBin[(size_t) t + 2] - x2, 1.0f);
            if (rise <= 0.0f || fall >= 0.0f) continue;
            // Left line: y1 + rise (x - x1). Right line: y2 + fall (x - x2).
            const float meet = (tipLevel[(size_t) t + 1] - tipLevel[(size_t) t] + rise * x1 - fall * x2) / (rise - fall);
            if (meet <= x1 + 0.5f || meet >= x2 - 0.5f) continue;
            const float height = tipLevel[(size_t) t] + rise * (meet - x1);
            const float taller = std::max (tipLevel[(size_t) t], tipLevel[(size_t) t + 1]);
            if (height < taller + 0.05f) continue;
            curveAt[(size_t) points] = meet;
            curveLevel[(size_t) points] = std::min (height, taller + 1.4f);
            ++points;
        }

        for (int k = 0, firstBin = (int) curveAt[0]; k <= firstBin; ++k)
            tipCurve[(size_t) k] = curveLevel[0];

        // A smooth curve through all those points.
        for (int i = 0; i + 1 < points; ++i)
        {
            const float x0 = curveAt[(size_t) i], x1 = curveAt[(size_t) i + 1];
            const float y0 = curveLevel[(size_t) i], y1 = curveLevel[(size_t) i + 1];
            const float gap = std::max (x1 - x0, 0.01f);
            const float before = i > 0 ? (y1 - curveLevel[(size_t) i - 1]) / std::max (x1 - curveAt[(size_t) i - 1], 0.01f) : (y1 - y0) / gap;
            const float after = i + 2 < points ? (curveLevel[(size_t) i + 2] - y0) / std::max (curveAt[(size_t) i + 2] - x0, 0.01f) : (y1 - y0) / gap;
            const float m0 = before * gap, m1 = after * gap;
            for (int k = (int) x0 + 1; k <= (int) x1; ++k)
            {
                const float u = juce::jlimit (0.0f, 1.0f, ((float) k - x0) / gap);
                const float u2 = u * u, u3 = u2 * u;
                tipCurve[(size_t) k] = (2.0f * u3 - 3.0f * u2 + 1.0f) * y0 + (u3 - 2.0f * u2 + u) * m0 + (3.0f * u2 - 2.0f * u3) * y1 + (u3 - u2) * m1;
            }
        }

        // Above the last harmonic looked at, ease back onto the smooth envelope.
        const int lastBin = (int) tipBin[(size_t) tips - 1];
        const float offsetAtEnd = tipLevel[(size_t) tips - 1] - scratch[(size_t) lastBin];
        const float easeBins = 1000.0f / (float) binHz();
        for (int k = lastBin + 1; k <= half; ++k)
            tipCurve[(size_t) k] = scratch[(size_t) k] + offsetAtEnd * std::max (0.0f, 1.0f - (float) (k - lastBin) / easeBins);

        for (int k = 0; k <= half; ++k)
            scratch[(size_t) k] += share * (tipCurve[(size_t) k] - scratch[(size_t) k]);
    }

    // All-pole fit to the envelope in scratch, up to 8 kHz, where formants live. The envelope's overall slope comes off first: an all-pole
    // fit spends itself on the loudest peaks, and without this the upper formants of a low voice get nothing. That slope is mostly the
    // voice source anyway, so it also stays out of what the map moves. Above 8 kHz the flattened envelope itself is used.
    void fitModel (float peakWideningHz, float harmonicShare) noexcept
    {
        constexpr float nepersPerOctave = -6.0f * 0.115129f;
        const float fall = nepersPerOctave * smoothStep (0.3f, 0.8f, voicedAmount);
        const auto nepersFall = [fall, this] (float bin)
        {
            const float f = bin / std::max (1.0f, 1.5f * std::max (pitchHz, 60.0f) / (float) binHz());
            return fall * 0.5f * std::log2 (1.0f + f * f);
        };
        const float cornerBin = std::max (1.0f, 1.5f * std::max (pitchHz, 60.0f) / (float) binHz());
        for (int k = 0; k <= half; ++k)
        {
            const float f = (float) k / cornerBin;
            slope[(size_t) k] = scratch[(size_t) k] - fall * 0.5f * std::log2 (1.0f + f * f); // Now the flattened envelope
        }

        // Onto the fit's own grid: modelBins steps from 0 Hz to the top of the band.
        const float topBin = (float) std::min (8000.0 / binHz(), (double) half - 1.0);
        const float step = topBin / (float) modelBins;
        float top = -1.0e9f;
        for (int k = 0; k <= modelBins; ++k)
        {
            const float at = (float) k * step;
            const int a = std::min ((int) at, half - 1);
            const float v = slope[(size_t) a] + (at - (float) a) * (slope[(size_t) a + 1] - slope[(size_t) a]);
            modelWork[(size_t) (2 * k)] = v;
            top = std::max (top, v);
        }
        for (int k = 0; k <= modelBins; ++k)
        {
            modelWork[(size_t) (2 * k)] = std::exp (2.0f * juce::jmax (-9.0f, modelWork[(size_t) (2 * k)] - top));
            modelWork[(size_t) (2 * k + 1)] = 0.0f;
        }
        std::fill (modelWork.begin() + (2 * modelBins + 2), modelWork.end(), 0.0f);
        modelFft->performRealOnlyInverseTransform (modelWork.data());

        // Autocorrelation to predictor. The lag window widens every peak by the same amount in Hz, without having to find the poles.
        std::array<double, modelOrder + 1> r, a, previous;
        const double bandHz = (double) topBin * binHz();
        for (int n = 0; n <= modelOrder; ++n)
        {
            const double w = juce::MathConstants<double>::pi * peakWideningHz * n / bandHz;
            r[(size_t) n] = (double) modelWork[(size_t) n] * std::exp (-0.5 * w * w);
        }
        r[0] = r[0] * 1.00001 + 1.0e-12;
        double error = r[0];
        a.fill (0.0);
        a[0] = 1.0;
        for (int i = 1; i <= modelOrder; ++i)
        {
            double sum = r[(size_t) i];
            for (int j = 1; j < i; ++j)
                sum += a[(size_t) j] * r[(size_t) (i - j)];
            const double reflection = -sum / error;
            previous = a;
            for (int j = 1; j < i; ++j)
                a[(size_t) j] = previous[(size_t) j] + reflection * previous[(size_t) (i - j)];
            a[(size_t) i] = reflection;
            error *= 1.0 - reflection * reflection;
            if (error <= 1.0e-14) break;
        }

        const double gain = std::sqrt (std::max (error, 1.0e-14));
        for (auto& v : a) v /= gain; // From here on the curve is 1 / |A|^2 with no separate gain

        if (harmonicShare > 0.5f && tipCount >= minTipsToFit)
            fitToHarmonics (a, topBin, peakWideningHz, top, nepersFall);

        std::fill (modelWork.begin(), modelWork.end(), 0.0f);
        for (int n = 0; n <= modelOrder; ++n)
            modelWork[(size_t) n] = (float) a[(size_t) n];
        modelFft->performRealOnlyForwardTransform (modelWork.data());
        const float level = top;
        const auto fitAt = [&] (int i)
        {
            const float re = modelWork[(size_t) (2 * i)], im = modelWork[(size_t) (2 * i + 1)];
            return level - 0.5f * std::log (re * re + im * im + 1.0e-12f);
        };

        // Back onto the envelope's grid, then over to the flattened envelope itself across the top 15% of the band.
        const int lastBin = (int) topBin;
        const int blendFrom = (int) (0.85f * topBin);
        const float endOffset = fitAt (modelBins) - slope[(size_t) lastBin];
        for (int k = 0; k <= half; ++k)
        {
            const float above = slope[(size_t) k] + endOffset;
            if (k > lastBin) { rawModel[(size_t) k] = above; continue; }
            const float at = (float) k / step;
            const int i = std::min ((int) at, modelBins - 1);
            const float y0 = fitAt (i), y1 = fitAt (i + 1);
            const float fit = y0 + (at - (float) i) * (y1 - y0);
            const float t = k <= blendFrom ? 0.0f : (float) (k - blendFrom) / (float) std::max (1, lastBin - blendFrom);
            rawModel[(size_t) k] = fit + t * (above - fit);
        }
    }

    // A voice's spectrum exists only at its harmonics. An ordinary all-pole fit also has to explain the gaps between them, and so it leans
    // towards whichever harmonic is nearest a formant. This fit answers to the harmonics alone (discrete all-pole modelling, after
    // El-Jaroudi and Makhoul), which finds a resonance's real place even when harmonics straddle it. The start is the ordinary fit.
    template <typename Fall>
    void fitToHarmonics (std::array<double, modelOrder + 1>& a, float topBin, float peakWideningHz, float top, Fall nepersFall) noexcept
    {
        const int p = modelOrder;
        int count = 0;
        const double bandHz = (double) topBin * binHz();
        for (int l = 0; l < tipCount && tipBin[(size_t) l] < topBin; ++l)
        {
            const double omega = juce::MathConstants<double>::pi * (double) tipBin[(size_t) l] / (double) topBin;
            const double c1 = std::cos (omega), s1 = std::sin (omega);
            double c = 1.0, s = 0.0;
            for (int n = 0; n <= p; ++n)
            {
                tipCos[(size_t) (l * (p + 1) + n)] = c;
                tipSin[(size_t) (l * (p + 1) + n)] = s;
                const double nextC = c * c1 - s * s1;
                s = s * c1 + c * s1;
                c = nextC;
            }
            const float flat = tipLevel[(size_t) l] - nepersFall (tipBin[(size_t) l]);
            tipPower[(size_t) l] = std::exp (2.0 * (double) juce::jmax (-9.0f, flat - top));
            ++count;
        }
        if (count < minTipsToFit) return;

        std::array<double, modelOrder + 1> r;
        for (int n = 0; n <= p; ++n)
        {
            double sum = 0.0;
            for (int l = 0; l < count; ++l)
                sum += tipPower[(size_t) l] * tipCos[(size_t) (l * (p + 1) + n)];
            const double w = juce::MathConstants<double>::pi * peakWideningHz * n / bandHz;
            r[(size_t) n] = sum / count * std::exp (-0.5 * w * w);
        }
        r[0] = r[0] * 1.0005 + 1.0e-12;

        // The matrix never changes during the fit, so it is factored once.
        std::array<std::array<double, modelOrder + 1>, modelOrder + 1> chol {};
        for (int i = 0; i <= p; ++i)
            for (int j = 0; j <= i; ++j)
            {
                double sum = r[(size_t) (i - j)];
                for (int k = 0; k < j; ++k)
                    sum -= chol[(size_t) i][(size_t) k] * chol[(size_t) j][(size_t) k];
                if (i == j)
                {
                    if (sum <= 1.0e-14) return; // Not enough harmonics to pin this down: keep the ordinary fit.
                    chol[(size_t) i][(size_t) i] = std::sqrt (sum);
                }
                else
                    chol[(size_t) i][(size_t) j] = sum / chol[(size_t) j][(size_t) j];
            }

        for (int round = 0; round < 6; ++round)
        {
            std::array<double, modelOrder + 1> h;
            h.fill (0.0);
            for (int l = 0; l < count; ++l)
            {
                const double* cs = tipCos.data() + l * (p + 1);
                const double* sn = tipSin.data() + l * (p + 1);
                double ar = 0.0, ai = 0.0;
                for (int k = 0; k <= p; ++k)
                {
                    ar += a[(size_t) k] * cs[k];
                    ai += a[(size_t) k] * sn[k];
                }
                const double size2 = ar * ar + ai * ai + 1.0e-12;
                for (int i = 0; i <= p; ++i)
                    h[(size_t) i] += (cs[i] * ar + sn[i] * ai) / size2;
            }
            for (auto& v : h) v /= count;

            std::array<double, modelOrder + 1> x;
            for (int i = 0; i <= p; ++i)
            {
                double sum = h[(size_t) i];
                for (int k = 0; k < i; ++k) sum -= chol[(size_t) i][(size_t) k] * x[(size_t) k];
                x[(size_t) i] = sum / chol[(size_t) i][(size_t) i];
            }
            for (int i = p; i >= 0; --i)
            {
                double sum = x[(size_t) i];
                for (int k = i + 1; k <= p; ++k) sum -= chol[(size_t) k][(size_t) i] * x[(size_t) k];
                x[(size_t) i] = sum / chol[(size_t) i][(size_t) i];
            }
            for (int i = 0; i <= p; ++i)
                a[(size_t) i] += 0.7 * (x[(size_t) i] - a[(size_t) i]);
        }
    }

    void findPeaks() noexcept
    {
        peakCount = 0;
        const int low = std::max (2, binAt (150.0)), high = std::min (half - 2, binAt (5500.0));
        float valley = model[(size_t) low];
        float lastPeak = -1.0e6f;
        for (int k = low; k <= high && peakCount < maxPeaks; ++k)
        {
            const float y0 = model[(size_t) k - 1], y1 = model[(size_t) k], y2 = model[(size_t) k + 1];
            valley = std::min (valley, y1);
            // Counts as a formant only when it stands 3 dB above the valley before it and well clear of the formant before.
            if (y1 > y0 && y1 >= y2 && y1 - valley > 0.35f && ((float) k - lastPeak) * (float) binHz() > 250.0f)
            {
                const float bend = y0 - 2.0f * y1 + y2;
                const float at = (float) k + (std::abs (bend) > 1.0e-9f ? juce::jlimit (-1.0f, 1.0f, 0.5f * (y0 - y2) / bend) : 0.0f);
                peaks[(size_t) peakCount++] = at * (float) binHz();
                lastPeak = (float) k;
                valley = y1;
            }
        }
    }

    double sr = 48000.0;
    int size = 2048, half = 1024, maxLag = 700;
    std::unique_ptr<juce::dsp::FFT> fft, modelFft;
    std::vector<float> work, logSpectrum, shaped, env, scratch, tipCurve, autocorr, windowLag, modelWork, model, rawModel, slope, flatEnv;
    std::array<float, maxPeaks> peaks {};
    std::array<float, maxTips> tipBin {}, tipLevel {};
    std::array<float, maxTips * 2> curveAt {}, curveLevel {};
    std::vector<double> tipCos, tipSin, tipPower;
    int tipCount = 0;
    int peakCount = 0;
    float pitchHz = 0.0f, voicedAmount = 0.0f, cutSamples = 48.0f;
    bool hasHistory = false;
};
} // namespace formant
