#include "TapeDisplay.h"

namespace ui
{
    namespace
    {
        constexpr float lowHz = 20.0f, highHz = 20000.0f;
        constexpr float topDb = 8.0f, bottomDb = -26.0f;           // the response scale
        constexpr float levelTop = -14.0f, levelBottom = -94.0f;  // the spectrum scale, per bin with a 3 dB per octave tilt
        constexpr float cornerRadius = 12.0f;

        juce::String minusSign() { return juce::String::fromUTF8 ("\xe2\x88\x92"); }

        juce::String decibelLabel (int db)
        {
            return db > 0 ? "+" + juce::String (db) : (db < 0 ? minusSign() + juce::String (-db) : juce::String ("0"));
        }

        // Rounds a panel's corners from the outside, over whatever is under them, with one flat colour.
        void coverCorners (juce::Graphics& g, juce::Rectangle<float> b)
        {
            juce::Path outside;
            outside.setUsingNonZeroWinding (false);
            outside.addRectangle (b);
            outside.addRoundedRectangle (b, cornerRadius);
            g.setColour (tone::base);
            g.fillPath (outside);
        }
    }

    // ---- response ---------------------------------------------------------------------------------------------

    float ResponseView::columnHz (int column) noexcept
    {
        return lowHz * std::pow (highHz / lowHz, ((float) column + 0.5f) / (float) columns);
    }

    ResponseView::ResponseView()
    {
        hint = "Cream goes on the tape, amber comes off. The line is the tone of the tape. Rust is what Age has taken.";
        setOpaque (true);
        setInterceptsMouseClicks (true, false);
        inDb.fill (-200.0f);
        outDb.fill (-200.0f);
        shownIn.fill (-200.0f);
        shownOut.fill (-200.0f);
    }

    juce::Rectangle<float> ResponseView::plot() const
    {
        return getLocalBounds().toFloat().withTrimmedRight (34.0f).withTrimmedBottom (24.0f);
    }

    float ResponseView::xFor (float hz) const
    {
        const auto p = plot();
        return p.getX() + p.getWidth() * std::log (hz / lowHz) / std::log (highHz / lowHz);
    }

    float ResponseView::yForResponse (float db) const
    {
        const auto p = plot();
        return p.getY() + p.getHeight() * (topDb - db) / (topDb - bottomDb);
    }

    float ResponseView::yForLevel (float db) const
    {
        const auto p = plot();
        return p.getY() + p.getHeight() * (levelTop - db) / (levelTop - levelBottom);
    }

    void ResponseView::resized()
    {
        layersValid = false;
        pathsValid = false;
    }

    void ResponseView::setSettings (const tapecolor::display::Settings& s, bool isBypassed)
    {
        settings = s;
        bypassed = isBypassed;
        layersValid = false;
        repaint();
    }

    bool ResponseView::setSpectra (const Column& in, const Column& out)
    {
        inDb = in;
        outDb = out;

        // Skip the repaint when nothing moved by more than a quarter of a pixel, counting small changes until they add up.
        const float pixelsPerDb = plot().getHeight() / (levelTop - levelBottom);
        const float limit = 0.25f / juce::jmax (0.01f, pixelsPerDb);
        bool moved = false;
        for (int i = 0; i < columns && ! moved; ++i)
            moved = std::abs (in[(size_t) i] - shownIn[(size_t) i]) > limit || std::abs (out[(size_t) i] - shownOut[(size_t) i]) > limit;

        if (moved)
        {
            shownIn = in;
            shownOut = out;
            pathsValid = false;
            repaint (plot().getSmallestIntegerContainer());
        }

        return moved;
    }

    void ResponseView::buildSpectrumPaths()
    {
        pathsValid = true;
        const auto p = plot();
        const float floorY = p.getBottom();
        const float columnWidth = p.getWidth() / (float) columns;

        const auto build = [&] (juce::Path& path, const Column& values)
        {
            path.clear();
            path.preallocateSpace (columns * 3 + 12);
            path.startNewSubPath (p.getX(), floorY);
            float lastY = -1000.0f;
            for (int i = 0; i < columns; ++i)
            {
                const float y = juce::jlimit (p.getY(), floorY, yForLevel (values[(size_t) i]));
                // Points on a nearly straight run are skipped: edges are what cost.
                if (i == 0 || i == columns - 1 || std::abs (y - lastY) > 0.5f)
                {
                    path.lineTo (p.getX() + ((float) i + 0.5f) * columnWidth, y);
                    lastY = y;
                }
            }
            path.lineTo (p.getRight(), floorY);
            path.closeSubPath();
        };

        build (inPath, shownIn);
        build (outPath, shownOut);
    }

    void ResponseView::rebuild (float scale)
    {
        using namespace tapecolor::display;
        layersValid = true;
        layerScale = scale;

        const auto bounds = getLocalBounds().toFloat();
        const auto p = plot();
        const int w = juce::jmax (1, juce::roundToInt (bounds.getWidth() * scale)), h = juce::jmax (1, juce::roundToInt (bounds.getHeight() * scale));

        // Under the sound: the panel, the grid and the scales.
        back = juce::Image (juce::Image::RGB, w, h, true);
        {
            juce::Graphics g (back);
            g.addTransform (juce::AffineTransform::scale (scale));
            g.fillAll (tone::base);
            g.setColour (tone::well);
            g.fillRoundedRectangle (bounds, cornerRadius);

            g.setColour (tone::text.withAlpha (0.055f));
            for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
                g.fillRect (juce::Rectangle<float> (xFor (hz) - 0.5f, p.getY(), 1.0f, p.getHeight()));
            for (int db : { 6, -6, -12, -18, -24 })
                g.fillRect (juce::Rectangle<float> (p.getX(), yForResponse ((float) db) - 0.5f, p.getWidth(), 1.0f));

            g.setFont (font (Weight::regular, 12.0f, true));
            g.setColour (tone::faint);
            const auto labelY = p.getBottom() + 4.0f;
            for (auto [hz, text] : std::initializer_list<std::pair<float, const char*>> { { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" }, { 1000.0f, "1k" }, { 2000.0f, "2k" }, { 5000.0f, "5k" }, { 10000.0f, "10k" } })
                g.drawText (text, juce::Rectangle<float> (xFor (hz) - 24.0f, labelY, 48.0f, 16.0f), juce::Justification::centred, false);
            for (int db : { 6, 0, -6, -12, -18, -24 })
                g.drawText (decibelLabel (db), juce::Rectangle<float> (p.getRight() + 2.0f, yForResponse ((float) db) - 8.0f, 28.0f, 16.0f), juce::Justification::centredRight, false);
        }

        // Over the sound: the response, what Age took, the legend and the panel's corners.
        over = juce::Image (juce::Image::ARGB, w, h, true);
        {
            juce::Graphics g (over);
            g.addTransform (juce::AffineTransform::scale (scale));

            auto fresh = settings;
            fresh.age = 0.0;
            const float reference = (float) responseDb (settings, 1000.0), freshReference = (float) responseDb (fresh, 1000.0);

            juce::Path line, ghost, lost;
            const int steps = juce::jmax (64, juce::roundToInt (p.getWidth() / 3.0f));
            std::vector<juce::Point<float>> linePoints ((size_t) steps + 1), ghostPoints ((size_t) steps + 1);

            for (int i = 0; i <= steps; ++i)
            {
                const float x = p.getX() + p.getWidth() * (float) i / (float) steps;
                const float hz = lowHz * std::pow (highHz / lowHz, (float) i / (float) steps);
                const float now = bypassed ? 0.0f : (float) responseDb (settings, hz) - reference;
                const float was = bypassed ? 0.0f : (float) responseDb (fresh, hz) - freshReference;
                // Past the floor is off the chart, not along it: the plot clips these.
                linePoints[(size_t) i] = { x, juce::jlimit (p.getY() - 40.0f, p.getBottom() + 40.0f, yForResponse (now)) };
                ghostPoints[(size_t) i] = { x, juce::jlimit (p.getY() - 40.0f, p.getBottom() + 40.0f, yForResponse (was)) };
            }

            line.startNewSubPath (linePoints.front());
            ghost.startNewSubPath (ghostPoints.front());
            lost.startNewSubPath (ghostPoints.front());
            for (size_t i = 1; i < linePoints.size(); ++i)
            {
                line.lineTo (linePoints[i]);
                ghost.lineTo (ghostPoints[i]);
                lost.lineTo (ghostPoints[i]);
            }
            for (size_t i = linePoints.size(); i-- > 0;)
                lost.lineTo (linePoints[i]);
            lost.closeSubPath();

            const bool aged = settings.age > 0.0005 && ! bypassed;
            {
                juce::Graphics::ScopedSaveState clip (g);
                g.reduceClipRegion (p.getSmallestIntegerContainer());
                if (aged)
                {
                    g.setColour (tone::rust.withAlpha (0.62f));
                    g.fillPath (lost);
                    g.setColour (tone::text.withAlpha (0.4f));
                    g.strokePath (ghost, juce::PathStrokeType (1.0f));
                }

                g.setColour (tone::text.withAlpha (bypassed ? 0.35f : 1.0f));
                g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // The legend, top left, in the plot's own colours.
            auto legend = juce::Rectangle<float> (p.getX() + 14.0f, p.getY() + 11.0f, 300.0f, 16.0f);
            const auto key = [&] (const char* text, juce::Colour colour, juce::Colour ink)
            {
                g.setColour (colour);
                g.fillRoundedRectangle (legend.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 10.0f), 2.5f);
                const auto f = font (Weight::regular, 12.5f);
                g.setFont (f);
                g.setColour (ink);
                const float width = juce::GlyphArrangement::getStringWidth (f, text) + 2.0f;
                g.drawText (text, legend.withTrimmedLeft (6.0f).withWidth (width), juce::Justification::centredLeft, false);
                legend.removeFromLeft (width + 20.0f);
            };
            key ("In", tone::text.withAlpha (0.5f), tone::muted);
            key ("Out", tone::amber, tone::muted);
            if (aged)
                key ("Lost to Age", tone::rust, tone::muted);

            if (bypassed)
            {
                g.setColour (tone::text);
                g.setFont (font (Weight::medium, 15.0f));
                g.drawText ("Bypassed", p.withTrimmedTop (34.0f).withHeight (20.0f).withTrimmedLeft (14.0f), juce::Justification::centredLeft, false);
            }

            coverCorners (g, bounds);
        }
    }

    void ResponseView::paint (juce::Graphics& g)
    {
        ++paintCount;
        const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (! layersValid || std::abs (scale - layerScale) > 0.001f)
            rebuild (scale);
        if (! pathsValid)
            buildSpectrumPaths();

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        const auto toPoints = juce::AffineTransform::scale (1.0f / scale);
        g.setColour (juce::Colours::black);
        g.drawImageTransformed (back, toPoints);

        g.setColour (tone::text.withAlpha (0.15f));
        g.fillPath (inPath);
        g.setColour (tone::amber.withAlpha (bypassed ? 0.3f : 0.6f));
        g.fillPath (outPath);

        g.setColour (juce::Colours::black);
        g.drawImageTransformed (over, toPoints);
    }

    // ---- the curve --------------------------------------------------------------------------------------------

    CurveView::CurveView()
    {
        hint = "How the tape bends the level. Amber is the part of the curve the sound is using. The number is how far its loudest moment is held down.";
        setOpaque (true);
        setInterceptsMouseClicks (true, false);
    }

    juce::Rectangle<float> CurveView::plot() const
    {
        auto b = getLocalBounds().toFloat().reduced (14.0f).withTrimmedTop (48.0f);
        const float side = juce::jmin (b.getWidth(), b.getHeight());
        return b.withSizeKeepingCentre (side, side);
    }

    void CurveView::resized()
    {
        layersValid = false;
    }

    void CurveView::setSettings (const tapecolor::display::Settings& s, bool isBypassed)
    {
        settings = s;
        bypassed = isBypassed;
        layersValid = false;
        shownPeak = -1.0f;
        shownSquash = -1.0f;
        reading = {};
        repaint();
    }

    bool CurveView::setPeak (float linearPeak)
    {
        peak = juce::jlimit (0.0f, 1.0f, linearPeak);
        const float pixelsPerUnit = plot().getWidth() * 0.5f;
        bool moved = false;

        if (std::abs (peak - shownPeak) * pixelsPerUnit > 0.25f)
        {
            shownPeak = peak;
            repaint (litArea());
            moved = true;
        }

        return moved;
    }

    bool CurveView::updateReading()
    {
        squashDb = bypassed ? 0.0f : (float) tapecolor::display::Curve (settings).squashDb ((double) peak);

        // Only its own corner repaints.
        if (std::abs (squashDb - shownSquash) >= 0.1f)
        {
            shownSquash = squashDb;
            squashText = juce::String (squashDb, 1) + " dB";
            reading = {};
            repaint (getLocalBounds().withHeight (56));
            return true;
        }

        return false;
    }

    juce::Rectangle<int> CurveView::litArea() const
    {
        return plot().getSmallestIntegerContainer().expanded (8);
    }

    void CurveView::rebuild (float scale)
    {
        layersValid = true;
        layerScale = scale;

        const auto bounds = getLocalBounds().toFloat();
        const auto p = plot();
        const int w = juce::jmax (1, juce::roundToInt (bounds.getWidth() * scale)), h = juce::jmax (1, juce::roundToInt (bounds.getHeight() * scale));

        const tapecolor::display::Curve curve (settings);
        constexpr float range = 1.12f;   // a little above the most the tape can hold
        const auto at = [&] (double in, double out)
        {
            return juce::Point<float> (p.getCentreX() + (float) in * p.getWidth() * 0.5f,
                                       juce::jlimit (p.getY() - 60.0f, p.getBottom() + 60.0f, p.getCentreY() - (float) out / range * p.getHeight() * 0.5f));
        };

        juce::Path line, straight, gap;
        constexpr int steps = 96;
        std::vector<juce::Point<float>> linePoints, straightPoints;
        for (int i = 0; i <= steps; ++i)
        {
            const double in = -1.0 + 2.0 * (double) i / steps;
            const auto low = bypassed ? in * 0.8 : curve (in);
            linePoints.push_back (at (in, low));
            straightPoints.push_back (at (in, bypassed ? in * 0.8 : curve.straight (in)));
        }
        line.startNewSubPath (linePoints.front());
        straight.startNewSubPath (straightPoints.front());
        gap.startNewSubPath (linePoints.front());
        for (size_t i = 1; i < linePoints.size(); ++i)
        {
            line.lineTo (linePoints[i]);
            straight.lineTo (straightPoints[i]);
            gap.lineTo (linePoints[i]);
        }
        for (size_t i = straightPoints.size(); i-- > 0;)
            gap.lineTo (straightPoints[i]);
        gap.closeSubPath();

        back = juce::Image (juce::Image::RGB, w, h, true);
        {
            juce::Graphics g (back);
            g.addTransform (juce::AffineTransform::scale (scale));
            g.fillAll (tone::base);
            g.setColour (tone::well);
            g.fillRoundedRectangle (bounds, cornerRadius);

            g.setColour (tone::text.withAlpha (0.055f));
            g.fillRect (juce::Rectangle<float> (p.getCentreX() - 0.5f, p.getY(), 1.0f, p.getHeight()));
            g.fillRect (juce::Rectangle<float> (p.getX(), p.getCentreY() - 0.5f, p.getWidth(), 1.0f));

            // How full the tape can get, top and bottom.
            const float full = at (0.0, 1.0).y, fullBelow = at (0.0, -1.0).y;
            g.setColour (tone::text.withAlpha (0.13f));
            g.fillRect (juce::Rectangle<float> (p.getX(), full - 0.5f, p.getWidth(), 1.0f));
            g.fillRect (juce::Rectangle<float> (p.getX(), fullBelow - 0.5f, p.getWidth(), 1.0f));

            {
                juce::Graphics::ScopedSaveState clip (g);
                g.reduceClipRegion (p.getSmallestIntegerContainer());
                g.setColour (tone::text.withAlpha (0.28f));
                g.strokePath (straight, juce::PathStrokeType (1.0f));
                g.setColour (tone::text.withAlpha (0.5f));
                g.strokePath (line, juce::PathStrokeType (1.6f));
            }

            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText ("Saturation", bounds.reduced (14.0f, 0.0f).withTrimmedTop (9.0f).withHeight (17.0f), juce::Justification::centredLeft, false);
        }

        lit = juce::Image (juce::Image::ARGB, w, h, true);
        {
            juce::Graphics g (lit);
            g.addTransform (juce::AffineTransform::scale (scale));
            g.reduceClipRegion (p.getSmallestIntegerContainer());
            g.setColour (tone::amber.withAlpha (0.3f));
            g.fillPath (gap);
            g.setColour (tone::amber);
            g.strokePath (line, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    void CurveView::paint (juce::Graphics& g)
    {
        ++paintCount;
        const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (! layersValid || std::abs (scale - layerScale) > 0.001f)
            rebuild (scale);

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        const auto toPoints = juce::AffineTransform::scale (1.0f / scale);
        g.setColour (juce::Colours::black);
        g.drawImageTransformed (back, toPoints);

        const auto p = plot();
        const float reach = peak * p.getWidth() * 0.5f;

        if (reach > 0.25f)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (juce::Rectangle<float> (p.getCentreX() - reach, p.getY() - 4.0f, reach * 2.0f, p.getHeight() + 8.0f).getSmallestIntegerContainer());
            g.drawImageTransformed (lit, toPoints);
        }

        // The reel's repaints can stretch the area being drawn over this, so the number is drawn once and kept.
        const auto readingArea = getLocalBounds().reduced (14, 0).withTrimmedTop (26).withHeight (28).withWidth (140);
        if (g.clipRegionIntersects (readingArea))
        {
            if (! reading.isValid() || reading.getWidth() != juce::roundToInt ((float) readingArea.getWidth() * scale))
            {
                reading = juce::Image (juce::Image::RGB, juce::roundToInt ((float) readingArea.getWidth() * scale), juce::roundToInt ((float) readingArea.getHeight() * scale), true);
                juce::Graphics r (reading);
                r.addTransform (juce::AffineTransform::scale (scale));
                r.fillAll (tone::well);
                r.setColour (tone::amber.withAlpha (bypassed ? 0.4f : 1.0f));
                r.setFont (font (Weight::medium, 24.0f, true));
                r.drawText (squashText.isNotEmpty() ? squashText : juce::String ("0.0 dB"), readingArea.withZeroOrigin(), juce::Justification::centredLeft, false);
            }

            g.setColour (juce::Colours::black);
            g.drawImageTransformed (reading, juce::AffineTransform::scale (1.0f / scale).translated ((float) readingArea.getX(), (float) readingArea.getY()));
        }
    }

    // ---- the reel ---------------------------------------------------------------------------------------------

    Reel::Reel()
    {
        hint = "Turns at the tape speed and shakes with the read head, like the real thing. It stops when nothing is playing.";
        setOpaque (true);
        setInterceptsMouseClicks (true, false);
        rebuildPaths();
    }

    void Reel::resized()
    {
        rebuildPaths();
    }

    void Reel::setAge (float ageZeroToOne)
    {
        if (std::abs (age - ageZeroToOne) > 0.002f)
        {
            age = ageZeroToOne;
            rebuildPaths();
            repaint();
        }
    }

    // The hub, in the coordinates of the paint. Up to Age 55 it is an open reel with round windows, and past that a cassette's toothed hub.
    // The tape wound on it shrinks all the way.
    void Reel::rebuildPaths()
    {
        const float size = (float) juce::jmin (getWidth(), getHeight()) * 0.5f;
        if (size <= 0.0f)
            return;

        packRadius = juce::jmap (age, size, size * 0.70f);
        packColour = age < 0.5f ? tone::text.interpolatedWith (tone::amber, age * 2.0f)
                                : tone::amber.interpolatedWith (tone::rust, (age - 0.5f) * 2.0f);

        const float hubRadius = juce::jmap (age, size * 0.62f, size * 0.40f);
        const bool cassette = age >= 0.55f;   // the same place the word changes from reel to cassette

        hub.clear();
        hub.setUsingNonZeroWinding (false);
        hub.addEllipse (-hubRadius, -hubRadius, hubRadius * 2.0f, hubRadius * 2.0f);

        if (! cassette)
        {
            // Five round windows and a pin: a reel, and not a warning sign.
            const float ring = hubRadius * 0.58f, hole = hubRadius * 0.2f;
            for (int i = 0; i < 5; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 5.0f;
                hub.addEllipse (std::sin (a) * ring - hole, -std::cos (a) * ring - hole, hole * 2.0f, hole * 2.0f);
            }
            const float pin = hubRadius * 0.11f;
            hub.addEllipse (-pin, -pin, pin * 2.0f, pin * 2.0f);
        }

        else
        {
            // A cassette hub: a round hole with six short teeth pointing in.
            const float hole = hubRadius * 0.56f, inner = hole * 0.7f;
            juce::Path holePath;
            const float twoPi = juce::MathConstants<float>::twoPi;
            const auto point = [] (float a, float r) { return juce::Point<float> (std::sin (a) * r, -std::cos (a) * r); };
            for (int i = 0; i < 6; ++i)
            {
                const float a = twoPi * (float) i / 6.0f;
                const float wide = 0.16f, narrow = 0.17f * hole / inner * 0.8f;
                if (i == 0)
                    holePath.startNewSubPath (point (a - wide, hole));
                else
                    holePath.lineTo (point (a - wide, hole));
                holePath.lineTo (point (a - narrow, inner));
                holePath.lineTo (point (a + narrow, inner));
                holePath.lineTo (point (a + wide, hole));
                for (int step = 1; step < 4; ++step)
                    holePath.lineTo (point (a + wide + (twoPi / 6.0f - 2.0f * wide) * (float) step / 4.0f, hole));
            }
            holePath.closeSubPath();
            hub.addPath (holePath);
        }
    }

    bool Reel::advance (double seconds, double turnsPerSecond, double headMs, bool running)
    {
        // Eases to the speed and to a stop, like a motor with some weight on it.
        const double target = running ? turnsPerSecond : 0.0;
        speed += (target - speed) * juce::jmin (1.0, seconds * 3.5);
        if (std::abs (speed) < 0.002 && ! running)
            speed = 0.0;
        angle += speed * seconds * juce::MathConstants<double>::twoPi;
        if (angle > juce::MathConstants<double>::twoPi)
            angle -= juce::MathConstants<double>::twoPi;

        // The read head's real movement in ms. Sped up for the eye: 1 ms is about 12 degrees.
        const double shown = angle + (speed != 0.0 ? headMs * 0.21 : 0.0);
        if (std::abs (shown - shownAngle) * (double) getWidth() * 0.25 > 0.25)
        {
            shownAngle = shown;
            repaint();
        }

        return speed != 0.0;
    }

    void Reel::paint (juce::Graphics& g)
    {
        g.fillAll (tone::base);
        const auto centre = getLocalBounds().toFloat().getCentre();

        // The tape wound on it is a flat disc. The hub is cut out of the same flat shape and turns.
        g.setColour (packColour);
        g.fillEllipse (juce::Rectangle<float> (packRadius * 2.0f, packRadius * 2.0f).withCentre (centre));

        // The hub is darker than the tape it carries.
        g.setColour (tone::menu);
        g.fillPath (hub, juce::AffineTransform::rotation ((float) shownAngle).translated (centre));
    }
}
