#include "StageView.h"

namespace ui
{
    namespace
    {
        constexpr float seatsHeight = 46.0f;      // the audience, under the stage
        constexpr float apronBelowLead = 84.0f;   // floor in front of the lead
        constexpr float backMargin = 30.0f;       // floor behind the last line
        constexpr float bowHeight = 44.0f;        // how far the rows curve back at the sides
        constexpr float cornerCut = 34.0f;

        constexpr float shoulderWidth = 60.0f, shoulderDepth = 19.0f, headSize = 27.0f, micReach = 30.0f;
        constexpr float leadScale = 1.15f;
        constexpr float spriteBox = 112.0f;       // room for the largest singer at any angle
        constexpr float angleStepSize = 0.07f;    // about 4 degrees a step
        constexpr int maxAngleStep = 7;
        inline int angleIndex (float angle) { return juce::jlimit (0, 2 * maxAngleStep, juce::roundToInt (angle / angleStepSize) + maxAngleStep); }
        inline float poolRadius (float level) { return 26.0f + 48.0f * std::sqrt (juce::jmin (1.0f, level)); }

        // Ms behind the lead, squeezed so a few ms of drift reads clearly near the lead while a 100 ms slapback still fits on the stage.
        constexpr double squeezeMs = 30.0, furthestMs = 160.0;
        inline double squeezed (double ms)
        {
            const double m = juce::jlimit (0.0, furthestMs, ms);
            return (m / (m + squeezeMs)) / (furthestMs / (furthestMs + squeezeMs));
        }

        juce::Image textTag (const juce::String& text, juce::Font f, juce::Colour colour, juce::Rectangle<int> size, float scale)
        {
            juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) size.getWidth() * scale), juce::roundToInt ((float) size.getHeight() * scale), true);
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (scale));
            g.setFont (f);
            g.setColour (colour);
            g.drawText (text, size.toFloat(), juce::Justification::centred, false);
            return image;
        }
    }

    StageView::StageView (PluginProcessor& p)
        : processor (p),
          widthAttachment (*p.apvts.getParameter ("width"), [] (float) {}, nullptr),
          looseAttachment (*p.apvts.getParameter ("tightness"), [] (float) {}, nullptr),
          leadPanAttachment (*p.apvts.getParameter ("leadpan"), [] (float) {}, nullptr)
    {
        setOpaque (true);

        // A singer from above, built once: shoulders, and one bent arm holding the mic.
        bodyPath.addRoundedRectangle (-shoulderWidth * 0.5f, -shoulderDepth * 0.5f, shoulderWidth, shoulderDepth, shoulderDepth * 0.5f);
        juce::Path arm, stroked;
        arm.startNewSubPath (shoulderWidth * 0.5f - 5.0f, 2.0f);
        arm.lineTo (shoulderWidth * 0.5f - 3.0f, 17.0f);
        arm.lineTo (3.0f, micReach - 1.0f);
        juce::PathStrokeType (6.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (stroked, arm);
        bodyPath.addPath (stroked);
        bodyPath.setUsingNonZeroWinding (true);

        setComponentID ("stage");
        setTitle ("Stage");
        updateHint();
    }

    float StageView::depth() const noexcept
    {
        return (float) getHeight() - seatsHeight - apronBelowLead - backMargin - bowHeight;
    }

    float StageView::frontY (float x) const noexcept
    {
        // The front of the stage bows out toward the audience, so the rows are arcs and the lead stands at the front of the curve.
        const float across = (x - centreX()) / juce::jmax (1.0f, (float) getWidth() * 0.5f);
        return (float) getHeight() - seatsHeight - apronBelowLead - bowHeight * across * across;
    }

    float StageView::yForMs (double ms, float x) const noexcept
    {
        return frontY (x) - depth() * (float) squeezed (ms);
    }

    void StageView::resized()
    {
        floorImage = {};
        zonesDirty = true;
        std::fill (settingsSeen.begin(), settingsSeen.end(), std::numeric_limits<float>::quiet_NaN());
        settled = false;

        // On first layout, singers start on their marks.
        const bool first = singers[0].markX == 0.0f && singers[0].x == 0.0f;
        readSettings();

        if (first)
            for (auto& s : singers)
            {
                s.x = s.markX;
                s.y = s.markY;
            }
    }

    void StageView::readSettings()
    {
        auto& state = processor.apvts;
        const auto get = [&state] (const char* id) { return state.getRawParameterValue (id)->load (std::memory_order_relaxed); };

        const std::array<float, 16> now { get ("doubles"), get ("tightness"), get ("width"), get ("lead"), get ("double1"), get ("double2"), get ("double3"), get ("timingamount"),
                                          get ("pitchamount"), get ("vibrato"), get ("timbre"), get ("leadpan"), get ("doublespan"), get ("extradelay"), get ("bypass"),
                                          (float) processor.getMainBusNumOutputChannels() };

        if (now == settingsSeen)
            return;

        settingsSeen = now;
        settled = false;

        doublesCount = juce::jlimit (1, (int) DuoEngine::maxDoubles, juce::roundToInt (now[0]));
        const float loose = now[1], width = now[2], leadPan = now[11], doublesPan = now[12], extra = now[13], timing = now[7], pitch = now[8], vibrato = now[9], timbre = now[10];
        bypassed = now[14] >= 0.5f;
        stereo = processor.getMainBusNumOutputChannels() > 1;
        const float widthShare = stereo ? width * 0.01f : 0.0f, panShare = stereo ? doublesPan * 0.01f : 0.0f;

        for (int v = 0; v < DuoEngine::maxDoubles; ++v)
        {
            auto& s = singers[(size_t) v];
            s.onStage = v < doublesCount;
            const float pan = juce::jlimit (-1.0f, 1.0f, DuoEngine::panAtFullWidth[doublesCount - 1][v] * widthShare + panShare);
            s.markX = xForPan (pan);
            const double rest = DuoEngine::restDelayMs (v, loose, timing, pitch, extra);
            const double range = DuoEngine::driftRangeMs (loose, timing, pitch, vibrato);
            s.markY = yForMs (rest, s.markX);
            s.zoneHalf = 0.5f * (yForMs (rest - range, s.markX) - yForMs (rest + range, s.markX));
            const float db = now[(size_t) (4 + v)];
            s.off = db <= DuoEngine::offDb;
            s.gain = (s.off || bypassed) ? 0.0f : DuoEngine::levelToGain (db);
            s.splitReach = spanX() * widthShare * DuoEngine::splitAtFullWidth[doublesCount - 1][v];
            // A higher throat reads as a smaller singer.
            const float size = 1.0f - 0.16f * timbre * 0.01f * (float) (DuoEngine::timbreSemitones[(size_t) v] / 3.0);

            if (size != s.size)
            {
                s.size = size;
                for (auto& sprite : sprites[(size_t) v])
                    sprite = {};
            }
        }

        auto& lead = singers[leadIndex];
        lead.onStage = true;
        lead.markX = xForPan (stereo ? leadPan * 0.01f : 0.0f);
        lead.markY = yForMs (0.0, lead.markX);
        lead.off = now[3] <= DuoEngine::offDb && ! bypassed;
        lead.gain = bypassed ? 1.0f : (lead.off ? 0.0f : DuoEngine::levelToGain (now[3]));

        if (lead.size != leadScale)
        {
            lead.size = leadScale;
            for (auto& sprite : sprites[(size_t) leadIndex])
                sprite = {};
        }

        zonesDirty = true;
        repaint();
    }

    double StageView::restMs (int v) const
    {
        const auto get = [this] (const char* id) { return (double) processor.apvts.getRawParameterValue (id)->load (std::memory_order_relaxed); };
        return DuoEngine::restDelayMs (v, get ("tightness"), get ("timingamount"), get ("pitchamount"), get ("extradelay"));
    }

    void StageView::advance (double dtSeconds)
    {
        const float dt = (float) juce::jlimit (0.001, 0.25, dtSeconds);
        readSettings();

        const float peak = processor.takeInputPeak();
        silentFor = peak > 0.0003f ? 0.0 : silentFor + dtSeconds;
        const bool wasLive = live;
        live = silentFor < 0.35;

        const float moveCoef = 1.0f - std::exp (-dt / 0.045f);
        const float restCoef = 1.0f - std::exp (-dt / 0.14f);
        const float fall = std::exp (-dt / 0.16f);
        bool anythingMoved = wasLive != live;
        juce::Rectangle<int> dirty;

        for (int i = 0; i < (int) singers.size(); ++i)
        {
            auto& s = singers[(size_t) i];
            const bool isLead = i == leadIndex;
            const float oldX = s.x, oldY = s.y, oldLevel = s.level, oldAngle = s.angle;
            s.x += (s.markX - s.x) * moveCoef;

            // The doubles' depth is read from the sound. With nothing playing there is nothing to hear drift, so they walk back to their marks.
            float targetY = s.markY, targetAngle = 0.0f;

            if (! isLead && live)
            {
                const double ms = (double) processor.getDelayMs (i);
                targetY = yForMs (ms, s.x);
                // A moving delay is a pitch change: 1 ms per second is 1.73 cents. The shoulders turn with it, up to about a quarter turn at the loosest.
                const double cents = wasLive ? -(ms - s.lastMs) / juce::jmax (0.005, dtSeconds) * 1.7312 : 0.0;
                targetAngle = (float) juce::jlimit (-0.49, 0.49, cents * 0.022);
                s.lastMs = ms;
            }

            s.y += (targetY - s.y) * (live ? moveCoef : restCoef);
            s.angle += (targetAngle - s.angle) * restCoef;

            const float heard = peak * s.gain;
            s.level = juce::jmax (heard, s.level * fall);
            if (s.level < 0.002f)
                s.level = 0.0f;

            if (! s.onStage)
                continue;

            // Still easing toward somewhere, even if it is too little to draw yet.
            if (std::abs (s.x - oldX) > 0.01f || std::abs (s.y - oldY) > 0.01f || std::abs (s.level - oldLevel) > 0.0005f || std::abs (s.angle - oldAngle) > 0.001f)
                anythingMoved = true;

            // Only draw once it has moved by a quarter of a pixel, counting small changes until they add up.
            const float pool = s.level > 0.0f ? poolRadius (s.level) : 0.0f;

            if (std::abs (s.x - s.drawnX) > 0.25f || std::abs (s.y - s.drawnY) > 0.25f || std::abs (pool - s.drawnPool) > 0.25f || angleIndex (s.angle) != angleIndex (s.drawnAngle))
            {
                s.drawnX = s.x;
                s.drawnY = s.y;
                s.drawnPool = pool;
                s.drawnAngle = s.angle;
                const auto oldBounds = s.drawnBounds;
                s.drawnBounds = boundsOf (s, isLead);
                dirty = dirty.getUnion (oldBounds.getUnion (s.drawnBounds).getSmallestIntegerContainer());
            }
        }

        if (! dirty.isEmpty())
            repaint (dirty);

        // Nothing moved by a visible amount: draw nothing until something does.
        settled = ! anythingMoved;
    }

    juce::Rectangle<float> StageView::boundsOf (const Singer& s, bool isLead) const
    {
        const float pool = poolRadius (s.level);
        const float halfWidth = juce::jmax (pool, s.splitReach + 22.0f, shoulderWidth * 0.7f * s.size);
        const float halfHeight = juce::jmax (pool, (micReach + 6.0f) * s.size);
        auto r = juce::Rectangle<float> (s.x - halfWidth, s.y - halfHeight, halfWidth * 2.0f, halfHeight * 2.0f).expanded (3.0f);

        if (isLead)
            r = r.getUnion ({ s.x - 30.0f, s.y, 60.0f, 70.0f });

        return r;
    }

    // ---- the cached layers ---------------------------------------------------------------------------------------

    void StageView::rebuildFloor (float scale)
    {
        // One pixel over, so a size that lands between pixels never leaves a seam along the edge.
        const int w = (int) std::ceil ((float) getWidth() * scale) + 1, h = (int) std::ceil ((float) getHeight() * scale) + 1;
        floorImage = juce::Image (juce::Image::RGB, w, h, false);
        juce::Graphics g (floorImage);
        g.addTransform (juce::AffineTransform::scale (scale));

        const float width = (float) getWidth(), height = (float) getHeight();
        g.fillAll (tone::base);

        // The floor plan: a flat back wall with cut corners, a front that bows out to the audience.
        const float leadY = height - seatsHeight - apronBelowLead;
        const float apronEdge = leadY + apronBelowLead - 16.0f;
        juce::Path floor;
        floor.startNewSubPath (cornerCut, 0.0f);
        floor.lineTo (width - cornerCut, 0.0f);
        floor.lineTo (width, cornerCut);
        floor.lineTo (width, apronEdge - bowHeight);
        floor.quadraticTo (width * 0.5f, apronEdge + bowHeight, 0.0f, apronEdge - bowHeight);
        floor.lineTo (0.0f, cornerCut);
        floor.closeSubPath();
        g.setColour (tone::floor);
        g.fillPath (floor);

        // The rows, as arcs. The label sits at the right end of each.
        const auto rowPath = [&] (double ms)
        {
            juce::Path p;
            const float y = (float) squeezed (ms) * depth();
            bool started = false;
            for (float x = 14.0f; x <= width - 56.0f; x += 8.0f)
            {
                const float py = frontY (x) - y;
                if (! started) p.startNewSubPath (x, py); else p.lineTo (x, py);
                started = true;
            }
            return p;
        };

        for (const double ms : { 20.0, 40.0, 80.0, 150.0 })
        {
            g.setColour (tone::ring);
            g.strokePath (rowPath (ms), juce::PathStrokeType (1.0f));
            g.setColour (tone::faint);
            g.setFont (font (Weight::regular, 12.0f));
            const float ly = frontY (width - 56.0f) - (float) squeezed (ms) * depth();
            g.drawText (ms == 20.0 ? "20 ms" : juce::String ((int) ms) + " ms", juce::Rectangle<float> (width - 54.0f, ly - 9.0f, 50.0f, 18.0f), juce::Justification::centredLeft, false);
        }

        // The lead's line: on time.
        g.setColour (tone::ring.brighter (0.25f));
        g.strokePath (rowPath (0.0), juce::PathStrokeType (1.0f));
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 12.0f));
        g.drawText ("On time", juce::Rectangle<float> (width - 54.0f, frontY (width - 56.0f) - 9.0f, 52.0f, 18.0f), juce::Justification::centredLeft, false);

        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText ("Left", juce::Rectangle<float> (18.0f, frontY (18.0f) + 22.0f, 60.0f, 18.0f), juce::Justification::centredLeft, false);
        g.drawText ("Right", juce::Rectangle<float> (width - 78.0f, frontY (width - 18.0f) + 22.0f, 60.0f, 18.0f), juce::Justification::centredRight, false);
        g.setColour (tone::faint);
        g.setFont (font (Weight::regular, 12.5f));
        g.drawText ("Further back is later", juce::Rectangle<float> (0.0f, 10.0f, width, 18.0f), juce::Justification::centred, false);

        // The seats: rows of small round dots seen from above, following the front of the stage.
        g.setColour (tone::lift);
        for (int row = 0; row < 2; ++row)
        {
            const float rowY = apronEdge + 16.0f + (float) row * 14.0f;
            for (float x = width * 0.5f - (row == 0 ? 0.0f : 11.0f); x > 0.0f; x -= 22.0f)
            {
                const float across = (x - width * 0.5f) / (width * 0.5f);
                const float y = rowY - bowHeight * across * across;
                g.fillEllipse (x - 3.0f, y - 3.0f, 6.0f, 6.0f);
                g.fillEllipse (width - x - 3.0f, y - 3.0f, 6.0f, 6.0f);
            }
        }

        // Things that are only drawn once, and then moved around.
        for (auto& row : sprites)
            for (auto& sprite : row)
                sprite = {};

        leadTag = textTag ("Lead", font (Weight::medium, 13.0f), tone::text, { 0, 0, 60, 18 }, scale);
        layerScale = scale;
        zonesDirty = true;
    }

    void StageView::rebuildZones (float scale)
    {
        zonesImage = floorImage.createCopy();
        juce::Graphics g (zonesImage);
        g.addTransform (juce::AffineTransform::scale (scale));

        for (int v = 0; v < DuoEngine::maxDoubles; ++v)
        {
            const auto& s = singers[(size_t) v];
            if (! s.onStage)
                continue;

            // The lane it can wander in. Loose makes it long, Tight makes it short.
            const float halfWidth = 40.0f, halfHeight = s.zoneHalf + 40.0f;
            const juce::Rectangle<float> zone (s.markX - halfWidth, s.markY - halfHeight, halfWidth * 2.0f, halfHeight * 2.0f);
            g.setColour (tone::sky.withAlpha (s.off ? 0.02f : 0.05f));
            g.fillRoundedRectangle (zone, halfWidth);
            g.setColour (tone::sky.withAlpha (s.off ? 0.15f : 0.42f));
            g.drawRoundedRectangle (zone, halfWidth, 1.25f);

            // A tape cross on the floor where its path is centred.
            g.setColour (tone::sky.withAlpha (s.off ? 0.25f : 0.8f));
            g.fillRect (juce::Rectangle<float> (s.markX - 39.0f, s.markY - 0.75f, 7.0f, 1.5f));
            g.fillRect (juce::Rectangle<float> (s.markX + 32.0f, s.markY - 0.75f, 7.0f, 1.5f));
        }

        // A mono track has no left or right to stand in.
        if (! stereo)
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText ("Mono track: everyone sings from the middle", juce::Rectangle<float> (0.0f, 30.0f, (float) getWidth(), 18.0f), juce::Justification::centred, false);
        }

        zonesDirty = false;
    }

    // ---- a frame -------------------------------------------------------------------------------------------------

    void StageView::paint (juce::Graphics& g)
    {
        ++paintCount;
        const float scale = juce::jmax (0.5f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (floorImage.isNull() || std::abs (scale - layerScale) > 0.01f)
            rebuildFloor (scale);
        if (zonesDirty || zonesImage.isNull())
            rebuildZones (scale);

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (zonesImage, juce::AffineTransform::scale (1.0f / scale));

        // Back of the stage first, so the nearer singer stands in front.
        std::array<int, DuoEngine::maxDoubles + 1> order { 0, 1, 2, leadIndex };
        std::sort (order.begin(), order.end(), [this] (int a, int b) { return singers[(size_t) a].y < singers[(size_t) b].y; });

        for (const int i : order)
            if (singers[(size_t) i].onStage)
                drawSinger (g, singers[(size_t) i], i);
    }

    void StageView::drawSinger (juce::Graphics& g, const Singer& s, int index) const
    {
        const bool isLead = index == leadIndex;
        const bool lit = hovered == index || dragged == index;
        const float presence = s.off || (bypassed && ! isLead) ? 0.38f : 1.0f;

        // A pool of light, as big as what this singer is singing right now. No singing, no light.
        if (s.level > 0.0f)
        {
            const float r = poolRadius (s.level);
            g.setColour (isLead ? tone::text.withAlpha (0.15f) : (s.splitReach > r ? tone::pool.withAlpha (0.2f) : tone::pool.withAlpha (0.3f)));
            g.fillEllipse (s.x - juce::jmax (r, s.splitReach), s.y - r, juce::jmax (r, s.splitReach) * 2.0f, r * 2.0f);
        }
        else if (s.splitReach > 1.0f)
        {
            // Spread to both sides even at rest, so Width shows on a lone double with nothing playing.
            g.setColour (tone::sky.withAlpha (0.1f));
            g.fillEllipse (s.x - s.splitReach - 22.0f, s.y - 22.0f, (s.splitReach + 22.0f) * 2.0f, 44.0f);
        }

        // The figure is a picture drawn once for each angle it can turn to. Paths and text cost far more per frame than moving an image.
        auto& sprite = sprites[(size_t) index][(size_t) angleIndex (s.angle)];

        if (sprite.isNull())
            sprite = makeSprite (index, angleIndex (s.angle), s.size);

        const float pixel = 1.0f / layerScale, grow = lit ? 1.07f : 1.0f;
        g.setOpacity (presence);
        g.drawImageTransformed (sprite, juce::AffineTransform::scale (pixel).translated (-spriteBox * 0.5f, -spriteBox * 0.5f).scaled (grow).translated (s.x, s.y));

        if (isLead)
            g.drawImageTransformed (leadTag, juce::AffineTransform::scale (pixel).translated (s.x - 30.0f, s.y + (micReach + 6.0f) * s.size * grow));
    }

    juce::Image StageView::makeSprite (int index, int angleStep, float size) const
    {
        const bool isLead = index == leadIndex;
        const int pixels = juce::roundToInt (spriteBox * layerScale);
        juce::Image image (juce::Image::ARGB, pixels, pixels, true);
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (layerScale).translated (0.0f, 0.0f));
        g.addTransform (juce::AffineTransform::translation (spriteBox * 0.5f, spriteBox * 0.5f));

        const float angle = (float) (angleStep - maxAngleStep) * angleStepSize;
        const float micX = -std::sin (angle) * micReach * size, micY = std::cos (angle) * micReach * size, micR = 4.4f * size, head = headSize * size;

        // The body from above, turning with the pitch: shoulders and an arm holding a mic out toward the audience, then the head on top.
        g.setColour (isLead ? tone::leadDeep : tone::skyDeep);
        g.fillPath (bodyPath, juce::AffineTransform::scale (size).rotated (angle));
        g.setColour (isLead ? tone::text : tone::skyPale);
        g.fillEllipse (micX - micR, micY - micR, micR * 2.0f, micR * 2.0f);
        g.fillEllipse (-head * 0.5f, -head * 0.5f, head, head);

        if (! isLead)
        {
            g.setColour (tone::ink);
            g.setFont (font (Weight::medium, 14.0f * juce::jmax (0.9f, size)));
            g.drawText (juce::String (index + 1), juce::Rectangle<float> (-12.0f, -12.0f, 24.0f, 24.0f), juce::Justification::centred, false);
        }

        return image;
    }

    // ---- moving singers with the mouse ---------------------------------------------------------------------------

    int StageView::singerAtPoint (juce::Point<float> p) const
    {
        int best = -1;
        float bestDistance = 40.0f;

        for (int i = 0; i < (int) singers.size(); ++i)
        {
            const auto& s = singers[(size_t) i];
            if (! s.onStage)
                continue;

            const float distance = p.getDistanceFrom ({ s.x, s.y });

            if (distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
        }

        return best;
    }

    void StageView::updateHint()
    {
        const int who = dragged >= 0 ? dragged : hovered;

        if (who == leadIndex)
            hint = "You. Drag sideways to move your voice left or right. Double-click for the middle.";
        else if (who >= 0 && singers[(size_t) who].splitReach > 0.5f)
            hint = "Double " + juce::String (who + 1) + " has no partner, so Width spreads it to both sides. Drag back or forward for Tightness.";
        else if (who >= 0)
            hint = "Double " + juce::String (who + 1) + ". Drag sideways for Width, back or forward for Tightness. Double-click to reset both.";
        else
            hint = "The stage from above. Further back is later behind you. Each lane shows how far that double can drift.";
    }

    void StageView::mouseMove (const juce::MouseEvent& e)
    {
        const int now = singerAtPoint (e.position);

        if (now != hovered)
        {
            hovered = now;
            updateHint();
            setMouseCursor (now >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();

        }
    }

    void StageView::mouseExit (const juce::MouseEvent&)
    {
        if (hovered != -1)
        {
            hovered = -1;
            updateHint();
            repaint();
        }
    }

    void StageView::mouseDown (const juce::MouseEvent& e)
    {
        dragged = singerAtPoint (e.position);
        dragAxis = Axis::undecided;
        widthAtStart = processor.apvts.getRawParameterValue ("width")->load (std::memory_order_relaxed);
        looseAtStart = processor.apvts.getRawParameterValue ("tightness")->load (std::memory_order_relaxed);
        panAtStart = processor.apvts.getRawParameterValue ("leadpan")->load (std::memory_order_relaxed);
        updateHint();
        repaint();
    }

    void StageView::mouseDrag (const juce::MouseEvent& e)
    {
        if (dragged < 0)
            return;

        const float dx = (float) e.getDistanceFromDragStartX(), dy = (float) e.getDistanceFromDragStartY();

        // The lead only goes sideways.
        if (dragged == leadIndex)
        {
            if (! panGesture && std::abs (dx) > 2.0f)
            {
                leadPanAttachment.beginGesture();
                panGesture = true;
            }

            if (panGesture)
                leadPanAttachment.setValueAsPartOfGesture (juce::jlimit (-100.0f, 100.0f, panAtStart + dx / spanX() * 100.0f));
        }
        else
        {
            // A double: whichever way the drag starts decides what it changes, so a sideways drag cannot nudge Tightness by accident.
            if (dragAxis == Axis::undecided && e.getDistanceFromDragStart() > 5)
            {
                dragAxis = std::abs (dx) >= std::abs (dy) ? Axis::across : Axis::along;

                if (dragAxis == Axis::across)
                {
                    widthAttachment.beginGesture();
                    widthGesture = true;
                }
                else
                {
                    looseAttachment.beginGesture();
                    looseGesture = true;
                }
            }

            if (widthGesture)
            {
                const float pan = DuoEngine::panAtFullWidth[doublesCount - 1][dragged];
                // On a double that is in the middle, or with the pair together at Width 0, wider is whichever way the drag goes.
                const float side = pan < 0.0f && widthAtStart > 2.0f ? -1.0f : (pan > 0.0f && widthAtStart > 2.0f ? 1.0f : (dx < 0.0f ? -1.0f : 1.0f));
                widthAttachment.setValueAsPartOfGesture (juce::jlimit (0.0f, 100.0f, widthAtStart + side * dx / spanX() * 100.0f));
            }
            else if (looseGesture)
            {
                // Back is looser. The whole stage depth is the whole range.
                looseAttachment.setValueAsPartOfGesture (juce::jlimit (0.0f, 100.0f, looseAtStart - dy / juce::jmax (40.0f, depth() * 0.6f) * 100.0f));
            }
        }

        if (onActivity != nullptr)
            onActivity();
    }

    void StageView::mouseUp (const juce::MouseEvent&)
    {
        if (widthGesture) widthAttachment.endGesture();
        if (looseGesture) looseAttachment.endGesture();
        if (panGesture) leadPanAttachment.endGesture();
        widthGesture = looseGesture = panGesture = false;
        dragged = -1;
        updateHint();
        repaint();
    }

    void StageView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const int who = singerAtPoint (e.position);
        const auto reset = [this] (juce::ParameterAttachment& attachment, const char* id)
        {
            auto* p = processor.apvts.getParameter (id);
            attachment.setValueAsCompleteGesture (p->convertFrom0to1 (p->getDefaultValue()));
        };

        if (who == leadIndex)
        {
            reset (leadPanAttachment, "leadpan");
        }
        else if (who >= 0)
        {
            reset (widthAttachment, "width");
            reset (looseAttachment, "tightness");
        }

        if (onActivity != nullptr)
            onActivity();
    }
}
