#include "ResonanceGraph.h"

#include "../PluginProcessor.h"

namespace ui
{
    namespace
    {
        constexpr float loHz = 20.0f, hiHz = 20000.0f;
        constexpr float sensReach = 0.30f;        // +12 dB of sensitivity sits this fraction of the height above the middle
        constexpr float cutFullScale = 0.64f;     // 24 dB of reduction reaches this fraction of the height down from the ceiling
        constexpr float ceilingOffset = 5.0f;     // at rest the gold line hides just above the ceiling
        constexpr float levelFloor = -96.0f, levelTop = 10.0f;
        constexpr float tiltDbPerOctave = 4.5f;   // display only

        const juce::String bandNames[] = { "1", "2", "3" };

        constexpr float gridHz[] = { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
        const char* const gridText[] = { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };

        float glide (float current, float target, float riseTau, float fallTau, float dt) noexcept
        {
            const float tau = target > current ? riseTau : fallTau;
            return current + (target - current) * (1.0f - std::exp (-dt / tau));
        }

        // Two passes of a moving average make a triangle. The radius follows how many graph points one FFT bin covers,
        // so the steps of a linear FFT on a log axis are gone at the low end, and the highs keep their detail.
        template <size_t N>
        void smoothBy (std::array<float, N>& data, std::array<float, N>& work, const std::array<float, N>& hz, float binsPerRadius, int minRadius, int passes)
        {
            constexpr float pointsPerHzAtOneHz = 854.0f;   // points per FFT bin at 1 Hz: about 23.4 Hz bins, 25.6 points an octave

            for (int pass = 0; pass < passes; ++pass)
            {
                float sum = 0.0f;
                std::array<float, N + 1> prefix;
                prefix[0] = 0.0f;

                for (size_t i = 0; i < N; ++i)
                {
                    sum += data[i];
                    prefix[i + 1] = sum;
                }

                for (size_t i = 0; i < N; ++i)
                {
                    const int radius = juce::jlimit (minRadius, 18, (int) (pointsPerHzAtOneHz / juce::jmax (hz[i], 1.0f) * binsPerRadius));
                    const size_t a = (size_t) juce::jmax (0, (int) i - radius);
                    const size_t b = juce::jmin (N, i + (size_t) radius + 1);
                    work[i] = (prefix[b] - prefix[a]) / (float) (b - a);
                }

                data = work;
            }
        }
    }

    ResonanceGraph::ResonanceGraph (SmoothResonanceProcessor& p)
        : processor (p)
    {
        setOpaque (true);
        setComponentID ("graph");

        auto& s = processor.parameters;

        for (int b = 0; b < numBands; ++b)
        {
            const auto number = juce::String (b + 1);
            bands[b] = { s.getParameter ("band" + number + "Freq"), s.getParameter ("band" + number + "Sens"), s.getParameter ("band" + number + "Width") };
        }

        lowParam = s.getParameter ("lowFreq");
        highParam = s.getParameter ("highFreq");
        depthParam = s.getParameter ("depth");
        mixParam = s.getParameter ("mix");
        deltaParam = s.getParameter ("delta");
        bypassParam = s.getParameter ("bypass");
        keyParam = s.getParameter ("sideChain");

        const float octaves = std::log2 (hiHz / loHz);

        for (size_t i = 0; i < (size_t) numPoints; ++i)
        {
            const float position = ((float) i + 0.5f) / (float) numPoints;
            pointX[i] = position;
            pointHz[i] = loHz * std::pow (2.0f, octaves * position);
            tilt[i] = tiltDbPerOctave * std::log2 (pointHz[i] / 1000.0f);
            shownLevel[i] = targetLevel[i] = levelFloor - 8.0f;
        }
    }

    ResonanceGraph::~ResonanceGraph()
    {
        endGesture();
    }

    void ResonanceGraph::resized()
    {
    }

    float ResonanceGraph::xForHz (float hz) const noexcept
    {
        return (float) getWidth() * std::log (juce::jlimit (loHz, hiHz, hz) / loHz) / std::log (hiHz / loHz);
    }

    float ResonanceGraph::hzForX (float x) const noexcept
    {
        return loHz * std::pow (hiHz / loHz, juce::jlimit (0.0f, 1.0f, x / (float) juce::jmax (1, getWidth())));
    }

    float ResonanceGraph::yForSensitivity (float db) const noexcept
    {
        return (float) getHeight() * (0.5f - db / 12.0f * sensReach);
    }

    float ResonanceGraph::sensitivityForY (float y) const noexcept
    {
        return (0.5f - y / (float) juce::jmax (1, getHeight())) / sensReach * 12.0f;
    }

    float ResonanceGraph::yForLevel (float db) const noexcept
    {
        return (float) getHeight() * (1.0f - (db - levelFloor) / (levelTop - levelFloor));
    }

    float ResonanceGraph::yForReduction (float db) const noexcept
    {
        return (float) getHeight() * cutFullScale * db / 24.0f - ceilingOffset;
    }

    void ResonanceGraph::buildTargets()
    {
        const float mix = juce::jlimit (0.0f, 1.0f, getReal (*mixParam) * 0.01f);

        slotsApart = false;

        for (size_t i = 0; i < (size_t) numPoints; ++i)
        {
            targetLevel[i] = juce::jmax (levelFloor - 8.0f, frame.inputDb[i] + tilt[i]);

            for (int slot = 0; slot < 2; ++slot)
            {
                // What you hear after Mix: dry and treated added back together, so a lower Mix shows a shallower cut.
                const float r = juce::jmax (0.0f, frame.reductionDb[slot][i]);
                targetCut[slot][i] = mix >= 0.999f ? r : -20.0f * std::log10 (juce::jmax (1.0e-4f, 1.0f - mix + mix * std::pow (10.0f, -r * 0.05f)));
            }

            slotsApart = slotsApart || std::abs (targetCut[0][i] - targetCut[1][i]) > 0.25f;
        }

        smoothBy (targetLevel, scratch, pointHz, 0.7f, 1, 2);
        smoothBy (targetCut[0], scratch, pointHz, 0.45f, 0, 1);
        smoothBy (targetCut[1], scratch, pointHz, 0.45f, 0, 1);
    }

    bool ResonanceGraph::advance (double dtSeconds)
    {
        const float dt = (float) juce::jlimit (0.001, 0.1, dtSeconds);
        bool changed = false;

        if (processor.readGraph (frame))
        {
            staleSeconds = 0.0;
            buildTargets();
            changed = true;
        }
        else
        {
            staleSeconds += (double) dt;

            if (staleSeconds > 0.25)    // transport stopped: let everything settle out instead of freezing
            {
                targetLevel.fill (levelFloor - 8.0f);
                targetCut[0].fill (0.0f);
                targetCut[1].fill (0.0f);
                slotsApart = false;
            }
        }

        float loudest = -200.0f, deepest = 0.0f, moved = 0.0f;

        for (size_t i = 0; i < (size_t) numPoints; ++i)
        {
            const float nextLevel = glide (shownLevel[i], targetLevel[i], 0.016f, 0.095f, dt);
            moved = juce::jmax (moved, std::abs (nextLevel - shownLevel[i]));
            shownLevel[i] = nextLevel;
            loudest = juce::jmax (loudest, nextLevel);

            for (int slot = 0; slot < 2; ++slot)
            {
                const float nextCut = glide (shownCut[slot][i], targetCut[slot][i], 0.018f, 0.070f, dt);
                moved = juce::jmax (moved, std::abs (nextCut - shownCut[slot][i]));
                shownCut[slot][i] = nextCut;
                deepest = juce::jmax (deepest, nextCut);
            }
        }

        peakDb = deepest;

        const auto settle = [&] (float& v, float target, float tau)
        {
            const float next = v + (target - v) * (1.0f - std::exp (-dt / tau));
            moved = juce::jmax (moved, std::abs (next - v) * 40.0f);
            v = std::abs (next - target) < 0.002f ? target : next;
        };

        settle (bypassShown, bypassParam->getValue() > 0.5f ? 1.0f : 0.0f, 0.06f);
        settle (deltaShown, deltaParam->getValue() > 0.5f ? 1.0f : 0.0f, 0.06f);
        settle (hintShown, staleSeconds > 0.8 || loudest < -88.0f ? 1.0f : 0.0f, 0.12f);

        // Automation moving a dot has to show even with no audio running.
        juce::uint32 stamp = 17;
        const auto mixIn = [&stamp] (const juce::RangedAudioParameter* prm) { stamp = stamp * 31u + (juce::uint32) juce::roundToInt (prm->getValue() * 100000.0f); };

        for (const auto& b : bands)
        {
            mixIn (b.freq);
            mixIn (b.sens);
            mixIn (b.width);
        }

        mixIn (lowParam);
        mixIn (highParam);
        mixIn (depthParam);

        if (stamp != paramStamp)
        {
            paramStamp = stamp;
            changed = true;
        }

        return changed || moved > 0.01f;
    }

    void ResonanceGraph::upsample (const std::array<float, numPoints>& from, std::array<float, numDense>& to) const
    {
        // Catmull-Rom halfway points, so the lines stay curved at twice the graph's resolution.
        for (int i = 0; i < numPoints; ++i)
        {
            to[(size_t) (i * 2)] = from[(size_t) i];

            if (i + 1 < numPoints)
            {
                const float a = from[(size_t) juce::jmax (0, i - 1)], b = from[(size_t) i], c = from[(size_t) (i + 1)], d = from[(size_t) juce::jmin (numPoints - 1, i + 2)];
                to[(size_t) (i * 2 + 1)] = (9.0f * (b + c) - a - d) / 16.0f;
            }
        }
    }

    void ResonanceGraph::buildRibbon (const float* ys, const float* xs, int count, float halfWidth)
    {
        ribbon.clear();

        for (int i = 0; i < count; ++i)
        {
            const int before = juce::jmax (0, i - 1), after = juce::jmin (count - 1, i + 1);
            const float dx = xs[after] - xs[before], dy = ys[after] - ys[before];
            const float scale = halfWidth / std::sqrt (dx * dx + dy * dy + 1.0e-6f);
            normalX[(size_t) i] = -dy * scale;
            normalY[(size_t) i] = dx * scale;

            if (i == 0)
                ribbon.startNewSubPath (xs[i] + normalX[(size_t) i], ys[i] + normalY[(size_t) i]);
            else
                ribbon.lineTo (xs[i] + normalX[(size_t) i], ys[i] + normalY[(size_t) i]);
        }

        for (int i = count; --i >= 0;)
            ribbon.lineTo (xs[i] - normalX[(size_t) i], ys[i] - normalY[(size_t) i]);

        ribbon.closeSubPath();
    }

    // ---- drawing ---------------------------------------------------------------------------------------------

    void ResonanceGraph::paint (juce::Graphics& g)
    {
        const auto w = (float) getWidth();
        ++paintCount;

        for (size_t i = 0; i < (size_t) numPoints; ++i)
        {
            yLevel[i] = yForLevel (shownLevel[i]);
            yAfter[i] = yForLevel (shownLevel[i] - juce::jmax (shownCut[0][i], shownCut[1][i]) * (1.0f - bypassShown));
            yCut[0][i] = yForReduction (shownCut[0][i]);
            yCut[1][i] = yForReduction (shownCut[1][i]);
        }

        upsample (yLevel, yVoice);
        upsample (yAfter, yShaved);
        upsample (yCut[0], yGold[0]);
        upsample (yCut[1], yGold[1]);

        for (int i = 0; i < numDense; ++i)
            denseX[(size_t) i] = w * (float) (i + 1) / (float) (numDense + 1);

        drawVoice (g);
        drawGrid (g);

        drawSensitivity (g);
        drawGold (g);
        drawHandles (g);
        drawOverlays (g);
    }

    void ResonanceGraph::drawVoice (juce::Graphics& g)
    {
        const auto w = (float) getWidth(), h = (float) getHeight();
        const float lowX = juce::jlimit (0.0f, w, xForHz (getReal (*lowParam))), highX = juce::jlimit (lowX, w, xForHz (getReal (*highParam)));

        // The voice is a mountain of light standing on the floor: deep at the base, pale where it is loud. It is dimmer
        // outside Low and High, where the sound is left alone.
        // Painted inside out to stay fast: the finished colours go down as one opaque picture, then everything above the
        // voice is covered with flat well colour. CoreGraphics shades gradients pixel by pixel and blends every alpha fill,
        // and on a Retina canvas those two were most of the frame. The picture is only redrawn when Low, High, Delta or the
        // size changes.
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (voiceImage.isNull() || scale != voiceScale || lowX != voiceLowX || highX != voiceHighX || deltaShown != voiceDelta
            || voiceImage.getWidth() != juce::roundToInt (w * scale) || voiceImage.getHeight() != juce::roundToInt (h * scale))
        {
            voiceScale = scale;
            voiceLowX = lowX;
            voiceHighX = highX;
            voiceDelta = deltaShown;

            if (voiceImage.getWidth() != juce::roundToInt (w * scale) || voiceImage.getHeight() != juce::roundToInt (h * scale))
                voiceImage = juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt (w * scale)), juce::jmax (1, juce::roundToInt (h * scale)), false);

            juce::Graphics picture (voiceImage);
            picture.addTransform (juce::AffineTransform::scale (scale));

            constexpr int bandsOfVoice = 240;
            constexpr float dimOutside = 0.56f;

            for (int band = 0; band < bandsOfVoice; ++band)
            {
                const float t = ((float) band + 0.5f) / (float) bandsOfVoice;
                const float top = h * (float) band / (float) bandsOfVoice;
                const float tall = h / (float) bandsOfVoice + 1.0f;   // each covers the last one's lower edge, so no seams at any size
                auto colour = t < 0.48f ? tone::voiceTop.interpolatedWith (tone::cobalt, juce::jlimit (0.0f, 1.0f, (t - 0.18f) / 0.30f))
                                        : tone::cobalt.interpolatedWith (tone::voiceLow, (t - 0.48f) / 0.52f);
                colour = colour.interpolatedWith (tone::well, 0.74f * deltaShown);
                picture.setColour (colour);
                picture.fillRect (juce::Rectangle<float> (lowX, top, highX - lowX, tall));
                picture.setColour (colour.interpolatedWith (tone::well, dimOutside));
                picture.fillRect (juce::Rectangle<float> (0.0f, top, lowX, tall));
                picture.fillRect (juce::Rectangle<float> (highX, top, w - highX, tall));
            }
        }

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (voiceImage, juce::AffineTransform::scale (1.0f / scale));

        // Everything above what you hear after treatment.
        shape.clear();
        shape.startNewSubPath (-2.0f, -2.0f);
        shape.lineTo (w + 2.0f, -2.0f);
        shape.lineTo (w + 2.0f, yShaved[numDense - 1]);

        for (int i = numDense; --i >= 0;)
            shape.lineTo (denseX[(size_t) i], yShaved[(size_t) i]);

        shape.lineTo (-2.0f, yShaved[0]);
        shape.closeSubPath();
        g.setColour (tone::well);
        g.fillPath (shape);

        // What was shaved off, in gold, right where it came off the voice.
        shape.clear();

        for (int i = 0; i < numDense; ++i)
        {
            if (i == 0)
                shape.startNewSubPath (denseX[0], yVoice[0]);
            else
                shape.lineTo (denseX[(size_t) i], yVoice[(size_t) i]);
        }

        for (int i = numDense; --i >= 0;)
            shape.lineTo (denseX[(size_t) i], yShaved[(size_t) i]);

        shape.closeSubPath();
        const auto gold = tone::gold.interpolatedWith (tone::boneFaint, bypassShown);
        g.setColour (gold.withAlpha (0.72f - 0.45f * bypassShown));
        g.fillPath (shape);

        // The bright edge is the sound after treatment.
        const float presence = 1.0f - 0.72f * deltaShown;
        buildRibbon (yShaved.data(), denseX.data(), numDense, 0.85f);
        const juce::Graphics::ScopedSaveState outer (g);
        g.setColour (tone::bone.withAlpha (0.42f * presence));
        g.fillPath (ribbon);
        g.reduceClipRegion (juce::Rectangle<float> (lowX, 0.0f, highX - lowX, h).getSmallestIntegerContainer());
        g.setColour (tone::bone.withAlpha (presence));
        g.fillPath (ribbon);
    }

    void ResonanceGraph::drawGrid (juce::Graphics& g)
    {
        const auto w = (float) getWidth(), h = (float) getHeight();

        for (size_t i = 0; i < std::size (gridHz); ++i)
        {
            const float x = std::round (xForHz (gridHz[i]));
            const bool major = gridHz[i] == 100.0f || gridHz[i] == 1000.0f || gridHz[i] == 10000.0f;
            g.setColour (tone::bone.withAlpha (major ? 0.11f : 0.055f));
            g.fillRect (juce::Rectangle<float> (x, 0.0f, 1.0f, h));
        }

        for (int db = 6; db <= 24; db += 6)
        {
            const float y = std::round (yForReduction ((float) db) + ceilingOffset);
            g.setColour (tone::gold.withAlpha (0.09f));
            g.fillRect (juce::Rectangle<float> (0.0f, y, w, 1.0f));
        }
    }

    void ResonanceGraph::drawGold (juce::Graphics& g)
    {
        const auto w = (float) getWidth(), h = (float) getHeight();
        const float dim = 1.0f - 0.72f * bypassShown;
        const auto gold = tone::gold.interpolatedWith (tone::boneFaint, bypassShown);
        const auto hot = tone::goldHot.interpolatedWith (tone::boneSoft, bypassShown);

        if (slotsApart)
        {
            buildRibbon (yGold[1].data(), denseX.data(), numDense, 3.0f);
            g.setColour (tone::copper.withAlpha (0.25f * dim));
            g.fillPath (ribbon);
            buildRibbon (yGold[1].data(), denseX.data(), numDense, 1.1f);
            g.setColour (tone::copper.withAlpha (dim));
            g.fillPath (ribbon);
        }

        // Gold poured from the ceiling: dark where it starts, bright where it pools.
        shape.clear();
        shape.startNewSubPath (-2.0f, -8.0f);
        shape.lineTo (w + 2.0f, -8.0f);
        shape.lineTo (w + 2.0f, yGold[0][numDense - 1]);

        for (int i = numDense; --i >= 0;)
            shape.lineTo (denseX[(size_t) i], yGold[0][(size_t) i]);

        shape.lineTo (-2.0f, yGold[0][0]);
        shape.closeSubPath();

        g.setColour (gold.withAlpha (0.94f * dim));
        g.fillPath (shape);

        // Darker where it leaves the ceiling, so the gold looks poured instead of flat. Only where there can be gold: Low
        // to High, plus the taper just outside them. A real gradient here: alpha bands would show seams, and the area is small.
        const float shadeDepth = h * 0.20f;
        const float shadeFrom = juce::jmax (0.0f, xForHz (getReal (*lowParam) * 0.5f)), shadeTo = juce::jmin (w, xForHz (getReal (*highParam) * 2.0f));
        juce::ColourGradient shade (tone::well.withAlpha (0.56f), 0.0f, 0.0f, tone::well.withAlpha (0.0f), 0.0f, shadeDepth, false);
        g.setGradientFill (shade);
        g.fillRect (juce::Rectangle<float> (shadeFrom, 0.0f, juce::jmax (0.0f, shadeTo - shadeFrom), shadeDepth));

        buildRibbon (yGold[0].data(), denseX.data(), numDense, 4.0f);
        g.setColour (gold.withAlpha (0.20f * dim));
        g.fillPath (ribbon);
        buildRibbon (yGold[0].data(), denseX.data(), numDense, 1.0f);
        g.setColour (hot.withAlpha (dim));
        g.fillPath (ribbon);
    }

    void ResonanceGraph::drawSensitivity (juce::Graphics& g)
    {
        const auto w = (float) getWidth(), h = (float) getHeight();
        const float zeroY = yForSensitivity (0.0f);

        // The same bell the engine uses, drawn from the parameters so it moves with no audio running.
        float amounts[numBands], centres[numBands], sigmas[numBands];

        for (int b = 0; b < numBands; ++b)
        {
            amounts[b] = getReal (*bands[b].sens);
            centres[b] = std::log2 (juce::jmax (getReal (*bands[b].freq), 1.0f));
            sigmas[b] = juce::jmax (getReal (*bands[b].width), 0.05f) / 2.3548f;
        }

        shape.clear();

        for (int i = 0; i < numSensSteps; ++i)
        {
            const float x = w * (float) i / (float) (numSensSteps - 1);
            const float logHz = std::log2 (hzForX (x));
            float s = 0.0f;

            for (int b = 0; b < numBands; ++b)
            {
                const float d = (logHz - centres[b]) / sigmas[b];

                if (std::abs (d) < 4.0f)
                    s += amounts[b] * std::exp (-0.5f * d * d);
            }

            sensX[(size_t) i] = x;
            sensY[(size_t) i] = yForSensitivity (juce::jlimit (-15.0f, 15.0f, s));

            if (i == 0)
                shape.startNewSubPath (x, sensY[0]);
            else
                shape.lineTo (x, sensY[(size_t) i]);
        }

        shape.lineTo (w, zeroY);
        shape.lineTo (0.0f, zeroY);
        shape.closeSubPath();

        const float lowX = xForHz (getReal (*lowParam)), highX = xForHz (getReal (*highParam));

        g.setColour (tone::bone.withAlpha (0.10f));
        g.fillPath (shape);

        // Bright where it has an effect, faint where Low and High leave the sound alone.
        buildRibbon (sensY.data(), sensX.data(), numSensSteps, 0.8f);
        g.setColour (tone::bone.withAlpha (0.34f));
        g.fillPath (ribbon);

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (juce::Rectangle<float> (lowX, 0.0f, juce::jmax (0.0f, highX - lowX), h).getSmallestIntegerContainer());
            g.setColour (tone::bone);
            buildRibbon (sensY.data(), sensX.data(), numSensSteps, 1.1f);
            g.fillPath (ribbon);
        }

        // "More" and "Less" say which way a dot goes without any jargon.
        g.setFont (font (Weight::regular, 16.5f));
        g.setColour (tone::boneSoft);
        g.drawText ("Treat more", juce::Rectangle<float> (14.0f, yForSensitivity (12.0f) - 10.0f, 100.0f, 20.0f), juce::Justification::centredLeft, false);
        g.drawText ("Treat less", juce::Rectangle<float> (14.0f, yForSensitivity (-12.0f) - 10.0f, 100.0f, 20.0f), juce::Justification::centredLeft, false);
        g.setColour (tone::bone.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> (0.0f, std::round (zeroY), w, 1.0f));

        // The dots.
        for (int b = 0; b < numBands; ++b)
        {
            const auto grab = (Grab) ((int) Grab::band1 + b);
            const auto c = juce::Point<float> (xForHz (getReal (*bands[b].freq)), yForSensitivity (getReal (*bands[b].sens)));
            const bool isSelected = b == selectedBand;
            const bool lit = hover == grab || drag == grab;
            const float r = isSelected ? 12.5f : 11.0f;

            if (isSelected)
            {
                // The selected dot shows how wide it reaches.
                const float halfWidth = getReal (*bands[b].width) * 0.5f;
                const float hz = getReal (*bands[b].freq);
                const float x1 = xForHz (hz * std::pow (2.0f, -halfWidth)), x2 = xForHz (hz * std::pow (2.0f, halfWidth));
                g.setColour (tone::bone.withAlpha (0.65f));
                g.fillRect (juce::Rectangle<float> (x1, c.y - 0.5f, x2 - x1, 1.0f));
                g.fillRect (juce::Rectangle<float> (x1 - 0.5f, c.y - 6.0f, 1.0f, 12.0f));
                g.fillRect (juce::Rectangle<float> (x2 - 0.5f, c.y - 6.0f, 1.0f, 12.0f));
            }

            if (lit)
            {
                g.setColour (tone::bone.withAlpha (0.22f));
                g.fillEllipse (c.x - r - 7.0f, c.y - r - 7.0f, (r + 7.0f) * 2.0f, (r + 7.0f) * 2.0f);
            }

            g.setColour (tone::well.withAlpha (0.55f));
            g.fillEllipse (c.x - r - 2.0f, c.y - r - 1.0f, (r + 2.0f) * 2.0f, (r + 2.0f) * 2.0f);

            g.setColour (isSelected ? tone::bone : tone::well);
            g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);
            g.setColour (tone::bone);
            g.drawEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.6f);

            g.setColour (isSelected ? tone::well : tone::bone);
            g.setFont (font (Weight::semibold, 18.0f));
            g.drawText (bandNames[b], juce::Rectangle<float> (c.x - r, c.y - r - 0.5f, r * 2.0f, r * 2.0f), juce::Justification::centred, false);
        }
    }

    void ResonanceGraph::drawHandles (juce::Graphics& g)
    {
        const auto h = (float) getHeight();

        const auto drawHandle = [&] (Grab grab, float x, const juce::String& text, bool textOnRight)
        {
            const bool lit = hover == grab || drag == grab;
            g.setColour (tone::bone.withAlpha (lit ? 0.9f : 0.42f));
            g.fillRect (juce::Rectangle<float> (x - 0.75f, 0.0f, 1.5f, h));

            const float tabW = 46.0f, tabH = 24.0f;
            const auto tab = juce::Rectangle<float> (textOnRight ? x : x - tabW, h - 66.0f, tabW, tabH);
            juce::Path shape;
            shape.addRoundedRectangle (tab.getX(), tab.getY(), tab.getWidth(), tab.getHeight(), 6.0f, 6.0f, ! textOnRight, textOnRight, ! textOnRight, textOnRight);
            g.setColour (lit ? tone::bone : tone::bone.withAlpha (0.86f));
            g.fillPath (shape);
            g.setColour (tone::well);
            g.setFont (font (Weight::semibold, 16.0f));
            g.drawText (text, tab, juce::Justification::centred, false);
        };

        drawHandle (Grab::low, xForHz (getReal (*lowParam)), "Low", true);
        drawHandle (Grab::high, xForHz (getReal (*highParam)), "High", false);
    }

    void ResonanceGraph::drawTag (juce::Graphics& g, juce::Point<float> anchor, const juce::String& text, juce::Colour textColour)
    {
        const auto f = font (Weight::semibold, 17.0f);
        const float width = juce::GlyphArrangement::getStringWidth (f, text) + 18.0f;
        auto box = juce::Rectangle<float> (anchor.x - width * 0.5f, anchor.y, width, 26.0f);
        box = box.withX (juce::jlimit (6.0f, juce::jmax (6.0f, (float) getWidth() - width - 6.0f), box.getX()));
        g.setColour (tone::well.withAlpha (0.92f));
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (tone::bone.withAlpha (0.25f));
        g.drawRoundedRectangle (box, 6.0f, 1.0f);
        g.setColour (textColour);
        g.setFont (f);
        g.drawText (text, box, juce::Justification::centred, false);
    }

    juce::String ResonanceGraph::tagFor (Grab grab) const
    {
        if (grab == Grab::low)  return "Low " + hertzText (getReal (*lowParam));
        if (grab == Grab::high) return "High " + hertzText (getReal (*highParam));

        const int b = (int) grab - (int) Grab::band1;
        const float s = getReal (*bands[b].sens);
        return hertzText (getReal (*bands[b].freq)) + ", "
             + (std::abs (s) < 0.05f ? juce::String ("0.0") : (s > 0.0f ? "+" : "") + juce::String (s, 1)) + " dB";
    }

    void ResonanceGraph::drawOverlays (juce::Graphics& g)
    {
        const auto w = (float) getWidth(), h = (float) getHeight();

        // The dB scale goes over the gold, so that it can be read against a cut.
        g.setFont (font (Weight::regular, 16.0f));

        for (int db = 6; db <= 24; db += 6)
        {
            const float y = std::round (yForReduction ((float) db) + ceilingOffset);
            g.setColour (tone::gold.withAlpha (0.78f));
            g.drawText (db == 24 ? "24 dB" : juce::String (db), juce::Rectangle<float> (w - 64.0f, y - 21.0f, 54.0f, 20.0f), juce::Justification::centredRight, false);
        }

        // The floor of the graph has the frequencies, on a soft fade so that they read over the voice.
        juce::ColourGradient floor (tone::well.withAlpha (0.0f), 0.0f, h - 46.0f, tone::well.withAlpha (0.88f), 0.0f, h - 14.0f, false);
        g.setGradientFill (floor);
        g.fillRect (juce::Rectangle<float> (0.0f, h - 46.0f, w, 46.0f));

        g.setFont (font (Weight::regular, 16.0f));
        g.setColour (tone::boneSoft);

        for (size_t i = 0; i < std::size (gridHz); ++i)
            g.drawText (gridText[i], juce::Rectangle<float> (xForHz (gridHz[i]) - 24.0f, h - 26.0f, 48.0f, 20.0f), juce::Justification::centred, false);

        // Hovering Depth draws the limit it sets.
        if (highlight == "depth")
        {
            const float y = yForReduction (getReal (*depthParam));
            juce::Path line;
            line.startNewSubPath (0.0f, y);
            line.lineTo (w, y);
            const float dashes[] = { 7.0f, 6.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, line, dashes, 2);
            g.setColour (tone::goldHot);
            g.fillPath (dashed);
            g.setColour (tone::goldHot);
            g.setFont (font (Weight::semibold, 17.0f));
            g.drawText ("Deepest cut", juce::Rectangle<float> (12.0f, y - 24.0f, 200.0f, 22.0f), juce::Justification::centredLeft, false);
        }
        else if (highlight == "lowFreq" || highlight == "highFreq")
        {
            const float x = xForHz (getReal (highlight == "lowFreq" ? *lowParam : *highParam));
            g.setColour (tone::bone.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (x - 4.0f, 0.0f, 8.0f, h));
        }

        // What the sound is doing now, when it is not the normal treated sound.
        const auto banner = [&] (const juce::String& text, juce::Colour fill, float alpha)
        {
            if (alpha < 0.01f)
                return;

            const auto f = font (Weight::semibold, 18.0f);
            const float width = juce::GlyphArrangement::getStringWidth (f, text) + 26.0f;
            const auto box = juce::Rectangle<float> (14.0f, 14.0f, width, 32.0f);
            g.setColour (fill.withAlpha (alpha));
            g.fillRoundedRectangle (box, 6.0f);
            g.setColour (tone::well.withAlpha (alpha));
            g.setFont (f);
            g.drawText (text, box, juce::Justification::centred, false);
        };

        if (bypassShown > 0.01f)
            banner ("Bypassed: you hear the untouched sound", tone::bone, bypassShown);
        else
            banner ("Delta on: you hear only what is being taken out", tone::gold, deltaShown);

        // An empty graph says what to do.
        if (hintShown > 0.01f && drag == Grab::none)
        {
            // In the widest clear space, so that it does not cross a Low or High line.
            const float lowX = xForHz (getReal (*lowParam)), highX = xForHz (getReal (*highParam));
            const float gaps[][2] = { { 0.0f, lowX }, { lowX, highX }, { highX, w } };
            int widest = 0;

            for (int i = 1; i < 3; ++i)
                if (gaps[i][1] - gaps[i][0] > gaps[widest][1] - gaps[widest][0])
                    widest = i;

            g.setColour (tone::boneSoft.withAlpha (hintShown));
            g.setFont (font (Weight::regular, 22.0f));
            g.drawText ("Play something to see it here", juce::Rectangle<float> (gaps[widest][0], h * 0.72f, gaps[widest][1] - gaps[widest][0], 36.0f), juce::Justification::centred, false);
        }

        // A dot or handle being touched says where it is. Elsewhere, the mouse reads the frequency under it.
        const auto active = drag != Grab::none ? drag : hover;

        if (active != Grab::none)
        {
            const bool isBand = active != Grab::low && active != Grab::high;
            const int b = isBand ? (int) active - (int) Grab::band1 : 0;
            const auto anchor = isBand ? juce::Point<float> (xForHz (getReal (*bands[b].freq)), yForSensitivity (getReal (*bands[b].sens)) + 22.0f)
                                       : juce::Point<float> (xForHz (getReal (active == Grab::low ? *lowParam : *highParam)), h - 100.0f);
            drawTag (g, anchor, tagFor (active), tone::bone);
        }
        else if (mouseInside)
        {
            drawTag (g, { mousePos.x, h - 60.0f }, hertzText (hzForX (mousePos.x)), tone::boneSoft);
        }
    }

    // ---- mouse -----------------------------------------------------------------------------------------------

    ResonanceGraph::Grab ResonanceGraph::grabAt (juce::Point<float> p) const
    {
        Grab best = Grab::none;
        float bestDistance = 18.0f;

        for (int b = 0; b < numBands; ++b)
        {
            const float d = p.getDistanceFrom ({ xForHz (getReal (*bands[b].freq)), yForSensitivity (getReal (*bands[b].sens)) });

            if (d < bestDistance)
            {
                bestDistance = d;
                best = (Grab) ((int) Grab::band1 + b);
            }
        }

        if (best != Grab::none)
            return best;

        if (std::abs (p.x - xForHz (getReal (*lowParam))) < 8.0f)
            return Grab::low;

        if (std::abs (p.x - xForHz (getReal (*highParam))) < 8.0f)
            return Grab::high;

        return Grab::none;
    }

    void ResonanceGraph::updateHover (juce::Point<float> p)
    {
        const auto now = grabAt (p);
        mousePos = p;
        mouseInside = true;

        if (now != hover)
        {
            hover = now;

            setMouseCursor (now == Grab::none ? juce::MouseCursor::NormalCursor
                                              : (now == Grab::low || now == Grab::high ? juce::MouseCursor::LeftRightResizeCursor
                                                                                       : juce::MouseCursor::DraggingHandCursor));
            if (onHint != nullptr)
            {
                if (now == Grab::low)
                    onHint ("Nothing below this line is touched. Double-click to reset.");
                else if (now == Grab::high)
                    onHint ("Nothing above this line is touched. Double-click to reset.");
                else if (now != Grab::none)
                    onHint ("Up finds more here, down protects it. Scroll for width. Double-click to reset.");
                else
                    onHint ({});
            }
        }

        repaint();
    }

    void ResonanceGraph::mouseMove (const juce::MouseEvent& e)
    {
        updateHover (e.position);
    }

    void ResonanceGraph::mouseExit (const juce::MouseEvent&)
    {
        mouseInside = false;
        hover = Grab::none;

        if (onHint != nullptr)
            onHint ({});

        repaint();
    }

    void ResonanceGraph::setSelectedBand (int band)
    {
        band = juce::jlimit (0, numBands - 1, band);

        if (band != selectedBand)
        {
            selectedBand = band;
            repaint();

            if (onBandSelected != nullptr)
                onBandSelected (band);
        }
    }

    void ResonanceGraph::setHighlight (const juce::String& paramId)
    {
        if (highlight != paramId)
        {
            highlight = paramId;
            repaint();
        }
    }

    void ResonanceGraph::mouseDown (const juce::MouseEvent& e)
    {
        endGesture();
        drag = grabAt (e.position);
        lastDrag = e.position;
        logicalPos = e.position;
        widthDrag = false;

        // The drag starts from where the dot or handle really is, so a click a little off centre does not make it jump.
        if (drag == Grab::low)
        {
            logicalPos.x = xForHz (getReal (*lowParam));
            lowParam->beginChangeGesture();
        }
        else if (drag == Grab::high)
        {
            logicalPos.x = xForHz (getReal (*highParam));
            highParam->beginChangeGesture();
        }
        else if (drag != Grab::none)
        {
            const int b = (int) drag - (int) Grab::band1;
            setSelectedBand (b);
            widthDrag = e.mods.isCommandDown();
            logicalPos = { xForHz (getReal (*bands[b].freq)), yForSensitivity (getReal (*bands[b].sens)) };
            dragStartY = logicalPos.y;
            widthAtDragStart = getReal (*bands[b].width);

            if (widthDrag)
            {
                bands[b].width->beginChangeGesture();
            }
            else
            {
                bands[b].freq->beginChangeGesture();
                bands[b].sens->beginChangeGesture();
            }
        }

        repaint();
    }

    void ResonanceGraph::mouseDrag (const juce::MouseEvent& e)
    {
        mousePos = e.position;

        if (drag == Grab::none)
            return;

        // Shift gives a five times finer drag, any time during the drag.
        logicalPos += (e.position - lastDrag) * (e.mods.isShiftDown() ? 0.2f : 1.0f);
        lastDrag = e.position;

        if (drag == Grab::low)
        {
            setReal (*lowParam, juce::jmin (hzForX (logicalPos.x), getReal (*highParam)));
        }
        else if (drag == Grab::high)
        {
            setReal (*highParam, juce::jmax (hzForX (logicalPos.x), getReal (*lowParam)));
        }
        else
        {
            const auto& band = bands[(int) drag - (int) Grab::band1];

            if (widthDrag)
            {
                setReal (*band.width, juce::jlimit (0.1f, 4.0f, widthAtDragStart * std::pow (2.0f, (dragStartY - logicalPos.y) / 90.0f)));
            }
            else
            {
                setReal (*band.freq, juce::jlimit (20.0f, 20000.0f, hzForX (logicalPos.x)));
                setReal (*band.sens, juce::jlimit (-12.0f, 12.0f, sensitivityForY (logicalPos.y)));
            }
        }

        repaint();
    }

    void ResonanceGraph::mouseUp (const juce::MouseEvent& e)
    {
        endGesture();
        updateHover (e.position);
    }

    void ResonanceGraph::endGesture()
    {
        if (drag == Grab::low)
            lowParam->endChangeGesture();
        else if (drag == Grab::high)
            highParam->endChangeGesture();
        else if (drag != Grab::none)
        {
            const int b = (int) drag - (int) Grab::band1;

            if (widthDrag)
            {
                bands[b].width->endChangeGesture();
            }
            else
            {
                bands[b].freq->endChangeGesture();
                bands[b].sens->endChangeGesture();
            }
        }

        drag = Grab::none;
    }

    void ResonanceGraph::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const auto grab = grabAt (e.position);
        const auto reset = [] (juce::RangedAudioParameter& p)
        {
            p.beginChangeGesture();
            p.setValueNotifyingHost (p.getDefaultValue());
            p.endChangeGesture();
        };

        if (grab == Grab::low)
            reset (*lowParam);
        else if (grab == Grab::high)
            reset (*highParam);
        else if (grab != Grab::none)
        {
            const int b = (int) grab - (int) Grab::band1;
            reset (*bands[b].freq);
            reset (*bands[b].sens);
            reset (*bands[b].width);
        }

        repaint();
    }

    void ResonanceGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        const auto grab = grabAt (e.position);

        if (grab == Grab::none || grab == Grab::low || grab == Grab::high)
            return;

        const int b = (int) grab - (int) Grab::band1;
        setSelectedBand (b);
        const float now = getReal (*bands[b].width);
        setAsGesture (*bands[b].width, juce::jlimit (0.1f, 4.0f, now * std::pow (2.0f, wheel.deltaY * (wheel.isReversed ? -1.0f : 1.0f))));
        repaint();
    }
}
