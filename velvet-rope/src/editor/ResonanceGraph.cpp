#include "ResonanceGraph.h"

#include "../PluginProcessor.h"

namespace ui
{
    namespace
    {
        constexpr float loHz = 20.0f, hiHz = 20000.0f;
        constexpr float sensReach = 0.30f;        // +12 dB of sensitivity sits this fraction of the height above the middle
        constexpr float cutFullScale = 0.64f;     // 24 dB of reduction reaches this fraction of the height down from the ceiling
        constexpr float ceilingOffset = 2.0f;     // at rest the gold line hides just above the ceiling
        constexpr float levelFloor = -96.0f, levelTop = 10.0f;
        constexpr float tiltDbPerOctave = 4.5f;   // display only
        constexpr float cornerRadius = 10.0f;

        const juce::String bandNames[] = { "1", "2", "3", "4", "5", "6", "7", "8" };

        constexpr float gridHz[] = { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };

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
            bands[b] = { s.getParameter ("band" + number + "Freq"), s.getParameter ("band" + number + "Sens"), s.getParameter ("band" + number + "Width"), s.getParameter ("band" + number + "On") };
        }

        lowParam = s.getParameter ("lowFreq");
        highParam = s.getParameter ("highFreq");
        depthParam = s.getParameter ("depth");
        mixParam = s.getParameter ("mix");
        deltaParam = s.getParameter ("delta");
        bypassParam = s.getParameter ("bypass");
        maxCutParam = s.getParameter ("maxCut");
        listenParam = s.getParameter ("bandListen");

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
        // The graph is opaque, so its rounded corners are small pieces of window colour painted over it.
        const auto w = (float) getWidth(), h = (float) getHeight();
        const float r = cornerRadius;
        corners.clear();

        const auto addCorner = [this, r] (float x, float y, float dx, float dy)
        {
            corners.startNewSubPath (x, y);
            corners.lineTo (x + dx * r, y);
            corners.quadraticTo (x, y, x, y + dy * r);
            corners.closeSubPath();
        };

        addCorner (0.0f, 0.0f, 1.0f, 1.0f);
        addCorner (w, 0.0f, -1.0f, 1.0f);
        addCorner (0.0f, h, 1.0f, -1.0f);
        addCorner (w, h, -1.0f, -1.0f);

        for (int i = 0; i < numDense; ++i)
            denseX[(size_t) i] = w * (float) (i + 1) / (float) (numDense + 1);

        fullRepaint = true;
        staticLayerStale = true;
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

    float ResonanceGraph::yForReductionLine (float db) const noexcept
    {
        return std::round (yForReduction (db) + ceilingOffset);
    }

    int ResonanceGraph::denseIndexForX (float x) const noexcept
    {
        return juce::jlimit (0, numDense - 1, juce::roundToInt (x / (float) juce::jmax (1, getWidth()) * (float) (numDense + 1)) - 1);
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

        if (processor.readGraph (frame))
        {
            staleSeconds = 0.0;
            buildTargets();
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

        float loudest = -200.0f, deepest = 0.0f;

        for (size_t i = 0; i < (size_t) numPoints; ++i)
        {
            const float nextLevel = glide (shownLevel[i], targetLevel[i], 0.016f, 0.095f, dt);
            shownLevel[i] = nextLevel;
            loudest = juce::jmax (loudest, nextLevel);

            for (int slot = 0; slot < 2; ++slot)
            {
                const float nextCut = glide (shownCut[slot][i], targetCut[slot][i], 0.018f, 0.070f, dt);
                shownCut[slot][i] = nextCut;
                deepest = juce::jmax (deepest, nextCut);
            }
        }

        peakDb = deepest;

        bool fades = false;

        const auto settle = [&] (float& v, float target, float tau)
        {
            const float next = v + (target - v) * (1.0f - std::exp (-dt / tau));
            fades = fades || std::abs (next - v) > 0.0003f;
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
            mixIn (b.on);
        }

        mixIn (maxCutParam);
        mixIn (listenParam);

        mixIn (lowParam);
        mixIn (highParam);
        mixIn (depthParam);

        if (stamp != paramStamp)
        {
            paramStamp = stamp;
            selectAnotherIfOff();
            staticLayerStale = true;
            fullRepaint = true;
        }

        if (fades)
            fullRepaint = true;

        refreshCurves();
        return fullRepaint || ! voiceDirty.isEmpty() || ! goldDirty.isEmpty();
    }

    void ResonanceGraph::refreshCurves()
    {
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

        // What has moved by more than a quarter of a pixel on a 2x screen since the screen last showed it. Each point only
        // counts as shown once it has been asked for, so small changes add up instead of being lost.
        constexpr float threshold = 0.12f, margin = 3.0f;

        const auto compare = [&] (const std::array<float, numDense>& now, std::array<float, numDense>& shown, juce::Rectangle<float>& dirty)
        {
            for (size_t i = 0; i < (size_t) numDense; ++i)
            {
                if (std::abs (now[i] - shown[i]) > threshold)
                {
                    const auto area = juce::Rectangle<float> (denseX[i] - margin, juce::jmin (now[i], shown[i]) - margin,
                                                              margin * 2.0f, std::abs (now[i] - shown[i]) + margin * 2.0f);
                    dirty = dirty.isEmpty() ? area : dirty.getUnion (area);
                    shown[i] = now[i];
                }
            }
        };

        compare (yVoice, shownVoice, voiceDirty);
        compare (yShaved, shownShaved, voiceDirty);
        compare (yGold[0], shownGold[0], goldDirty);
        compare (yGold[1], shownGold[1], goldDirty);
    }

    void ResonanceGraph::flushRepaint()
    {
        if (fullRepaint)
        {
            repaint();
        }
        else
        {
            if (! voiceDirty.isEmpty())
                repaint (voiceDirty.getSmallestIntegerContainer());

            if (! goldDirty.isEmpty())
                repaint (goldDirty.getSmallestIntegerContainer());
        }

        fullRepaint = false;
        voiceDirty = {};
        goldDirty = {};
    }

    void ResonanceGraph::simplify (const std::array<float, numDense>& ys, int from, int to)
    {
        // Keeps a sleeve of slopes from the last kept point, and only keeps a new point when the sleeve closes.
        constexpr float tolerance = 0.15f;   // under a third of a pixel on a 2x screen

        const auto keep = [this, &ys] (int i)
        {
            simpleX[(size_t) numSimple] = denseX[(size_t) i];
            simpleY[(size_t) numSimple] = ys[(size_t) i];
            ++numSimple;
        };

        keep (from);
        int anchor = from;
        float low = -1.0e9f, high = 1.0e9f;

        for (int i = from + 1; i <= to; ++i)
        {
            const float run = (float) (i - anchor);
            const float rise = ys[(size_t) i] - ys[(size_t) anchor];
            const float newLow = juce::jmax (low, (rise - tolerance) / run), newHigh = juce::jmin (high, (rise + tolerance) / run);

            if (newLow > newHigh)
            {
                keep (i - 1);
                anchor = i - 1;
                const float restart = ys[(size_t) i] - ys[(size_t) anchor];
                low = restart - tolerance;
                high = restart + tolerance;
            }
            else
            {
                low = newLow;
                high = newHigh;
            }
        }

        keep (to);
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

    void ResonanceGraph::fillRangedRibbon (juce::Graphics& g, const float* ys, const float* xs, int count, int lowIndex, int highIndex, float halfWidth, juce::Colour inside, juce::Colour outside)
    {
        const auto segment = [&] (int from, int to, juce::Colour colour)
        {
            if (to - from >= 1)
            {
                buildRibbon (ys + from, xs + from, to - from + 1, halfWidth);
                g.setColour (colour);
                g.fillPath (ribbon);
            }
        };

        segment (0, lowIndex, outside);
        segment (highIndex, count - 1, outside);
        segment (lowIndex, highIndex, inside);
    }

    // ---- drawing ---------------------------------------------------------------------------------------------

    void ResonanceGraph::paint (juce::Graphics& g)
    {
        const auto w = (float) getWidth();
        ++paintCount;
        paintedArea += (double) g.getClipBounds().getWidth() * (double) g.getClipBounds().getHeight();
        const auto started = juce::Time::getMillisecondCounterHiRes();

        const float lowX = juce::jlimit (0.0f, w, xForHz (getReal (*lowParam)));
        const float highX = juce::jlimit (lowX, w, xForHz (getReal (*highParam)));
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        drawVoice (g, lowX, highX);
        drawGold (g);

        refreshStaticLayer (scale, lowX, highX);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (staticLayer, juce::AffineTransform::scale (1.0f / scale));

        drawOverlays (g);

        paintSeconds += (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    }

    void ResonanceGraph::refreshStaticLayer (float scale, float lowX, float highX)
    {
        const int pixelsWide = juce::jmax (1, juce::roundToInt ((float) getWidth() * scale)), pixelsHigh = juce::jmax (1, juce::roundToInt ((float) getHeight() * scale));

        if (staticLayer.getWidth() != pixelsWide || staticLayer.getHeight() != pixelsHigh)
        {
            staticLayer = juce::Image (juce::Image::ARGB, pixelsWide, pixelsHigh, true);
            staticLayerStale = true;
        }

        if (! staticLayerStale && scale == staticScale)
            return;

        staticLayerStale = false;
        staticScale = scale;
        staticLayer.clear (staticLayer.getBounds());

        juce::Graphics layer (staticLayer);
        layer.addTransform (juce::AffineTransform::scale (scale));
        drawGrid (layer);
        drawSensitivity (layer, lowX, highX);
        drawHandles (layer);
        layer.setColour (tone::base);
        layer.fillPath (corners);
    }

    void ResonanceGraph::refreshVoicePicture (float scale, float lowX, float highX)
    {
        const auto w = (float) getWidth(), h = (float) getHeight();
        const int pixelsWide = juce::jmax (1, juce::roundToInt (w * scale)), pixelsHigh = juce::jmax (1, juce::roundToInt (h * scale));
        const bool wrongSize = voicePicture.getWidth() != pixelsWide || voicePicture.getHeight() != pixelsHigh;

        if (! (wrongSize || scale != pictureScale || lowX != pictureLowX || highX != pictureHighX || deltaShown != pictureDelta))
            return;

        pictureScale = scale;
        pictureLowX = lowX;
        pictureHighX = highX;
        pictureDelta = deltaShown;

        if (wrongSize)
            voicePicture = juce::Image (juce::Image::RGB, pixelsWide, pixelsHigh, false);

        juce::Graphics picture (voicePicture);
        picture.addTransform (juce::AffineTransform::scale (scale));

        constexpr int bandsOfVoice = 240;
        constexpr float dimOutside = 0.62f;

        for (int band = 0; band < bandsOfVoice; ++band)
        {
            const float t = ((float) band + 0.5f) / (float) bandsOfVoice;
            const float top = h * (float) band / (float) bandsOfVoice;
            const float tall = h / (float) bandsOfVoice + 1.0f;   // each covers the last one's lower edge, so no seams at any size
            auto colour = t < 0.48f ? tone::voiceTop.interpolatedWith (tone::cobalt, juce::jlimit (0.0f, 1.0f, (t - 0.18f) / 0.30f))
                                    : tone::cobalt.interpolatedWith (tone::voiceLow, (t - 0.48f) / 0.52f);
            colour = colour.interpolatedWith (tone::well, 0.76f * deltaShown);
            picture.setColour (colour);
            picture.fillRect (juce::Rectangle<float> (lowX, top, highX - lowX, tall));
            picture.setColour (colour.interpolatedWith (tone::well, dimOutside));
            picture.fillRect (juce::Rectangle<float> (0.0f, top, lowX, tall));
            picture.fillRect (juce::Rectangle<float> (highX, top, w - highX, tall));
        }
    }

    void ResonanceGraph::drawVoice (juce::Graphics& g, float lowX, float highX)
    {
        const auto w = (float) getWidth();

        // The voice is a body of light standing on the floor: deep at the base, pale where it is loud, dimmer outside Low
        // and High, where the sound is left alone. Painted inside out to stay fast: the finished colours go down as one
        // opaque picture, then everything above the voice is covered with flat well colour.
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        refreshVoicePicture (scale, lowX, highX);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (voicePicture, juce::AffineTransform::scale (1.0f / scale));

        // Everything above what you hear after treatment.
        numSimple = 0;
        simplify (yShaved, 0, numDense - 1);
        const int shavedPoints = numSimple;

        shape.clear();
        shape.startNewSubPath (-2.0f, -2.0f);
        shape.lineTo (w + 2.0f, -2.0f);
        shape.lineTo (w + 2.0f, simpleY[(size_t) shavedPoints - 1]);

        for (int i = shavedPoints; --i >= 0;)
            shape.lineTo (simpleX[(size_t) i], simpleY[(size_t) i]);

        shape.lineTo (-2.0f, simpleY[0]);
        shape.closeSubPath();
        g.setColour (tone::well);
        g.fillPath (shape);

        // What was shaved off, in gold, right where it came off the voice. Only where something was.
        int first = -1, last = -1;

        for (int i = 0; i < numDense; ++i)
        {
            if (yShaved[(size_t) i] - yVoice[(size_t) i] > 0.3f)
            {
                if (first < 0)
                    first = i;

                last = i;
            }
        }

        if (first >= 0)
        {
            first = juce::jmax (0, first - 1);
            last = juce::jmin (numDense - 1, last + 1);

            numSimple = 0;
            simplify (yVoice, first, last);
            shape.clear();
            shape.startNewSubPath (simpleX[0], simpleY[0]);

            for (int i = 1; i < numSimple; ++i)
                shape.lineTo (simpleX[(size_t) i], simpleY[(size_t) i]);

            numSimple = 0;
            simplify (yShaved, first, last);

            for (int i = numSimple; --i >= 0;)
                shape.lineTo (simpleX[(size_t) i], simpleY[(size_t) i]);

            shape.closeSubPath();
            g.setColour (tone::gold.interpolatedWith (tone::faint, bypassShown).withAlpha (0.82f - 0.5f * bypassShown));
            g.fillPath (shape);
        }

        // The edge is the sound after treatment, bright inside Low and High and faint outside them.
        const float presence = 1.0f - 0.7f * deltaShown;
        const int lowIndex = denseIndexForX (lowX), highIndex = denseIndexForX (highX);

        // The two faint ends go in one path, because each extra fill costs more than its points.
        ribbon.clear();
        auto faintEnds = juce::Path();
        faintEnds.preallocateSpace (256);

        const auto addEnd = [&] (int from, int to)
        {
            if (to > from)
            {
                numSimple = 0;
                simplify (yShaved, from, to);
                buildRibbon (simpleY.data(), simpleX.data(), numSimple, 0.6f);
                faintEnds.addPath (ribbon);
            }
        };

        addEnd (0, lowIndex);
        addEnd (highIndex, numDense - 1);

        if (! faintEnds.isEmpty())
        {
            g.setColour (tone::voiceEdge.withAlpha (0.32f * presence));
            g.fillPath (faintEnds);
        }

        if (highIndex > lowIndex)
        {
            numSimple = 0;
            simplify (yShaved, lowIndex, highIndex);
            buildRibbon (simpleY.data(), simpleX.data(), numSimple, 0.6f);
            g.setColour (tone::voiceEdge.withAlpha (presence));
            g.fillPath (ribbon);
        }
    }

    void ResonanceGraph::drawGrid (juce::Graphics& g)
    {
        const auto w = (float) getWidth(), h = (float) getHeight();

        g.setColour (tone::text.withAlpha (0.05f));

        for (auto hz : gridHz)
            g.fillRect (juce::Rectangle<float> (std::round (xForHz (hz)), 0.0f, 1.0f, h));

        for (int db = 6; db <= 24; db += 6)
            g.fillRect (juce::Rectangle<float> (0.0f, yForReductionLine ((float) db), w, 1.0f));
    }

    void ResonanceGraph::drawGold (juce::Graphics& g)
    {
        const auto w = (float) getWidth();
        const float dim = 1.0f - 0.68f * bypassShown;
        const auto gold = tone::gold.interpolatedWith (tone::faint, bypassShown);
        const auto edge = tone::goldEdge.interpolatedWith (tone::muted, bypassShown);

        // Only the part of the width with a cut in it is drawn, and at rest there is nothing to draw at all.
        const float atRest = -ceilingOffset + 0.5f;

        for (int slot = 0; slot < (slotsApart ? 2 : 1); ++slot)   // the second channel goes on top, or the gold would hide it
        {
            const auto& ys = yGold[slot];
            int first = -1, last = -1;

            for (int i = 0; i < numDense; ++i)
            {
                if (ys[(size_t) i] > atRest)
                {
                    if (first < 0)
                        first = i;

                    last = i;
                }
            }

            if (first < 0)
                continue;

            first = juce::jmax (0, first - 1);
            last = juce::jmin (numDense - 1, last + 1);

            numSimple = 0;
            simplify (ys, first, last);

            if (slot == 1)
            {
                buildRibbon (simpleY.data(), simpleX.data(), numSimple, 0.7f);
                g.setColour (tone::second.withAlpha (dim));
                g.fillPath (ribbon);
                continue;
            }

            shape.clear();
            shape.startNewSubPath (simpleX[0], -4.0f);

            for (int i = 0; i < numSimple; ++i)
                shape.lineTo (simpleX[(size_t) i], simpleY[(size_t) i]);

            shape.lineTo (juce::jmin (w, simpleX[(size_t) numSimple - 1]), -4.0f);
            shape.closeSubPath();
            g.setColour (gold.withAlpha (0.86f * dim));
            g.fillPath (shape);

            buildRibbon (simpleY.data(), simpleX.data(), numSimple, 0.7f);
            g.setColour (edge.withAlpha (dim));
            g.fillPath (ribbon);
        }
    }

    void ResonanceGraph::drawSensitivity (juce::Graphics& g, float lowX, float highX)
    {
        const auto w = (float) getWidth();

        // The same bell the engine uses, drawn from the parameters so it moves with no audio running.
        float amounts[numBands], centres[numBands], sigmas[numBands];

        for (int b = 0; b < numBands; ++b)
        {
            amounts[b] = isBandOn (b) ? getReal (*bands[b].sens) : 0.0f;
            centres[b] = std::log2 (juce::jmax (getReal (*bands[b].freq), 1.0f));
            sigmas[b] = juce::jmax (getReal (*bands[b].width), 0.05f) / 2.3548f;
        }

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
        }

        // Bright where it has an effect, faint where Low and High leave the sound alone.
        const auto indexFor = [&] (float x) { return juce::jlimit (0, numSensSteps - 1, juce::roundToInt (x / w * (float) (numSensSteps - 1))); };
        fillRangedRibbon (g, sensY.data(), sensX.data(), numSensSteps, indexFor (lowX), indexFor (highX), 0.75f, tone::text, tone::text.withAlpha (0.3f));

        // The dots.
        for (int b = 0; b < numBands; ++b)
        {
            if (! isBandOn (b))
                continue;

            const auto grab = (Grab) ((int) Grab::band1 + b);
            const auto c = dotCentre (b);
            const bool isSelected = b == selectedBand;
            const bool lit = hover == grab || drag == grab;
            const float r = 10.0f;

            if (isSelected)
            {
                // The selected dot shows how wide it reaches.
                const float halfWidth = getReal (*bands[b].width) * 0.5f;
                const float hz = getReal (*bands[b].freq);
                const float x1 = xForHz (hz * std::pow (2.0f, -halfWidth)), x2 = xForHz (hz * std::pow (2.0f, halfWidth));
                g.setColour (tone::text.withAlpha (0.4f));
                g.fillRect (juce::Rectangle<float> (x1, c.y - 0.5f, x2 - x1, 1.0f));
                g.fillRect (juce::Rectangle<float> (x1 - 0.5f, c.y - 4.0f, 1.0f, 8.0f));
                g.fillRect (juce::Rectangle<float> (x2 - 0.5f, c.y - 4.0f, 1.0f, 8.0f));
            }

            if (lit)
            {
                g.setColour (tone::text.withAlpha (0.16f));
                g.fillEllipse (c.x - r - 6.0f, c.y - r - 6.0f, (r + 6.0f) * 2.0f, (r + 6.0f) * 2.0f);
            }

            g.setColour (isSelected ? tone::text : tone::well);
            g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);

            if (! isSelected)
            {
                g.setColour (tone::text);
                g.drawEllipse (c.x - r + 0.6f, c.y - r + 0.6f, (r - 0.6f) * 2.0f, (r - 0.6f) * 2.0f, 1.2f);
            }

            g.setColour (isSelected ? tone::well : tone::text);
            g.setFont (font (Weight::medium, 12.0f));
            g.drawText (bandNames[b], juce::Rectangle<float> (c.x - r, c.y - r, r * 2.0f, r * 2.0f), juce::Justification::centred, false);

            // A hovered dot offers its own way out.
            if (hover == grab && drag == Grab::none)
            {
                const auto badge = removeBadge (b);
                g.setColour (overRemove ? tone::text : tone::base.brighter (0.5f));
                g.fillEllipse (badge);
                g.setColour (overRemove ? tone::well : tone::text);
                const auto m = badge.reduced (4.2f);
                juce::Path cross;
                cross.startNewSubPath (m.getTopLeft());
                cross.lineTo (m.getBottomRight());
                cross.startNewSubPath (m.getTopRight());
                cross.lineTo (m.getBottomLeft());
                g.strokePath (cross, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
    }

    void ResonanceGraph::drawHandles (juce::Graphics& g)
    {
        const auto h = (float) getHeight();

        const auto drawHandle = [&] (Grab grab, float x, const juce::String& text, bool textOnRight)
        {
            const bool lit = hover == grab || drag == grab;
            g.setColour (tone::text.withAlpha (lit ? 0.85f : 0.32f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, 0.0f, 1.0f, h));

            const auto box = juce::Rectangle<float> (textOnRight ? x + 6.0f : x - 40.0f, h - 26.0f, 34.0f, 18.0f);
            g.setColour (tone::text.withAlpha (lit ? 1.0f : 0.8f));
            g.setFont (font (Weight::medium, 12.0f));
            g.drawText (text, box, textOnRight ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
        };

        drawHandle (Grab::low, xForHz (getReal (*lowParam)), "Low", true);
        drawHandle (Grab::high, xForHz (getReal (*highParam)), "High", false);
    }

    void ResonanceGraph::drawTag (juce::Graphics& g, juce::Point<float> anchor, const juce::String& text, juce::Colour textColour)
    {
        const auto f = font (Weight::medium, 12.5f);
        const float width = juce::GlyphArrangement::getStringWidth (f, text) + 18.0f;
        auto box = juce::Rectangle<float> (anchor.x - width * 0.5f, anchor.y, width, 24.0f);
        box = box.withX (juce::jlimit (6.0f, juce::jmax (6.0f, (float) getWidth() - width - 6.0f), box.getX()));
        g.setColour (tone::base.brighter (0.25f));
        g.fillRoundedRectangle (box, 6.0f);
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

        // While listening to one dot, what you do not hear steps back, and the edges of what you do hear are marked.
        const int listening = getListeningBand();
        const float listenEnds = 1.0f - bypassShown;

        if (listening >= 0 && listenEnds > 0.01f)
        {
            const float hz = getReal (*bands[listening].freq), half = getReal (*bands[listening].width) * 0.5f;
            const float x1 = juce::jmax (0.0f, xForHz (hz * std::pow (2.0f, -half))), x2 = juce::jmin (w, xForHz (hz * std::pow (2.0f, half)));
            g.setColour (tone::well.withAlpha (0.78f * listenEnds));
            g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, x1, h));
            g.fillRect (juce::Rectangle<float> (x2, 0.0f, w - x2, h));
            g.setColour (tone::listen.withAlpha (listenEnds));
            g.fillRect (juce::Rectangle<float> (x1 - 0.75f, 0.0f, 1.5f, h));
            g.fillRect (juce::Rectangle<float> (x2 - 0.75f, 0.0f, 1.5f, h));
        }

        // Hovering Depth or Max Cut draws the limit it sets.
        if (highlight == "depth" || highlight == "maxCut")
        {
            const bool ceilingOnly = highlight == "maxCut";
            const float y = yForReductionLine (ceilingOnly ? getReal (*maxCutParam) : juce::jmin (getReal (*depthParam), getReal (*maxCutParam)));
            juce::Path line;
            line.startNewSubPath (0.0f, y);
            line.lineTo (w, y);
            const float dashes[] = { 4.0f, 4.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
            g.setColour (tone::goldEdge);
            g.fillPath (dashed);
            g.setFont (font (Weight::regular, 12.5f));
            g.drawText (ceilingOnly ? "Max Cut" : "Deepest cut", juce::Rectangle<float> (14.0f, y - 22.0f, 200.0f, 20.0f), juce::Justification::centredLeft, false);
        }
        else if (highlight == "lowFreq" || highlight == "highFreq")
        {
            const float x = xForHz (getReal (highlight == "lowFreq" ? *lowParam : *highParam));
            g.setColour (tone::text.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, 0.0f, 1.0f, h));
        }

        // What the sound is doing now, when it is not the normal treated sound.
        const auto status = [&] (const juce::String& text, juce::Colour colour, float alpha)
        {
            if (alpha < 0.01f)
                return;

            g.setColour (colour.withAlpha (alpha));
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (text, juce::Rectangle<float> (16.0f, 12.0f, 400.0f, 20.0f), juce::Justification::centredLeft, false);
        };

        if (bypassShown > 0.01f)
            status ("Bypassed: you hear the untouched sound", tone::text, bypassShown);
        else if (listening < 0)
            status ("Delta: you hear only what is being taken out", tone::goldEdge, deltaShown);

        // Band Listen changes what comes out of the track, so it cannot be missed: a solid banner, and a click on it stops it.
        if (listening >= 0 && bypassShown < 0.99f)
        {
            const auto box = listenBanner();
            g.setColour (tone::listen.withMultipliedAlpha (1.0f - bypassShown));
            g.fillRoundedRectangle (box, 7.0f);
            g.setColour (tone::well.withMultipliedAlpha (1.0f - bypassShown));
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText ((deltaShown > 0.5f ? "Listening to what dot " + bandNames[listening] + " loses" : "Listening to dot " + bandNames[listening] + " only") + ". Click to stop.",
                        box, juce::Justification::centred, false);
        }

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

            g.setColour (tone::muted.withAlpha (hintShown));
            g.setFont (font (Weight::regular, 15.0f));
            g.drawText ("Play something to see it here", juce::Rectangle<float> (gaps[widest][0], h * 0.72f, gaps[widest][1] - gaps[widest][0], 30.0f), juce::Justification::centred, false);
        }

        // A dot or handle being touched says where it is. Elsewhere, the mouse reads the frequency under it.
        const auto active = drag != Grab::none ? drag : hover;

        if (active != Grab::none)
        {
            const bool isBand = active != Grab::low && active != Grab::high;
            const int b = isBand ? (int) active - (int) Grab::band1 : 0;
            const auto anchor = isBand ? juce::Point<float> (xForHz (getReal (*bands[b].freq)), yForSensitivity (getReal (*bands[b].sens)) + 20.0f)
                                       : juce::Point<float> (xForHz (getReal (active == Grab::low ? *lowParam : *highParam)), h - 62.0f);
            drawTag (g, anchor, tagFor (active), tone::text);
        }
        else if (mouseInside)
        {
            drawTag (g, { mousePos.x, h - 34.0f }, hertzText (hzForX (mousePos.x)), tone::muted);
        }
    }

    // ---- mouse -----------------------------------------------------------------------------------------------

    juce::Point<float> ResonanceGraph::dotCentre (int band) const
    {
        return { xForHz (getReal (*bands[band].freq)), yForSensitivity (getReal (*bands[band].sens)) };
    }

    juce::Rectangle<float> ResonanceGraph::removeBadge (int band) const
    {
        const auto c = dotCentre (band);
        return { c.x + 8.0f, c.y - 22.0f, 14.0f, 14.0f };
    }

    juce::Rectangle<float> ResonanceGraph::listenBanner() const
    {
        return { 10.0f, 10.0f, 300.0f, 28.0f };
    }

    int ResonanceGraph::getListeningBand() const noexcept
    {
        return juce::roundToInt (getReal (*listenParam)) - 1;
    }

    int ResonanceGraph::addBandAt (juce::Point<float> p)
    {
        for (int b = 0; b < numBands; ++b)
        {
            if (! isBandOn (b))
            {
                setAsGesture (*bands[b].freq, juce::jlimit (20.0f, 20000.0f, hzForX (p.x)));
                setAsGesture (*bands[b].sens, juce::jlimit (-12.0f, 12.0f, sensitivityForY (p.y)));
                setAsGesture (*bands[b].width, bands[b].width->convertFrom0to1 (bands[b].width->getDefaultValue()));
                addBand (b);
                return b;
            }
        }

        if (onHint != nullptr)
            onHint ("All 8 dots are in use. Take one away to add another.");

        return -1;
    }

    void ResonanceGraph::addBand (int band)
    {
        setAsGesture (*bands[band].on, 1.0f);
        setSelectedBand (band);
        staticLayerStale = true;
        repaint();
    }

    void ResonanceGraph::removeBand (int band)
    {
        if (getListeningBand() == band)
            setAsGesture (*listenParam, 0.0f);

        setAsGesture (*bands[band].on, 0.0f);

        if (hover == (Grab) ((int) Grab::band1 + band))
            hover = Grab::none;

        overRemove = false;
        selectAnotherIfOff();
        staticLayerStale = true;
        repaint();
    }

    void ResonanceGraph::selectAnotherIfOff()
    {
        if (isBandOn (selectedBand))
            return;

        // The nearest on dot, by number. With none left the selection stays, and the strip shows it as off.
        for (int distance = 1; distance < numBands; ++distance)
        {
            for (int candidate : { selectedBand + distance, selectedBand - distance })
            {
                if (juce::isPositiveAndBelow (candidate, numBands) && isBandOn (candidate))
                {
                    setSelectedBand (candidate);
                    return;
                }
            }
        }
    }

    void ResonanceGraph::toggleListen (int band)
    {
        setAsGesture (*listenParam, getListeningBand() == band ? 0.0f : (float) band + 1.0f);
        repaint();
    }

    void ResonanceGraph::showDotMenu (int band, juce::Point<float> p)
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        menu.addItem (1, getListeningBand() == band ? "Stop listening" : "Listen to dot " + bandNames[band]);
        menu.addItem (2, "Reset dot " + bandNames[band]);
        menu.addItem (3, "Remove dot " + bandNames[band]);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (1, 1).withPosition (localPointToGlobal (p.toInt()))).withStandardItemHeight (28),
                            [safe = juce::Component::SafePointer<ResonanceGraph> (this), band] (int result)
                            {
                                if (safe == nullptr)
                                    return;

                                if (result == 1)
                                {
                                    safe->setSelectedBand (band);
                                    safe->toggleListen (band);
                                }
                                else if (result == 2)
                                {
                                    for (auto* prm : { safe->bands[band].freq, safe->bands[band].sens, safe->bands[band].width })
                                        setAsGesture (*prm, prm->convertFrom0to1 (prm->getDefaultValue()));
                                }
                                else if (result == 3)
                                {
                                    safe->removeBand (band);
                                }
                            });
    }

    void ResonanceGraph::showEmptyMenu (juce::Point<float> p)
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        bool anyFree = false;

        for (int b = 0; b < numBands; ++b)
            anyFree = anyFree || ! isBandOn (b);

        menu.addItem (1, anyFree ? "Add a dot here" : "All 8 dots are in use", anyFree);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (1, 1).withPosition (localPointToGlobal (p.toInt()))).withStandardItemHeight (28),
                            [safe = juce::Component::SafePointer<ResonanceGraph> (this), p] (int result)
                            {
                                if (safe != nullptr && result == 1)
                                    safe->addBandAt (p);
                            });
    }

    ResonanceGraph::Grab ResonanceGraph::grabAt (juce::Point<float> p) const
    {
        // The small x sits just outside the dot it belongs to, and only counts while that dot is hovered.
        if ((int) hover >= (int) Grab::band1 && removeBadge ((int) hover - (int) Grab::band1).expanded (2.0f).contains (p))
            return hover;

        Grab best = Grab::none;
        float bestDistance = 18.0f;

        for (int b = 0; b < numBands; ++b)
        {
            if (! isBandOn (b))
                continue;

            const float d = p.getDistanceFrom (dotCentre (b));

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

        const bool nowOverRemove = (int) now >= (int) Grab::band1 && removeBadge ((int) now - (int) Grab::band1).expanded (2.0f).contains (p);

        if (nowOverRemove != overRemove)
        {
            overRemove = nowOverRemove;
            staticLayerStale = true;

            if (onHint != nullptr && overRemove)
                onHint ("Take this dot away.");
        }

        if (now != hover)
        {
            hover = now;

            staticLayerStale = true;

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
                    onHint ("Up finds more here, down protects it. Scroll for width. The x takes it away.");
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

        if (hover != Grab::none)
            staticLayerStale = true;

        hover = Grab::none;
        overRemove = false;

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
            staticLayerStale = true;
            repaint();

            if (getListeningBand() >= 0 && isBandOn (band))
                setAsGesture (*listenParam, (float) band + 1.0f);

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
        staticLayerStale = true;

        if (getListeningBand() >= 0 && listenBanner().contains (e.position))
        {
            setAsGesture (*listenParam, 0.0f);
            repaint();
            return;
        }

        const auto touched = grabAt (e.position);

        if (e.mods.isPopupMenu())
        {
            if ((int) touched >= (int) Grab::band1)
                showDotMenu ((int) touched - (int) Grab::band1, e.position);
            else
                showEmptyMenu (e.position);

            return;
        }

        if ((int) touched >= (int) Grab::band1 && touched == hover && removeBadge ((int) touched - (int) Grab::band1).expanded (2.0f).contains (e.position))
        {
            removeBand ((int) touched - (int) Grab::band1);
            return;
        }

        drag = touched;
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
        staticLayerStale = true;
    }

    void ResonanceGraph::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const auto grab = grabAt (e.position);

        if (grab == Grab::none && ! listenBanner().contains (e.position))
        {
            addBandAt (e.position);
            return;
        }

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
