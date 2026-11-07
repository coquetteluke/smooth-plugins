#include "CombField.h"
#include <Accelerate/Accelerate.h>
#include <complex>

namespace ui
{
    namespace
    {
        constexpr float kMinHz = 40.0f, kMaxHz = 20000.0f;
        constexpr float kAxisPower = 0.7f;      // opens up the middle, where the ear is most sensitive to small moves
        constexpr float kEarliestMs = 5.25f;    // as far early as the wet can go
        constexpr float kMaxFlangeMs = 5.0f;
        constexpr float kLoopMinMs = 0.25f;     // the processor's shortest feedback loop
        constexpr double kRebuildSeconds = 1.0 / 15.0;
        constexpr double kSettledSeconds = 0.15; // no change for this long and the picture is worth working out in full
        constexpr float kWakeFadeSeconds = 1.5f;
        constexpr float kSnapPoints = 4.0f;     // this close to the middle lands on exactly 0, which is what takes the wet out

        float octavesSpan() { return std::log2 (kMaxHz / kMinHz); }

        juce::PixelARGB premultiplied (juce::Colour c, float alpha)
        {
            auto p = c.withAlpha (alpha).getPixelARGB();
            return p;
        }

        /** Colours for the steel picture. Index 0 is a deep notch, about 210 is level, 255 is 6 dB up. */
        struct SteelRamp
        {
            juce::PixelARGB lit[256], dim[256];

            SteelRamp()
            {
                const juce::Colour stops[] = { juce::Colour (0xff0b0e13), juce::Colour (0xff161c24), juce::Colour (0xff36424f), juce::Colour (0xff66768a) };
                const float at[] = { 0.0f, 0.45f, 0.84f, 1.0f };

                for (int i = 0; i < 256; ++i)
                {
                    const float t = (float) i / 255.0f;
                    int s = 0;
                    while (s < 2 && t > at[s + 1])
                        ++s;
                    const auto c = stops[s].interpolatedWith (stops[s + 1], (t - at[s]) / (at[s + 1] - at[s]));
                    lit[i] = c.getPixelARGB();
                    dim[i] = c.interpolatedWith (juce::Colour (0xff10151b), 0.6f).getPixelARGB();
                }
            }
        };
    }

    bool CombField::Settings::operator== (const Settings& o) const noexcept
    {
        return feedback == o.feedback && mix == o.mix && tone == o.tone && lowCut == o.lowCut && highCut == o.highCut && depth == o.depth
            && delay == o.delay && drift == o.drift && invert == o.invert && comp == o.comp && manual == o.manual && bypass == o.bypass;
    }

    CombField::CombField (PluginProcessor& p)
        : processor (p),
          flangeParam (*p.apvts.getParameter ("flange")),
          depthParam (*p.apvts.getParameter ("depth"))
    {
        setOpaque (true);
        setComponentID ("field");
        setTitle ("Comb field");

        auto& s = p.apvts;
        feedbackRaw = s.getRawParameterValue ("feedback");
        mixRaw = s.getRawParameterValue ("mix");
        toneRaw = s.getRawParameterValue ("tone");
        lowCutRaw = s.getRawParameterValue ("lowcut");
        highCutRaw = s.getRawParameterValue ("highcut");
        depthRaw = s.getRawParameterValue ("depth");
        delayRaw = s.getRawParameterValue ("delay");
        wowRaw = s.getRawParameterValue ("wowdepth");
        flutterRaw = s.getRawParameterValue ("flutterdepth");
        randomRaw = s.getRawParameterValue ("randomdepth");
        flangeRaw = s.getRawParameterValue ("flange");
        invertRaw = s.getRawParameterValue ("invert");
        compRaw = s.getRawParameterValue ("feedbackcomp");
        modeRaw = s.getRawParameterValue ("mode");
        bypassRaw = s.getRawParameterValue ("bypass");

        wake.assign ((size_t) (wakeColumns * wakeRows), 0.0f);
        combMask.assign ((size_t) (wakeColumns * wakeRows), 1.0f);
        wakeImage = juce::Image (juce::Image::ARGB, wakeColumns, wakeRows, true, juce::NativeImageType());
        column[0].assign ((size_t) wakeRows, 0.0f);
        column[1].assign ((size_t) wakeRows, 0.0f);

        // From nothing, through deep violet, to nearly white where the sound is loudest.
        struct Stop { float at; juce::Colour colour; float alpha; };
        const Stop stops[] = { { 0.00f, juce::Colour (0xff2b1f73), 0.0f },
                               { 0.26f, juce::Colour (0xff4a35b8), 0.30f },
                               { 0.50f, juce::Colour (0xff7a56ee), 0.88f },
                               { 0.78f, juce::Colour (0xffb49dff), 1.0f },
                               { 1.00f, juce::Colour (0xfff3eeff), 1.0f } };

        for (int i = 0; i < 256; ++i)
        {
            const float t = (float) i / 255.0f;
            int s2 = 0;
            while (s2 < 3 && t > stops[s2 + 1].at)
                ++s2;
            const float f = (t - stops[s2].at) / (stops[s2 + 1].at - stops[s2].at);
            const auto c = stops[s2].colour.interpolatedWith (stops[s2 + 1].colour, f);
            const float a = stops[s2].alpha + f * (stops[s2 + 1].alpha - stops[s2].alpha);
            wakeColours[(size_t) i] = premultiplied (c, a);
            wakeColours[(size_t) i].premultiply();
        }

        settings = readSettings();
        updateAxis (settings);
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    }

    CombField::~CombField()
    {
        finishGesture();
    }

    CombField::Settings CombField::readSettings() const
    {
        Settings s;
        s.feedback = feedbackRaw->load (std::memory_order_relaxed);
        s.mix = mixRaw->load (std::memory_order_relaxed);
        s.tone = toneRaw->load (std::memory_order_relaxed);
        s.lowCut = lowCutRaw->load (std::memory_order_relaxed);
        s.highCut = highCutRaw->load (std::memory_order_relaxed);
        s.depth = depthRaw->load (std::memory_order_relaxed);
        s.delay = delayRaw->load (std::memory_order_relaxed);
        s.drift = wowRaw->load (std::memory_order_relaxed) + flutterRaw->load (std::memory_order_relaxed) + randomRaw->load (std::memory_order_relaxed);
        s.invert = invertRaw->load (std::memory_order_relaxed) > 0.5f;
        s.comp = compRaw->load (std::memory_order_relaxed) > 0.5f;
        s.manual = modeRaw->load (std::memory_order_relaxed) > 0.5f;
        s.bypass = bypassRaw->load (std::memory_order_relaxed) > 0.5f;
        return s;
    }

    void CombField::updateAxis (const Settings& s)
    {
        // Zero stays in the middle. The late side stretches when Delay pushes the sweep past 5 ms.
        const float reach = s.delay + (s.manual ? kMaxFlangeMs : s.depth) + s.drift;
        const float right = juce::jmax (kEarliestMs, reach * 1.04f);

        if (std::abs (right - rightMs) > 0.001f)
        {
            rightMs = right;
            std::fill (wake.begin(), wake.end(), 0.0f);
            wakePeak = 0.0f;
        }

        leftMs = kEarliestMs;
    }

    float CombField::uOfMs (float ms) const noexcept
    {
        return ms >= 0.0f ? std::pow (juce::jmin (ms / rightMs, 1.0f), kAxisPower)
                          : -std::pow (juce::jmin (-ms / leftMs, 1.0f), kAxisPower);
    }

    float CombField::msOfU (float u) const noexcept
    {
        return u >= 0.0f ? rightMs * std::pow (u, 1.0f / kAxisPower)
                         : -leftMs * std::pow (-u, 1.0f / kAxisPower);
    }

    float CombField::xOfMs (float ms) const noexcept
    {
        const auto half = (float) getWidth() * 0.5f;
        return half + uOfMs (ms) * half;
    }

    float CombField::msOfX (float x) const noexcept
    {
        const auto half = (float) getWidth() * 0.5f;
        return msOfU (juce::jlimit (-1.0f, 1.0f, (x - half) / juce::jmax (1.0f, half)));
    }

    float CombField::yOfHz (float hz) const noexcept
    {
        return (float) plotBounds().getHeight() * std::log2 (kMaxHz / hz) / octavesSpan();
    }

    void CombField::resized()
    {
        baseDirty = true;
        repaint();
    }

    void CombField::finishPicture()
    {
        lastChange = 0.0;
        if (baseIsDraft)
            baseDirty = true;
    }

    void CombField::setHoldPicture (bool shouldHold)
    {
        holdPicture = shouldHold;
        if (! shouldHold)
        {
            baseDirty = true;
            repaint();
        }
    }

    // ---- what the settings predict ------------------------------------------------------------------------------

    namespace
    {
        /** Everything about the wet path that depends only on frequency. The same model for the picture and the proof. */
        struct WetModel
        {
            float dryGain, wetMix, polarity, loopScale, feedback;

            float wetMagnitude (float hz, float tone, float lowCut, float highCut, bool lowOn, bool highOn) const noexcept
            {
                const float a = std::pow (10.0f, tone / 20.0f), r = hz * hz / (hz * hz + 16.0e6f);
                float g = 1.0f + (a - 1.0f) * r;   // 4 kHz shelf, close enough to draw
                if (highOn) g *= 1.0f / (1.0f + hz * hz / (highCut * highCut));
                if (lowOn)  g *= 1.0f / (1.0f + lowCut * lowCut / (hz * hz));
                return g;
            }

            float loopGain (float hz) const noexcept
            {
                return feedback * std::sqrt (1.0f / (1.0f + hz * hz / 144.0e6f)) * std::sqrt (1.0f / (1.0f + 400.0f / (hz * hz)));
            }
        };

        WetModel modelFrom (float feedbackPct, float mixPct, bool invert, bool comp, bool bypass)
        {
            WetModel w;
            const float fb = feedbackPct * 0.01f;
            const float m = bypass ? 0.0f : mixPct * 0.01f;
            w.dryGain = 1.0f - m;
            w.wetMix = m;
            w.polarity = (fb < 0.0f ? -1.0f : 1.0f) * (invert ? -1.0f : 1.0f);
            w.feedback = fb;
            w.loopScale = comp ? std::sqrt (juce::jmax (0.0f, 1.0f - fb * fb)) : 1.0f;
            return w;
        }
    }

    float CombField::predictedDb (float hz, float ms) const
    {
        const auto s = settings;
        const auto w = modelFrom (s.feedback, s.mix, s.invert, s.comp, s.bypass);
        const float tau = ms * 0.001f, loop = std::sqrt (tau * tau + kLoopMinMs * kLoopMinMs * 1.0e-6f);
        const float omega = juce::MathConstants<float>::twoPi * hz;
        const float g = w.loopGain (hz), wetGain = w.polarity * w.loopScale * w.wetMagnitude (hz, s.tone, s.lowCut, s.highCut, s.lowCut > 20.5f, s.highCut < 19999.5f);
        const std::complex<float> line = std::complex<float> (wetGain, 0.0f) * std::polar (1.0f, -omega * tau) / (std::complex<float> (1.0f, 0.0f) - g * std::polar (1.0f, -omega * loop));
        const auto h = w.dryGain + w.wetMix * line;
        return 10.0f * std::log10 (juce::jmax (1.0e-6f, std::norm (h)));
    }

    float CombField::paintedAt (float hz, float ms) const
    {
        const int col = juce::jlimit (0, wakeColumns - 1, (int) ((uOfMs (ms) + 1.0f) * 0.5f * (float) wakeColumns));
        const int row = juce::jlimit (0, wakeRows - 1, (int) (std::log2 (kMaxHz / hz) / octavesSpan() * (float) wakeRows));
        return wake[(size_t) (row * wakeColumns + col)];
    }

    namespace
    {
        /** Works out the comb, in dB x 0.1, for one delay at every row of a grid. All the buffers are one per row. */
        struct CombColumn
        {
            CombColumn (const WetModel& m, int rowsToUse, float pixelsPerOctaveToUse)
                : model (m), rows (rowsToUse), pixelsPerOctave (pixelsPerOctaveToUse)
            {
                const auto n = (size_t) rows;
                for (auto* v : { &omega, &wetGain, &loopG, &meanP, &hzRow, &phase, &sinW, &cosW, &sinL, &cosL, &power })
                    v->resize (n);
            }

            void setRow (int r, float hz, float tone, float lowCut, float highCut)
            {
                const auto i = (size_t) r;
                hzRow[i] = hz;
                omega[i] = juce::MathConstants<float>::twoPi * hz;
                wetGain[i] = model.polarity * model.loopScale * model.wetMagnitude (hz, tone, lowCut, highCut, lowCut > 20.5f, highCut < 19999.5f);
                loopG[i] = model.loopGain (hz);
                const float wetMean = wetGain[i] * wetGain[i] / juce::jmax (0.01f, 1.0f - loopG[i] * loopG[i]);
                meanP[i] = model.dryGain * model.dryGain + model.wetMix * model.wetMix * wetMean;
            }

            void compute (float ms)
            {
                const float tau = ms * 0.001f, absTau = std::abs (tau);
                const float loop = std::sqrt (tau * tau + kLoopMinMs * kLoopMinMs * 1.0e-6f);

                for (int r = 0; r < rows; ++r)
                    phase[(size_t) r] = omega[(size_t) r] * tau;
                vvsincosf (sinW.data(), cosW.data(), phase.data(), &rows);

                for (int r = 0; r < rows; ++r)
                    phase[(size_t) r] = omega[(size_t) r] * loop;
                vvsincosf (sinL.data(), cosL.data(), phase.data(), &rows);

                for (int r = 0; r < rows; ++r)
                {
                    const size_t i = (size_t) r;
                    const float g = loopG[i], w = wetGain[i];
                    // wet = w e^-jwt / (1 - g e^-jwL)
                    const float dr = 1.0f - g * cosL[i], di = g * sinL[i];
                    const float nr = w * cosW[i], ni = -w * sinW[i];
                    const float dd = dr * dr + di * di;
                    const float wr = (nr * dr + ni * di) / dd, wi = (ni * dr - nr * di) / dd;
                    const float hr = model.dryGain + model.wetMix * wr, hq = model.wetMix * wi;
                    const float p = hr * hr + hq * hq;

                    // Fringes closer than a couple of pixels would only alias, so they ease to their average.
                    const float spacing = absTau > 1.0e-7f ? (pixelsPerOctave / 0.6931f) / (absTau * hzRow[i]) : 1.0e6f;
                    const float keep = juce::jlimit (0.0f, 1.0f, (spacing - 2.0f) * 0.5f);
                    power[i] = keep * p + (1.0f - keep) * meanP[i] + 1.0e-5f;
                }

                vvlog10f (power.data(), power.data(), &rows);
            }

            WetModel model;
            int rows;
            float pixelsPerOctave;
            std::vector<float> omega, wetGain, loopG, meanP, hzRow, phase, sinW, cosW, sinL, cosL, power;
        };
    }

    void CombField::rebuildMask()
    {
        const auto& s = settings;
        const float span = octavesSpan();
        CombColumn comb (modelFrom (s.feedback, s.mix, s.invert, s.comp, s.bypass), wakeRows, (float) wakeRows / span);

        for (int r = 0; r < wakeRows; ++r)
            comb.setRow (r, kMaxHz * std::pow (2.0f, -span * ((float) r + 0.5f) / (float) wakeRows), s.tone, s.lowCut, s.highCut);

        combMask.resize (wake.size());

        for (int c = 0; c < wakeColumns; ++c)
        {
            comb.compute (msOfU (((float) c + 0.5f) / (float) wakeColumns * 2.0f - 1.0f));

            for (int r = 0; r < wakeRows; ++r)
                combMask[(size_t) (r * wakeColumns + c)] = juce::jlimit (0.0f, 1.0f, (comb.power[(size_t) r] * 10.0f + 18.0f) / 18.0f);
        }
    }

    void CombField::rebuildBase (float scale, bool draft)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const float pixels = draft ? scale * 0.5f : scale;
        const int W = juce::jmax (4, juce::roundToInt ((float) getWidth() * pixels));
        const int plotH = juce::jmax (2, juce::roundToInt ((float) plotBounds().getHeight() * pixels));

        if (! base.isValid() || base.getWidth() != W || base.getHeight() != plotH)
            base = juce::Image (juce::Image::ARGB, W, plotH, false, juce::NativeImageType());

        static const SteelRamp ramp;
        const auto& s = settings;

        // Sweep range, as far as the settings can take the wet deck. Everything outside it is dimmed.
        const float lo = s.manual ? -kEarliestMs : juce::jmax (-kEarliestMs, s.delay - s.depth - s.drift);
        const float hi = s.manual ? rightMs : s.delay + s.depth + s.drift;

        const float span = octavesSpan();
        CombColumn comb (modelFrom (s.feedback, s.mix, s.invert, s.comp, s.bypass), plotH, (float) plotH / span);

        for (int r = 0; r < plotH; ++r)
            comb.setRow (r, kMaxHz * std::pow (2.0f, -span * ((float) r + 0.5f) / (float) plotH), s.tone, s.lowCut, s.highCut);

        const float halfPixels = (float) W * 0.5f;

        {
            juce::Image::BitmapData data (base, juce::Image::BitmapData::writeOnly);

            for (int c = 0; c < W; ++c)
            {
                const float ms = msOfU (((float) c + 0.5f - halfPixels) / halfPixels);
                comb.compute (ms);

                const bool inRange = ms >= lo - 0.001f && ms <= hi + 0.001f;
                const auto* colours = inRange ? ramp.lit : ramp.dim;

                for (int r = 0; r < plotH; ++r)
                {
                    const float db = comb.power[(size_t) r] * 10.0f;
                    const int index = juce::jlimit (0, 255, (int) ((db + 30.0f) / 36.0f * 255.0f));
                    *(juce::PixelARGB*) data.getPixelPointer (c, r) = colours[index];
                }
            }
        }

        baseIsDraft = draft;
        baseDirty = false;
        builtFrom = settings;
        ++rebuildCount;
        lastRebuild = juce::Time::getMillisecondCounterHiRes();
        lastRebuildMs = lastRebuild - started;
    }

    void CombField::rebuildOverlay (float scale)
    {
        const auto plot = plotBounds();
        const int W = juce::jmax (4, juce::roundToInt ((float) getWidth() * scale));
        const int H = juce::jmax (4, juce::roundToInt ((float) getHeight() * scale));

        if (! overlay.isValid() || overlay.getWidth() != W || overlay.getHeight() != H)
            overlay = juce::Image (juce::Image::ARGB, W, H, true, juce::NativeImageType());
        else
            overlay.clear (overlay.getBounds());

        const auto& s = settings;
        const float lo = s.manual ? -kEarliestMs : juce::jmax (-kEarliestMs, s.delay - s.depth - s.drift);
        const float hi = s.manual ? rightMs : s.delay + s.depth + s.drift;

        juce::Graphics g (overlay);
        g.addTransform (juce::AffineTransform::scale ((float) W / (float) getWidth(), (float) H / (float) getHeight()));

        const auto width = (float) getWidth();
        const auto plotF = plot.toFloat();
        g.setColour (tone::base);
        g.fillRect (juce::Rectangle<float> (0.0f, plotF.getBottom(), width, (float) axisHeight));

        g.setFont (font (Weight::regular, 12.0f));

        for (float hz : { 100.0f, 1000.0f, 10000.0f })
        {
            const float y = yOfHz (hz);
            g.setColour (tone::text.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> (0.0f, y - 0.5f, width, 1.0f));
            g.setColour (tone::muted);
            g.drawText (hz >= 10000.0f ? "10k" : (hz >= 1000.0f ? "1k" : "100"), juce::Rectangle<float> (8.0f, y - 17.0f, 40.0f, 16.0f), juce::Justification::centredLeft, false);
        }

        const auto cx = width * 0.5f;
        g.setColour (tone::text.withAlpha (0.30f));
        g.fillRect (juce::Rectangle<float> (cx - 0.5f, 0.0f, 1.0f, plotF.getHeight()));

        // The rail: the part of the range the sweep uses, thick and bright in LFO.
        const float railY = plotF.getBottom();
        if (! s.manual)
        {
            const float x1 = xOfMs (lo), x2 = xOfMs (hi);
            g.setColour (tone::steel);
            g.fillRect (juce::Rectangle<float> (x1, railY, juce::jmax (2.0f, x2 - x1), 3.0f));
        }

        g.setFont (font (Weight::regular, 12.0f));
        const auto label = [&] (float ms, const juce::String& text)
        {
            const float x = xOfMs (ms);
            g.setColour (tone::faint);
            g.fillRect (juce::Rectangle<float> (x - 0.5f, railY + 3.0f, 1.0f, 4.0f));
            g.setColour (tone::muted);
            g.drawText (text, juce::Rectangle<float> (x - 22.0f, railY + 9.0f, 44.0f, 16.0f), juce::Justification::centred, false);
        };

        label (0.0f, "0 ms");
        for (float ms : { 1.0f, 2.0f, 3.0f, 5.0f })
            label (-ms, juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) + juce::String ((int) ms));
        const float lateTicks[] = { 1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f };
        float lastX = cx;
        for (float ms : lateTicks)
        {
            if (ms > rightMs)
                break;
            const float x = xOfMs (ms);
            if (x - lastX < 26.0f)
                continue;
            label (ms, "+" + juce::String ((int) ms));
            lastX = x;
        }

        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 12.0f));
        g.drawText ("Early", juce::Rectangle<float> (8.0f, 6.0f, 60.0f, 16.0f), juce::Justification::centredLeft, false);
        g.drawText ("Late", juce::Rectangle<float> (width - 68.0f, 6.0f, 60.0f, 16.0f), juce::Justification::centredRight, false);

        overlayScale = scale;
        overlayRight = rightMs;
        overlayLow = lo;
        overlayHigh = hi;
        overlayManual = s.manual;
        overlayWidth = getWidth();
    }

    // ---- what the audio is really doing --------------------------------------------------------------------------

    void CombField::rebuildTables()
    {
        const double rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        tableRate = rate;

        // About 43 ms of sound at any rate, which resolves the notches of the longest delay.
        const int order = rate > 120000.0 ? 13 : (rate > 60000.0 ? 12 : 11);
        fftSize = 1 << order;
        fft = std::make_unique<juce::dsp::FFT> (order);
        fftData.assign ((size_t) fftSize * 2, 0.0f);
        tapBuffer.assign ((size_t) fftSize, 0.0f);
        window.resize ((size_t) fftSize);

        double sum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
            sum += window[(size_t) i];
        }
        for (auto& w : window)
            w = (float) ((double) w * 2.0 / sum);   // a full scale sine reads 0 dB

        binFrom.resize ((size_t) wakeRows);
        binTo.resize ((size_t) wakeRows);
        binTilt.resize ((size_t) wakeRows);
        binExact.resize ((size_t) wakeRows);
        smoothing.assign ((size_t) wakeRows, 0.0f);
        for (int ch = 0; ch < 2; ++ch)
            previous[ch].assign ((size_t) wakeRows, 0.0f);
        const float span = octavesSpan();
        const float binHz = (float) rate / (float) fftSize;

        for (int r = 0; r < wakeRows; ++r)
        {
            const float top = kMaxHz * std::pow (2.0f, -span * (float) r / (float) wakeRows);
            const float bottom = kMaxHz * std::pow (2.0f, -span * (float) (r + 1) / (float) wakeRows);
            const int a = juce::jlimit (1, fftSize / 2 - 1, (int) std::floor (bottom / binHz + 0.5f));
            const int b = juce::jlimit (a, fftSize / 2 - 1, (int) std::floor (top / binHz + 0.5f));
            binExact[(size_t) r] = juce::jlimit (1.0f, (float) (fftSize / 2 - 2), std::sqrt (top * bottom) / binHz);
            binFrom[(size_t) r] = a;
            binTo[(size_t) r] = b;
            // Voices lose level going up. A gentle lift keeps the top of the field readable. For display only.
            const float centre = std::sqrt (top * bottom);
            binTilt[(size_t) r] = 2.6f * std::log2 (juce::jmax (centre, 60.0f) / 1000.0f);
        }
    }

    void CombField::paintSound (const PluginProcessor::Readings& r, int channels)
    {
        if (tableRate != (processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0) || fft == nullptr)
            rebuildTables();

        for (int ch = 0; ch < channels; ++ch)
        {
            if (r.outputPeak[ch] < 1.0e-4f)
                continue;

            processor.readOutputTap (ch, tapBuffer.data(), fftSize);
            juce::FloatVectorOperations::multiply (fftData.data(), tapBuffer.data(), window.data(), fftSize);
            std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
            fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

            auto& col = column[(size_t) ch];

            for (int row = 0; row < wakeRows; ++row)
            {
                const int a = binFrom[(size_t) row], b = binTo[(size_t) row];
                float power = 0.0f;

                if (b - a < 2)
                {
                    // Rows narrower than a bin read between bins, or the low end turns into blocks.
                    const float exact = binExact[(size_t) row];
                    const int k = (int) exact;
                    const float t = exact - (float) k;
                    const float m = fftData[(size_t) k] + t * (fftData[(size_t) k + 1] - fftData[(size_t) k]);
                    power = m * m;
                }
                else
                {
                    for (int k = a; k <= b; ++k)
                        power += fftData[(size_t) k] * fftData[(size_t) k];
                    power /= (float) (b - a + 1);
                }

                const float db = 10.0f * std::log10 (power + 1.0e-12f) + binTilt[(size_t) row];
                smoothing[(size_t) row] = juce::jlimit (0.0f, 1.0f, (db + 66.0f) / 50.0f);
            }

            for (int row = 0; row < wakeRows; ++row)
            {
                const float up = smoothing[(size_t) juce::jmax (0, row - 1)], down = smoothing[(size_t) juce::jmin (wakeRows - 1, row + 1)];
                col[(size_t) row] = 0.25f * up + 0.5f * smoothing[(size_t) row] + 0.25f * down;
            }

            // Fills the columns the deck crossed this frame, gliding from the last spectrum to this one.
            const auto toColumn = [this] (float ms) { return (uOfMs (ms) + 1.0f) * 0.5f * (float) wakeColumns - 0.5f; };
            const float now = toColumn (cursorMs[ch]), before = haveLast ? toColumn (lastMs[ch]) : now;
            const float c1 = juce::jmin (now, before), c2 = juce::jmax (now, before), width = juce::jmax (1.0f, std::abs (now - before));
            constexpr int brush = 3;
            auto& prev = previous[(size_t) ch];

            for (int c = juce::jmax (0, (int) c1 - brush); c <= juce::jmin (wakeColumns - 1, (int) c2 + brush + 1); ++c)
            {
                const float away = juce::jmax (0.0f, juce::jmax (c1 - (float) c, (float) c - c2));
                const float weight = juce::jlimit (0.0f, 1.0f, 1.0f - away / ((float) brush + 1.0f));
                const float t = juce::jlimit (0.0f, 1.0f, 1.0f - std::abs ((float) c - now) / width);
                auto* dest = wake.data() + c;

                for (int row = 0; row < wakeRows; ++row)
                {
                    auto& cell = dest[(size_t) (row * wakeColumns)];
                    const float level = prev[(size_t) row] + t * (col[(size_t) row] - prev[(size_t) row]);
                    cell = juce::jmax (cell, level * weight);
                }
            }

            prev = col;
        }
    }

    void CombField::renderWake()
    {
        juce::Image::BitmapData data (wakeImage, juce::Image::BitmapData::writeOnly);

        for (int row = 0; row < wakeRows; ++row)
        {
            auto* px = (juce::PixelARGB*) data.getLinePointer (row);
            const auto* v = wake.data() + row * wakeColumns;
            const auto* m = combMask.data() + row * wakeColumns;

            // Real level, seen through the comb: the sound lights up the steel, and the notches stay dark.
            for (int c = 0; c < wakeColumns; ++c)
                px[c] = wakeColours[(size_t) (int) (v[c] * m[c] * 255.0f)];
        }
    }

    void CombField::advance (double dtSeconds)
    {
        const auto r = processor.getReadings();
        const int channels = processor.getActiveOutputChannels();
        const auto now = readSettings();
        const double time = juce::Time::getMillisecondCounterHiRes();

        if (! (now == settings))
        {
            settings = now;
            updateAxis (settings);
            baseDirty = true;
            maskDirty = true;
            lastChange = time;
        }

        if (maskDirty && (time - lastMaskRebuild) * 0.001 >= 0.1)
        {
            rebuildMask();
            maskDirty = false;
            lastMaskRebuild = time;
        }

        // Once things have stopped moving, the half size picture gets replaced by the sharp one.
        if (baseIsDraft && ! baseDirty && (time - lastChange) * 0.001 >= kSettledSeconds)
            baseDirty = true;

        if (baseDirty && ! holdPicture && (time - lastRebuild) * 0.001 >= kRebuildSeconds)
            repaint();   // the picture is rebuilt at the top of the next paint

        for (int ch = 0; ch < channels; ++ch)
            cursorMs[ch] = r.deckOffsetMs[ch];

        gate = r.wetGate;

        // Fades everything left behind, then paints where the deck is now.
        const float fade = (float) std::exp (-dtSeconds / (double) kWakeFadeSeconds);
        bool wakeWasOn = wakePeak > 0.004f;

        if (wakeWasOn)
        {
            const int count = wakeColumns * wakeRows;
            juce::FloatVectorOperations::multiply (wake.data(), fade, count);
            wakePeak *= fade;
        }

        const bool sound = r.outputPeak[0] > 1.0e-4f || (channels > 1 && r.outputPeak[1] > 1.0e-4f);

        if (sound)
        {
            paintSound (r, channels);
            wakePeak = 1.0f;
        }

        // The crossing. Either side going through zero sets it off.
        bool crossed = false;
        if (haveLast)
            for (int ch = 0; ch < channels; ++ch)
                crossed = crossed || (cursorMs[ch] * lastMs[ch] < 0.0f && std::abs (cursorMs[ch] - lastMs[ch]) > 1.0e-4f);

        const bool leadCrossed = haveLast && cursorMs[0] * lastMs[0] < 0.0f && std::abs (cursorMs[0] - lastMs[0]) > 1.0e-4f;
        const float flashFade = (float) std::exp (-dtSeconds / 0.25);
        flash = crossed ? 1.0f : (flash < 0.01f ? 0.0f : flash * flashFade);
        leadFlash = leadCrossed ? 1.0f : (leadFlash < 0.01f ? 0.0f : leadFlash * flashFade);

        for (int ch = 0; ch < 2; ++ch)
            lastMs[ch] = cursorMs[ch];
        haveLast = true;

        const bool wakeOn = wakePeak > 0.004f;

        if (wakeOn || wakeWasOn)
        {
            if (! wakeOn)
                std::fill (wake.begin(), wake.end(), 0.0f);
            renderWake();
            wakeShown = wakeOn;
        }

        const float thumb = settings.manual ? xOfMs (settings.delay + flangeRaw->load (std::memory_order_relaxed)) : -1.0f;
        const auto plot = plotBounds();

        if (channels != channelsShown)
        {
            channelsShown = channels;
            repaint (plot);
        }

        if (wakeOn || wakeWasOn)
        {
            repaint (plot);
        }
        else
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float x = xOfMs (cursorMs[ch]);
                if (std::abs (x - drawnX[ch]) > 0.25f)
                {
                    const auto was = drawnX[ch];
                    repaint (plot.withLeft (juce::roundToInt (juce::jmin (x, was) - 16.0f)).withRight (juce::roundToInt (juce::jmax (x, was) + 16.0f)));
                }
            }

            if (flash > 0.0f || drawnFlash > 0.0f)
                repaint (plot.withSizeKeepingCentre (320, plot.getHeight()));
        }

        if (std::abs (thumb - drawnThumb) > 0.25f)
        {
            const auto rail = getLocalBounds().removeFromBottom (axisHeight + 8);
            if (drawnThumb >= 0.0f)
                repaint (rail.withLeft (juce::roundToInt (drawnThumb) - 14).withWidth (28));
            if (thumb >= 0.0f)
                repaint (rail.withLeft (juce::roundToInt (thumb) - 14).withWidth (28));
        }
    }

    void CombField::paint (juce::Graphics& g)
    {
        ++paintCount;
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const auto time = juce::Time::getMillisecondCounterHiRes();

        // A new size or density always rebuilds. A parameter that keeps moving rebuilds at most 15 times a second, at half size.
        if (! holdPicture)
        {
            const bool wrongPixels = std::abs (scale - overlayScale) > 0.01f || overlayWidth != getWidth() || ! base.isValid();
            const bool moving = (time - lastChange) * 0.001 < kSettledSeconds;

            if (wrongPixels)
                rebuildBase (scale, false);
            else if (baseDirty && (time - lastRebuild) * 0.001 >= kRebuildSeconds)
                rebuildBase (scale, moving);

            const float lo = settings.manual ? -kEarliestMs : juce::jmax (-kEarliestMs, settings.delay - settings.depth - settings.drift);
            const float hi = settings.manual ? rightMs : settings.delay + settings.depth + settings.drift;

            if (wrongPixels || overlayRight != rightMs || overlayLow != lo || overlayHigh != hi || overlayManual != settings.manual)
                rebuildOverlay (scale);
        }

        const auto plot = plotBounds();
        const auto plotF = plot.toFloat();

        g.setImageResamplingQuality (baseIsDraft ? juce::Graphics::mediumResamplingQuality : juce::Graphics::lowResamplingQuality);

        if (base.isValid())
            g.drawImage (base, plotF, juce::RectanglePlacement::stretchToFit);
        else
            g.fillAll (tone::well);

        if (wakeShown)
        {
            g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
            g.drawImage (wakeImage, plotF, juce::RectanglePlacement::stretchToFit);
        }

        if (overlay.isValid())
        {
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.drawImage (overlay, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
        }

        const float cx = (float) getWidth() * 0.5f;

        // Each time a deck goes through zero, the zero line lights and a pair of lines run out from it.
        if (flash > 0.0f)
        {
            const float travel = (1.0f - flash) * 150.0f;
            g.setColour (juce::Colours::white.withAlpha (0.35f + 0.65f * flash));
            g.fillRect (juce::Rectangle<float> (cx - 1.0f, 0.0f, 2.0f, plotF.getHeight()));

            if (travel > 1.0f)
            {
                g.setColour (tone::violetPale.withAlpha (flash * flash));
                g.fillRect (juce::Rectangle<float> (cx - travel - 0.75f, 0.0f, 1.5f, plotF.getHeight()));
                g.fillRect (juce::Rectangle<float> (cx + travel - 0.75f, 0.0f, 1.5f, plotF.getHeight()));
            }
        }

        drawnFlash = flash;

        // The wet deck, with a tick at each notch it is making right now. They run up and off the top as it nears zero.
        const float wetAmount = settings.bypass ? 0.0f : settings.mix * 0.01f * gate;
        const float notchStrength = juce::jlimit (0.0f, 1.0f, 1.0f - std::abs (2.0f * wetAmount - 1.0f) * 1.1f);
        const float polarity = (settings.feedback < 0.0f ? -1.0f : 1.0f) * (settings.invert ? -1.0f : 1.0f);
        const float plotHeight = plotF.getHeight();

        for (int ch = channelsShown - 1; ch >= 0; --ch)
        {
            const float ms = cursorMs[ch], x = xOfMs (ms);
            drawnX[ch] = x;
            const bool leading = ch == 0;
            const auto colour = leading ? juce::Colours::white : tone::violetPale;
            const float cursorAlpha = 0.45f + 0.55f * juce::jlimit (0.0f, 1.0f, gate);

            g.setColour (colour.withAlpha (cursorAlpha * (leading ? 1.0f : 0.8f)));
            g.fillRect (juce::Rectangle<float> (x - (leading ? 1.0f : 0.75f), 0.0f, leading ? 2.0f : 1.5f, plotHeight));

            const float absMs = std::abs (ms);

            if (notchStrength > 0.02f && absMs > 0.0005f)
            {
                g.setColour (colour.withAlpha (notchStrength));
                const float seconds = absMs * 0.001f;

                for (int k = polarity > 0.0f ? 0 : 1; k < 64; ++k)
                {
                    const float hz = polarity > 0.0f ? ((float) k + 0.5f) / seconds : (float) k / seconds;
                    if (hz < kMinHz)
                        continue;
                    if (hz > kMaxHz)
                        break;
                    const float y = yOfHz (hz);
                    const float next = yOfHz (hz + 1.0f / seconds);
                    if (y - next < 5.0f)
                        break;
                    const float half = leading ? 8.0f : 5.0f;
                    g.fillRect (juce::Rectangle<float> (x - half, y - 1.0f, half * 2.0f, 2.0f));
                }
            }
        }

        // In Manual the thumb on the rail is where the hand has put Flange. The line above catches up with it.
        if (settings.manual)
        {
            const float x = xOfMs (settings.delay + flangeRaw->load (std::memory_order_relaxed));
            drawnThumb = x;
            const float r = dragging ? 8.0f : 6.5f;
            g.setColour (dragging ? tone::violet : tone::text);
            g.fillRect (juce::Rectangle<float> (x - r, plotHeight - r, r * 2.0f, r * 2.0f));
        }
        else
        {
            drawnThumb = -1.0f;
        }
    }

    // ---- the hand --------------------------------------------------------------------------------------------------

    void CombField::finishGesture()
    {
        if (gestureOpen && gestureParam != nullptr)
            gestureParam->endChangeGesture();
        gestureOpen = false;
        gestureParam = nullptr;
    }

    void CombField::applyDrag (const juce::MouseEvent& e)
    {
        const bool manual = settings.manual;
        auto& param = manual ? flangeParam : depthParam;

        if (! gestureOpen)
        {
            param.beginChangeGesture();
            gestureOpen = true;
            gestureParam = &param;
        }

        // Shift slows the hand to a fifth, measured from where shift first went down.
        float x = e.position.x;

        if (e.mods.isShiftDown())
        {
            if (! fineDrag)
            {
                fineDrag = true;
                fineAnchorX = x;
                fineAnchorValue = manual ? flangeRaw->load() : depthRaw->load();
            }

            const float at = xOfMs (settings.delay + (manual ? fineAnchorValue : fineAnchorValue));
            x = at + (x - fineAnchorX) * 0.2f;
        }
        else
        {
            fineDrag = false;
        }

        const float ms = msOfX (x);
        float value = manual ? ms - settings.delay : std::abs (ms - settings.delay);

        // Close to the middle is exactly 0, the only place Flange takes the wet out.
        if (std::abs (x - xOfMs (settings.delay)) < kSnapPoints)
            value = 0.0f;

        value = juce::jlimit (manual ? -kMaxFlangeMs : 0.0f, kMaxFlangeMs, value);
        param.setValueNotifyingHost (param.convertTo0to1 (value));
    }

    void CombField::mouseDown (const juce::MouseEvent& e)
    {
        dragging = true;
        fineDrag = false;
        // In Manual the deck goes straight to the hand. In LFO a stray click should not change the sweep.
        if (settings.manual)
            applyDrag (e);
    }

    void CombField::mouseDrag (const juce::MouseEvent& e)
    {
        if (settings.manual || e.getDistanceFromDragStart() > 2)
            applyDrag (e);
    }

    void CombField::mouseUp (const juce::MouseEvent&)
    {
        dragging = false;
        fineDrag = false;
        finishGesture();
        repaint (getLocalBounds().removeFromBottom (axisHeight + 12));
    }

    void CombField::mouseDoubleClick (const juce::MouseEvent&)
    {
        finishGesture();
        auto& param = settings.manual ? flangeParam : depthParam;
        param.beginChangeGesture();
        param.setValueNotifyingHost (param.getDefaultValue());
        param.endChangeGesture();
    }
}
