#include "TractView.h"

namespace ui
{
    namespace
    {
        // The tube's place in the view. It stays in the top and left, so that it can grow along both edges while the space under
        // the bend stays free for the formants.
        constexpr float viewW = 744.0f, viewH = 530.0f;
        constexpr float spineX = 122.0f, spineTop = 130.0f, bendRadius = 72.0f;
        constexpr float throatRun = 186.0f, mouthRun = 318.0f;
        constexpr float tubeRadius = 42.0f;

        // Widths and lengths span far more than fits. Both are drawn on a log scale, a little compressed.
        constexpr float widthCompress = 0.72f, lengthCompress = 0.6f;

        // How far the live voice moves the walls.
        constexpr float liveDepth = 1.25f;

        // Where the formants sit in a plain tube of the reference length: the shape they say is the difference from these.
        constexpr float plainTube[5] = { 500.0f, 1500.0f, 2500.0f, 3500.0f, 4500.0f };
        constexpr float formantWeight[5] = { 1.0f, 1.0f, 0.7f, 0.45f, 0.3f };

        // Where each formant can be in a voice. Lower than the first is the pitch itself showing through.
        constexpr float formantLow[5]  = { 260.0f, 750.0f, 1900.0f, 3000.0f, 3900.0f };
        constexpr float formantHigh[5] = { 1150.0f, 2700.0f, 3700.0f, 4900.0f, 6400.0f };
        constexpr float holdSeconds = 0.35f;

        // A resting silhouette, so that the neutral shape reads as a throat and not a pipe. Pairs of place and radius.
        constexpr float silhouette[][2] = { { 0.0f, 0.46f }, { 0.05f, 0.58f }, { 0.16f, 0.92f }, { 0.32f, 1.04f }, { 0.48f, 0.90f },
                                            { 0.66f, 1.10f }, { 0.86f, 1.04f }, { 1.0f, 0.90f } };

        constexpr float placeLow[TractView::numPoints]  = { 0.0f, 0.15f, 0.38f, 0.58f, 0.82f };
        constexpr float placeHigh[TractView::numPoints] = { 0.0f, 0.38f, 0.58f, 0.82f, 1.0f };

        const char* const widthIds[TractView::numPoints] = { "width1", "width2", "width3", "width4", "width5" };
        const char* const placeIds[TractView::numPoints] = { nullptr, "place2", "place3", "place4", "place5" };

        // The formants' plot.
        const juce::Rectangle<float> plot (306.0f, 344.0f, 410.0f, 138.0f);
        constexpr float plotSpanDb = 62.0f;   // The top of the plot follows the voice, because it is the shape that matters here.

        // The output meter, level with the mouth.
        const juce::Rectangle<float> meter (718.0f, 44.0f, 4.0f, 190.0f);
        constexpr float meterTopDb = 6.0f, meterBottomDb = -60.0f;

        constexpr int mapBins = 720;
        constexpr double mapBinHz = 25.0;

        float restingRadius (float u) noexcept
        {
            constexpr int n = (int) (sizeof (silhouette) / sizeof (silhouette[0]));
            int a = 0;
            while (a < n - 2 && u > silhouette[a + 1][0]) ++a;
            const float t = juce::jlimit (0.0f, 1.0f, (u - silhouette[a][0]) / (silhouette[a + 1][0] - silhouette[a][0]));
            const float eased = t * t * (3.0f - 2.0f * t);
            return silhouette[a][1] + eased * (silhouette[a + 1][1] - silhouette[a][1]);
        }

        float drawnLength (float length) noexcept  { return std::pow (length, lengthCompress); }
    }

    // ---- the live numbers under the bend: only repaints when a number changes, about six times a second at most --------

    class TractView::Numbers final : public juce::Component, public Hinted
    {
    public:
        Numbers()
        {
            setOpaque (true);
            setInterceptsMouseClicks (true, false);
            hint = "The voice's pitch and its resonances, in Hz. Bone is the singer, red is where the model puts them. Pitch never moves.";
        }

        void update (float pitchHz, bool voiced, const std::array<float, 5>& original, const std::array<float, 5>& model, bool modelIsOff)
        {
            std::array<juce::String, 12> next;
            const auto number = [] (float hz) { return hz > 1.0f ? juce::String (juce::roundToInt (hz / 10.0f) * 10) : juce::String ("-"); };
            next[0] = voiced && pitchHz > 1.0f ? juce::String (juce::roundToInt (pitchHz)) : juce::String ("-");
            next[6] = next[0];

            for (size_t i = 0; i < 5; ++i)
            {
                next[1 + i] = number (original[i]);
                next[7 + i] = number (model[i]);
            }

            if (next != cells || modelIsOff != off)
            {
                cells = next;
                off = modelIsOff;
                repaint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (tone::well);
            const float labelWidth = 68.0f, column = ((float) getWidth() - labelWidth) / 6.0f;
            const char* const heads[] = { "Pitch", "F1", "F2", "F3", "F4", "F5" };

            g.setFont (font (Weight::regular, 12.0f));
            g.setColour (tone::faint);
            g.drawText ("Hz", juce::Rectangle<float> (0.0f, 0.0f, labelWidth, 16.0f), juce::Justification::centredLeft, false);

            for (int i = 0; i < 6; ++i)
                g.drawText (heads[i], juce::Rectangle<float> (labelWidth + column * (float) i, 0.0f, column, 16.0f), juce::Justification::centredLeft, false);

            const auto row = [&] (const char* name, float y, juce::Colour colour, int first)
            {
                g.setColour (colour);
                g.setFont (font (Weight::regular, 13.0f));
                g.drawText (name, juce::Rectangle<float> (0.0f, y, labelWidth, 20.0f), juce::Justification::centredLeft, false);
                g.setFont (font (Weight::medium, 15.0f));

                for (int i = 0; i < 6; ++i)
                    g.drawText (cells[(size_t) (first + i)], juce::Rectangle<float> (labelWidth + column * (float) i, y, column, 20.0f), juce::Justification::centredLeft, false);
            };

            row ("Original", 19.0f, tone::text, 0);
            row ("Model", 40.0f, off ? tone::faint : tone::oxideLine, 6);
        }

    private:
        std::array<juce::String, 12> cells;
        bool off = false;
    };

    // ----------------------------------------------------------------------------------------------------------------

    const char* TractView::pointName (int i)
    {
        static const char* const names[] = { "Cords", "Throat", "Back", "Roof", "Lips" };
        return names[juce::jlimit (0, numPoints - 1, i)];
    }

    juce::String TractView::pointHint (int i)
    {
        switch (i)
        {
            case 0:  return "Vocal cords. Drag out or in to widen or narrow this end of the tract. Double-click to reset.";
            case 1:  return "Throat. Wider sounds fuller, narrower sounds pinched. Drag along the tube to move it. Double-click to reset.";
            case 2:  return "Back of the mouth. Changes the color of the vowels. Drag along the tube to move it. Double-click to reset.";
            case 3:  return "Roof of the mouth. Narrower leans toward an ee sound. Drag along the tube to move it. Double-click to reset.";
            case 4:  return "Lips. Narrower rounds and darkens the voice, wider opens it up. Drag along the tube to move it. Double-click to reset.";
            default: return {};
        }
    }

    TractView::TractView (PluginProcessor& p) : processor (p)
    {
        setOpaque (true);
        setTitle ("Vocal tract");
        modelMap.resize ((size_t) mapBins);

        // Each formant's own pattern along the tube: a resonance at f rises where the tube narrows near a pressure peak, and
        // falls where it narrows near a flow peak. Turned around, a formant that sits high says what shape made it.
        for (int i = 0; i < numSamples; ++i)
            for (int n = 0; n < numFormants; ++n)
                shapeBasis[(size_t) i][(size_t) n] = std::cos ((float) (2 * n + 1) * juce::MathConstants<float>::pi * (float) i / (float) (numSamples - 1));

        for (auto* a : { &inputDb, &outputDb })
            a->fill (plotTop - plotSpanDb);

        numbers = std::make_unique<Numbers>();
        numbers->setBounds (306, 272, 410, 62);
        addAndMakeVisible (*numbers);

        placeTube (singerTube, 1.0f);
        readParameters();
        readAudio (1.0);
        buildShapes();
    }

    TractView::~TractView() = default;

    void TractView::resized()
    {
        layersStale = true;
    }

    float TractView::place (int point) const
    {
        return places[(size_t) point];
    }

    float TractView::width (int point) const
    {
        return widths[(size_t) point];
    }

    juce::Point<float> TractView::spineAt (float u, float lengthDrawn, juce::Point<float>* outward) const
    {
        const float throat = throatRun * lengthDrawn, mouth = mouthRun * lengthDrawn;
        const float arc = juce::MathConstants<float>::halfPi * bendRadius;
        const float d = juce::jlimit (0.0f, 1.0f, u) * (throat + arc + mouth);
        const float bendY = spineTop + bendRadius;

        if (d <= throat)
        {
            if (outward != nullptr) *outward = { -1.0f, 0.0f };
            return { spineX, bendY + throat - d };
        }

        if (d <= throat + arc)
        {
            const float t = (d - throat) / bendRadius, c = std::cos (t), s = std::sin (t);
            if (outward != nullptr) *outward = { -c, -s };
            return { spineX + bendRadius - bendRadius * c, bendY - bendRadius * s };
        }

        if (outward != nullptr) *outward = { 0.0f, -1.0f };
        return { spineX + bendRadius + (d - throat - arc), spineTop };
    }

    float TractView::radiusFor (float u, float logWidth) const noexcept
    {
        return tubeRadius * restingRadius (u) * std::exp (widthCompress * logWidth);
    }

    float TractView::innerLimit (float u, float lengthDrawn) const noexcept
    {
        // The wall on the inside of the bend must not fold over itself. It may grow again as it leaves the bend.
        const float throat = throatRun * lengthDrawn, mouth = mouthRun * lengthDrawn;
        const float arc = juce::MathConstants<float>::halfPi * bendRadius;
        const float d = u * (throat + arc + mouth);
        const float away = juce::jmax (0.0f, throat - d, d - throat - arc);
        return bendRadius - 6.0f + away * 0.85f;
    }

    float TractView::settingLogWidth (float u) const noexcept
    {
        // The same straight lines in log width between the points that the sound engine uses.
        int a = 0;
        while (a < numPoints - 2 && u > places[(size_t) a + 1]) ++a;
        const float x0 = places[(size_t) a], x1 = places[(size_t) a + 1];
        const float t = x1 > x0 ? juce::jlimit (0.0f, 1.0f, (u - x0) / (x1 - x0)) : 0.0f;
        const float w0 = std::log (widths[(size_t) a]), w1 = std::log (widths[(size_t) a + 1]);
        return w0 + t * (w1 - w0) + std::log (widthParam);
    }

    void TractView::placeTube (Tube& tube, float lengthDrawn) const
    {
        for (int i = 0; i < numSamples; ++i)
            tube.centre[(size_t) i] = spineAt ((float) i / (float) (numSamples - 1), lengthDrawn, &tube.outward[(size_t) i]);

        tube.lengthDrawn = lengthDrawn;
    }

    void TractView::readParameters()
    {
        const auto& state = processor.apvts;
        const auto read = [&state] (const char* id) { return state.getRawParameterValue (id)->load (std::memory_order_relaxed); };

        std::array<float, numPoints> w, x;
        for (int i = 0; i < numPoints; ++i)
        {
            w[(size_t) i] = read (widthIds[i]);
            x[(size_t) i] = placeIds[i] != nullptr ? read (placeIds[i]) * 0.01f : 0.0f;
        }

        const float l = read ("length"), wide = read ("width");
        const bool off = read ("bypass") > 0.5f;

        if (w == widths && x == places && l == lengthParam && wide == widthParam && off == bypassed && modelTube.lengthDrawn > 0.0f)
            return;

        widths = w;
        places = x;
        lengthParam = l;
        widthParam = wide;
        bypassed = off;

        for (int i = 0; i < numSamples; ++i)
            settingLog[(size_t) i] = settingLogWidth ((float) i / (float) (numSamples - 1));

        placeTube (modelTube, drawnLength (lengthParam));

        // What the engine will do with these formants, from the engine's own tube model.
        formant::TractShape shape;
        shape.length = lengthParam;
        shape.width = widthParam;
        shape.widths = widths;
        shape.places = places;
        formant::TractModel().buildMap (shape, mapBinHz, mapBins, modelMap.data());

        overlayStale = true;
    }

    float TractView::modelFormant (float hz) const noexcept
    {
        // The map says where each output frequency reads from, so a formant lands where the map reaches its frequency.
        if (hz <= 1.0f)
            return 0.0f;

        const auto begin = modelMap.begin(), end = modelMap.end();
        const auto above = std::lower_bound (begin, end, hz);

        if (above == begin)
            return 0.0f;
        if (above == end)
            return 0.0f;

        const auto k = (int) std::distance (begin, above);
        const float a = modelMap[(size_t) k - 1], b = modelMap[(size_t) k];
        return (float) (((double) k - 1.0 + (double) ((hz - a) / juce::jmax (1.0e-3f, b - a))) * mapBinHz);
    }

    void TractView::readAudio (double dt)
    {
        const auto& a = processor.analysis();
        const auto coefficient = [dt] (double seconds) { return (float) (1.0 - std::exp (-dt / seconds)); };

        levelDb = processor.outputLevel->getDb();
        voicing = a.voicing.load (std::memory_order_relaxed);
        const float rawPitch = a.pitchHz.load (std::memory_order_relaxed);
        const bool sounding = levelDb > -55.0f;
        const bool voiced = sounding && voicing > 0.5f;
        const float seconds = (float) dt;

        // The engine lists the peaks it finds, in order, and can miss any of them. Each is put in the slot its frequency belongs to,
        // so that a missed first formant does not turn the second one into F1.
        std::array<float, numFormants> slot {};
        if (voiced)
        {
            int next = 0;
            for (int i = 0; i < numFormants && next < numFormants; ++i)
            {
                const float hz = a.formantHz[(size_t) i].load (std::memory_order_relaxed);
                if (hz < formantLow[0])
                    continue;

                while (next < numFormants && hz > formantHigh[next])
                    ++next;

                if (next < numFormants && hz >= formantLow[next])
                    slot[(size_t) next++] = hz;
            }
        }

        const float formantSpeed = coefficient (0.09);
        for (size_t i = 0; i < numFormants; ++i)
        {
            formantAge[i] += seconds;
            if (slot[i] > 0.0f)
            {
                formants[i] = formants[i] > 1.0f && formantAge[i] < holdSeconds ? formants[i] * std::pow (slot[i] / formants[i], formantSpeed) : slot[i];
                formantAge[i] = 0.0f;
            }
            else if (formantAge[i] >= holdSeconds)
            {
                formants[i] = 0.0f;
            }
        }

        pitchAge += seconds;
        if (voiced && rawPitch > 1.0f)
        {
            pitch = pitch > 1.0f && pitchAge < holdSeconds ? pitch * std::pow (rawPitch / pitch, coefficient (0.12)) : rawPitch;
            pitchAge = 0.0f;
        }
        else if (pitchAge >= holdSeconds)
        {
            pitch = 0.0f;
        }

        // How the formants sit against a plain tube. The singer's size, the slow average of that, is taken out. What is left is the shape.
        float mean = 0.0f;
        int found = 0;
        for (int i = 0; i < numFormants; ++i)
        {
            if (formants[(size_t) i] > 1.0f)
            {
                mean += std::log (formants[(size_t) i] / plainTube[i]);
                ++found;
            }
        }

        std::array<float, numFormants> target {};
        const bool good = found >= 2;
        if (good)
        {
            sizeLog += (mean / (float) found - sizeLog) * coefficient (1.0);
            for (int i = 0; i < numFormants; ++i)
                if (formants[(size_t) i] > 1.0f)
                    target[(size_t) i] = juce::jlimit (-0.6f, 0.6f, std::log (formants[(size_t) i] / plainTube[i]) - sizeLog) * formantWeight[i];
        }

        const float shapeSpeed = coefficient (good ? 0.07 : 0.3);
        for (size_t i = 0; i < numFormants; ++i)
            shapeAmount[i] += (target[i] - shapeAmount[i]) * shapeSpeed;

        // The plot's top rises quickly to a louder voice and settles slowly, so a whole phrase keeps one scale.
        float loudest = -200.0f;
        std::array<float, numEnvelope> rawIn, rawOut;
        for (size_t i = 0; i < (size_t) numEnvelope; ++i)
        {
            const float in = a.inputDb[i].load (std::memory_order_relaxed), out = a.outputDb[i].load (std::memory_order_relaxed);
            rawIn[i] = std::isfinite (in) ? in : -200.0f;
            rawOut[i] = std::isfinite (out) ? out : -200.0f;
            loudest = juce::jmax (loudest, rawIn[i], rawOut[i]);
        }

        if (sounding)
            plotTop += (loudest + 5.0f - plotTop) * coefficient (loudest + 5.0f > plotTop ? 0.08 : 3.0);

        const float floorDb = plotTop - plotSpanDb;
        const float curveSpeed = coefficient (sounding ? 0.045 : 0.22);
        for (size_t i = 0; i < (size_t) numEnvelope; ++i)
        {
            inputDb[i] += ((sounding ? juce::jlimit (floorDb, plotTop, rawIn[i]) : floorDb) - inputDb[i]) * curveSpeed;
            outputDb[i] += ((sounding ? juce::jlimit (floorDb, plotTop, rawOut[i]) : floorDb) - outputDb[i]) * curveSpeed;
        }

        // The meter jumps up and falls back. The red warms with the level.
        levelShown = levelDb > levelShown ? levelDb : levelShown + (levelDb - levelShown) * coefficient (0.18);
        const float wanted = bypassed ? 0.0f : juce::jlimit (0.0f, 1.0f, (levelDb + 50.0f) / 34.0f);
        heat += (wanted - heat) * coefficient (0.12);
    }

    float TractView::xForHz (float hz) const noexcept
    {
        return plot.getX() + plot.getWidth() * std::log (hz / 60.0f) / std::log (16000.0f / 60.0f);
    }

    float TractView::yForDb (float db) const noexcept
    {
        return plot.getY() + plot.getHeight() * (plotTop - db) / plotSpanDb;
    }

    void TractView::buildShapes()
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto& basis = shapeBasis[(size_t) i];
            float live = 0.0f;
            for (size_t n = 0; n < numFormants; ++n)
                live -= shapeAmount[n] * basis[n];
            live *= liveDepth;

            const float u = (float) i / (float) (numSamples - 1);
            const float base = tubeRadius * restingRadius (u);
            singerRadius[(size_t) i] = base * std::exp (widthCompress * live);
            modelRadius[(size_t) i] = base * std::exp (widthCompress * (live + settingLog[(size_t) i]));
        }

        modelPath.clear();
        singerPath.clear();

        const auto wall = [] (juce::Path& path, const Tube& tube, const std::array<float, numSamples>& radius, float side, float grow, bool forwards, const auto& limit)
        {
            for (int k = 0; k < numSamples; ++k)
            {
                const auto i = (size_t) (forwards ? k : numSamples - 1 - k);
                const float r = juce::jmin (radius[i], limit (i)) + grow;
                const auto at = tube.centre[i] + tube.outward[i] * (side * r);

                if (k == 0 && forwards)
                    path.startNewSubPath (at);
                else
                    path.lineTo (at);
            }
        };

        const auto noLimit = [] (size_t) { return 1.0e6f; };
        const auto modelLimit = [this] (size_t i) { return innerLimit ((float) i / (float) (numSamples - 1), modelTube.lengthDrawn); };
        const auto singerLimit = [this] (size_t i) { return innerLimit ((float) i / (float) (numSamples - 1), 1.0f); };

        // The model is one flat shape.
        wall (modelPath, modelTube, modelRadius, 1.0f, 0.0f, true, noLimit);
        wall (modelPath, modelTube, modelRadius, -1.0f, 0.0f, false, modelLimit);
        modelPath.closeSubPath();

        // The singer is a thin line, made as a filled ribbon so that it costs no stroking. Closed at the cords, open at the lips.
        constexpr float half = 0.8f;
        wall (singerPath, singerTube, singerRadius, 1.0f, half, true, noLimit);
        wall (singerPath, singerTube, singerRadius, 1.0f, -half, false, noLimit);
        singerPath.closeSubPath();
        wall (singerPath, singerTube, singerRadius, -1.0f, half, true, singerLimit);
        wall (singerPath, singerTube, singerRadius, -1.0f, -half, false, singerLimit);
        singerPath.closeSubPath();

        const auto capOuter = singerTube.centre[0] + singerTube.outward[0] * singerRadius[0];
        const auto capInner = singerTube.centre[0] - singerTube.outward[0] * singerRadius[0];
        singerPath.addRectangle (juce::jmin (capOuter.x, capInner.x), capOuter.y - half, std::abs (capInner.x - capOuter.x), half * 2.0f);

        // The two formant curves, as ribbons too.
        const auto curve = [this] (juce::Path& path, const std::array<float, numEnvelope>& db)
        {
            path.clear();
            constexpr float h = 0.75f;
            std::array<juce::Point<float>, numEnvelope> at;
            for (int i = 0; i < numEnvelope; ++i)
                at[(size_t) i] = { plot.getX() + plot.getWidth() * (float) i / (float) (numEnvelope - 1), yForDb (db[(size_t) i]) };

            path.startNewSubPath (at[0].x, at[0].y - h);
            for (size_t i = 1; i < (size_t) numEnvelope; ++i)
                path.lineTo (at[i].x, at[i].y - h);
            for (size_t i = (size_t) numEnvelope; i-- > 0;)
                path.lineTo (at[i].x, at[i].y + h);
            path.closeSubPath();
        };

        curve (curvesIn, inputDb);
        curve (curvesOut, outputDb);
    }

    void TractView::advance (double dtSeconds)
    {
        readParameters();
        readAudio (dtSeconds);

        // The numbers are text, which is what costs the most. They change at most about six times a second.
        static constexpr double numbersEvery = 1.0 / 6.0;
        numbersWait -= dtSeconds;
        if (numbersWait <= 0.0)
        {
            numbersWait = numbersEvery;
            std::array<float, numFormants> moved;
            for (size_t i = 0; i < numFormants; ++i)
                moved[i] = modelFormant (formants[i]);
            numbers->update (pitch, pitch > 1.0f, formants, moved, bypassed);
        }

        buildShapes();

        // Skip the frame when nothing moved by more than a quarter of a pixel since the last one drawn.
        float moved = overlayStale || layersStale || ! anyDrawn ? 10.0f : 0.0f;
        for (size_t i = 0; i < (size_t) numSamples; ++i)
        {
            moved = juce::jmax (moved, std::abs (singerRadius[i] - drawnSinger[i]));
            moved = juce::jmax (moved, std::abs (modelRadius[i] - drawnModel[i]));
        }

        const float yScale = plot.getHeight() / plotSpanDb;
        for (size_t i = 0; i < (size_t) numEnvelope; ++i)
        {
            moved = juce::jmax (moved, std::abs (inputDb[i] - drawnIn[i]) * yScale);
            moved = juce::jmax (moved, std::abs (outputDb[i] - drawnOut[i]) * yScale);
        }

        const float meterAt = meter.getHeight() * juce::jlimit (0.0f, 1.0f, (levelShown - meterBottomDb) / (meterTopDb - meterBottomDb));
        moved = juce::jmax (moved, std::abs (meterAt - drawnMeter));
        moved = juce::jmax (moved, std::abs (heat - drawnHeat) * 40.0f);

        if (moved >= 0.25f)
        {
            drawnSinger = singerRadius;
            drawnModel = modelRadius;
            drawnIn = inputDb;
            drawnOut = outputDb;
            drawnMeter = meterAt;
            drawnHeat = heat;
            anyDrawn = true;
            repaint();
        }
    }

    void TractView::paint (juce::Graphics& g)
    {
        const double started = juce::Time::getMillisecondCounterHiRes();
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        buildLayers (scale);

        g.drawImageTransformed (staticLayer, juce::AffineTransform::scale (1.0f / layerScale));

        // Live: the model, the singer over it, the two curves and the meter. Everything else is cached.
        const auto red = bypassed ? tone::resting : tone::oxideDim.interpolatedWith (tone::oxide, heat);
        g.setColour (red);
        g.fillPath (modelPath);
        g.setColour (tone::text);
        g.fillPath (singerPath);
        g.setColour (tone::text.withAlpha (0.8f));
        g.fillPath (curvesIn);
        g.setColour (bypassed ? tone::faint : tone::oxideLine);
        g.fillPath (curvesOut);

        g.setColour (levelShown > 0.0f ? tone::oxideLine : tone::text);
        const float h = meter.getHeight() * juce::jlimit (0.0f, 1.0f, (levelShown - meterBottomDb) / (meterTopDb - meterBottomDb));
        g.fillRect (meter.getX(), meter.getBottom() - h, meter.getWidth(), h);

        g.drawImageTransformed (overlay, juce::AffineTransform::scale (1.0f / layerScale));

        ++paintCount;
        paintSeconds += (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    }

    void TractView::buildLayers (float scale)
    {
        const int w = juce::roundToInt ((float) getWidth() * scale), h = juce::roundToInt ((float) getHeight() * scale);

        if (staticLayer.getWidth() != w || staticLayer.getHeight() != h || layerScale != scale || layersStale)
        {
            layerScale = scale;
            layersStale = false;
            overlayStale = true;
            staticLayer = juce::Image (juce::Image::RGB, w, h, false);
            overlay = juce::Image (juce::Image::ARGB, w, h, true);
            juce::Graphics layer (staticLayer);
            layer.addTransform (juce::AffineTransform::scale (scale));
            paintStaticLayer (layer);
        }

        if (overlayStale)
        {
            overlayStale = false;
            overlay.clear (overlay.getBounds());
            juce::Graphics layer (overlay);
            layer.addTransform (juce::AffineTransform::scale (scale));
            paintOverlay (layer);
        }
    }

    void TractView::paintStaticLayer (juce::Graphics& g)
    {
        g.fillAll (tone::base);
        g.setColour (tone::well);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f);

        // The realistic range: where the tube would sit from 0.80 to 1.20, in width and in length. Everything outside the gray is a stretch.
        const auto band = [this] (float amount)
        {
            juce::Path p;
            constexpr int steps = 72;
            const float lengthDrawnHere = drawnLength (amount), widen = std::exp (widthCompress * std::log (amount));

            for (int i = 0; i <= steps; ++i)
            {
                juce::Point<float> out;
                const float u = (float) i / (float) steps;
                const auto at = spineAt (u, lengthDrawnHere, &out);
                const auto q = at + out * (tubeRadius * restingRadius (u) * widen);
                if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
            }

            for (int i = steps; i >= 0; --i)
            {
                juce::Point<float> out;
                const float u = (float) i / (float) steps;
                const auto at = spineAt (u, lengthDrawnHere, &out);
                p.lineTo (at - out * juce::jmin (tubeRadius * restingRadius (u) * widen, innerLimit (u, lengthDrawnHere)));
            }

            p.closeSubPath();
            return p;
        };

        g.setColour (tone::range);
        g.fillPath (band (1.2f));
        g.setColour (tone::well);
        g.fillPath (band (0.8f));

        // The formant plot's scale.
        g.setFont (font (Weight::regular, 12.0f));
        const struct { float hz; const char* text; } ticks[] = { { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" }, { 1000.0f, "1k" }, { 2000.0f, "2k" }, { 5000.0f, "5k" }, { 10000.0f, "10k" } };

        for (const auto& t : ticks)
        {
            const float x = xForHz (t.hz);
            g.setColour (tone::text.withAlpha (0.05f));
            g.fillRect (x - 0.5f, plot.getY(), 1.0f, plot.getHeight());
            g.setColour (tone::faint);
            g.drawText (t.text, juce::Rectangle<float> (x - 24.0f, plot.getBottom() + 5.0f, 48.0f, 16.0f), juce::Justification::centred, false);
        }

        // The meter's track and name, level with the mouth.
        g.setColour (tone::text.withAlpha (0.1f));
        g.fillRect (meter);
        g.setColour (tone::faint);
        g.drawText ("Out", juce::Rectangle<float> (meter.getRight() - 40.0f, meter.getBottom() + 6.0f, 40.0f, 16.0f), juce::Justification::centredRight, false);
    }

    void TractView::paintOverlay (juce::Graphics& g)
    {
        const float lengthDrawnNow = drawnLength (lengthParam);

        // A word on the gray, which turns into a warning when the shape leaves it.
        bool outside = lengthParam < 0.8f || lengthParam > 1.2f || widthParam < 0.8f || widthParam > 1.2f;
        for (auto w : widths)
            outside = outside || w < 0.8f || w > 1.2f;

        const juce::Rectangle<float> note (446.0f, 22.0f, 256.0f, 16.0f);
        g.setFont (font (Weight::regular, 12.0f));
        g.setColour (outside ? tone::oxideLine : tone::faint);
        g.drawText (outside ? "Past the realistic range" : "Gray is the realistic range", note, juce::Justification::centredRight, false);

        // One rib across the tube at each point, with its handle on the outer wall and its name past the inner wall.
        for (int i = 0; i < numPoints; ++i)
        {
            const float u = place (i);
            juce::Point<float> out;
            const auto centre = spineAt (u, lengthDrawnNow, &out);
            const float r = radiusFor (u, std::log (width (i)) + std::log (widthParam));
            const float inner = juce::jmin (r, innerLimit (u, lengthDrawnNow));
            const auto handle = handlePosition (i);
            const auto far = centre - out * inner;
            const bool on = i == lit || i == hover || i == dragging;

            g.setColour (tone::text.withAlpha (on ? 0.9f : 0.42f));
            g.drawLine ({ handle, far }, on ? 1.4f : 1.0f);

            const float dot = on ? 8.5f : 6.5f;
            g.setColour (on ? tone::text : tone::well);
            g.fillEllipse (handle.x - dot, handle.y - dot, dot * 2.0f, dot * 2.0f);
            g.setColour (tone::text);
            g.drawEllipse (handle.x - dot, handle.y - dot, dot * 2.0f, dot * 2.0f, 1.8f);

            // The name sits clear of both shapes: past the inner wall, or past the edge of the gray, whichever is further.
            const auto towards = -out;
            const float clear = juce::jmax (inner, juce::jmin (radiusFor (u, std::log (1.2f)), innerLimit (u, lengthDrawnNow))) + 22.0f;
            const auto spot = centre + towards * clear;
            g.setFont (font (on ? Weight::medium : Weight::regular, 13.0f));
            g.setColour (on ? tone::text : tone::muted);

            juce::Rectangle<float> box (70.0f, 18.0f);
            if (towards.y > 0.8f)        box = box.withCentre ({ spot.x, spot.y + 7.0f });
            else if (towards.x > 0.8f)   box = box.withPosition (spot.x, spot.y - 9.0f);
            else                         box = box.withPosition (spot.x + 14.0f, spot.y - 10.0f);   // On the bend, up and clear of the throat's name.

            // Roof and Lips can come close enough to touch. Lips steps down.
            if (i == numPoints - 1 && std::abs (spineAt (place (3), lengthDrawnNow).x - centre.x) < 52.0f)
                box = box.translated (0.0f, 16.0f);

            const auto justify = towards.y > 0.8f ? juce::Justification::centred : juce::Justification::centredLeft;
            g.drawText (pointName (i), box, justify, false);
        }
    }

    juce::Point<float> TractView::handlePosition (int point) const
    {
        const float u = place (point);
        juce::Point<float> out;
        const auto centre = spineAt (u, drawnLength (lengthParam), &out);
        const float r = radiusFor (u, std::log (width (point)) + std::log (widthParam));
        const auto at = centre + out * r;
        // A handle can always be reached, however extreme the setting.
        return { juce::jlimit (12.0f, viewW - 12.0f, at.x), juce::jlimit (12.0f, viewH - 12.0f, at.y) };
    }

    int TractView::pointAt (juce::Point<float> p) const
    {
        int best = -1;
        float nearest = 18.0f;

        for (int i = 0; i < numPoints; ++i)
        {
            const float d = p.getDistanceFrom (handlePosition (i));
            if (d < nearest)
            {
                nearest = d;
                best = i;
            }
        }

        return best;
    }

    void TractView::setHover (int point)
    {
        if (point == hover)
            return;

        hover = point;
        overlayStale = true;
        repaint();
        setMouseCursor (point >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);

        if (onPointHovered != nullptr)
            onPointHovered (point);
    }

    void TractView::setHighlight (int point)
    {
        if (point != lit)
        {
            lit = point;
            overlayStale = true;
            repaint();
        }
    }

    void TractView::mouseMove (const juce::MouseEvent& e)
    {
        setHover (pointAt (e.position));
    }

    void TractView::mouseExit (const juce::MouseEvent&)
    {
        if (dragging < 0)
            setHover (-1);
    }

    void TractView::mouseDown (const juce::MouseEvent& e)
    {
        dragging = pointAt (e.position);

        if (dragging < 0)
            return;

        setHover (dragging);
        dragTarget = handlePosition (dragging);
        dragLast = e.position;

        processor.apvts.getParameter (widthIds[dragging])->beginChangeGesture();
        if (placeIds[dragging] != nullptr)
            processor.apvts.getParameter (placeIds[dragging])->beginChangeGesture();

        overlayStale = true;
        repaint();
    }

    void TractView::mouseDrag (const juce::MouseEvent& e)
    {
        if (dragging < 0)
            return;

        // Shift held gives a five times finer drag.
        dragTarget += (e.position - dragLast) * (e.mods.isShiftDown() ? 0.2f : 1.0f);
        dragLast = e.position;
        dragPointTo (dragTarget);
    }

    void TractView::mouseUp (const juce::MouseEvent& e)
    {
        if (dragging < 0)
            return;

        processor.apvts.getParameter (widthIds[dragging])->endChangeGesture();
        if (placeIds[dragging] != nullptr)
            processor.apvts.getParameter (placeIds[dragging])->endChangeGesture();

        dragging = -1;
        overlayStale = true;
        setHover (pointAt (e.position));
        repaint();
    }

    void TractView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const int point = pointAt (e.position);

        if (point < 0)
            return;

        for (const char* id : { widthIds[point], placeIds[point] })
        {
            if (id != nullptr)
            {
                auto* p = processor.apvts.getParameter (id);
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->getDefaultValue());
                p->endChangeGesture();
            }
        }
    }

    void TractView::dragPointTo (juce::Point<float> target)
    {
        // The handle follows the mouse: along the tube is the point's place, and out from the middle of the tube is its width.
        const float lengthDrawnNow = drawnLength (lengthParam);
        const float low = placeLow[dragging], high = placeHigh[dragging];
        float bestU = low, bestDistance = 1.0e9f;
        constexpr int steps = 96;

        for (int i = 0; i <= steps; ++i)
        {
            const float u = low + (high - low) * (float) i / (float) steps;
            const float d = target.getDistanceSquaredFrom (spineAt (u, lengthDrawnNow));
            if (d < bestDistance)
            {
                bestDistance = d;
                bestU = u;
            }
        }

        juce::Point<float> out;
        const auto centre = spineAt (bestU, lengthDrawnNow, &out);
        const float reach = juce::jmax (1.0f, (target - centre).getDotProduct (out));
        const float logTotal = std::log (reach / (tubeRadius * restingRadius (bestU))) / widthCompress;

        setReal (*processor.apvts.getParameter (widthIds[dragging]), std::exp (logTotal - std::log (widthParam)));

        if (placeIds[dragging] != nullptr)
            setReal (*processor.apvts.getParameter (placeIds[dragging]), bestU * 100.0f);
    }
}
