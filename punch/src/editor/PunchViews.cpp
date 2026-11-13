#include "PunchViews.h"

using namespace ui;

namespace punchview
{
    namespace
    {
        // The timeline is short, so it spends its height on the levels speech and music actually use.
        constexpr float timelineFloorDb = -42.0f;
        float timelineY (float level)  { return (topDb - juce::jlimit (timelineFloorDb, topDb, toDb (level))) / (topDb - timelineFloorDb); }

        constexpr int captionHeight = 24, timeHeight = 22, gutterWidth = 36, columnStep = 3;

        // Drops points that sit within a fraction of a pixel of the straight line past them. Fewer edges is what makes a filled shape cheap.
        void thinPoints (std::vector<juce::Point<float>>& points)
        {
            constexpr float tolerance = 0.25f;
            constexpr size_t longestRun = 8;   // checks only look one point ahead, so a gentle curve would otherwise drift into long flat facets
            if (points.size() < 3)
                return;

            size_t kept = 0, run = 0;
            for (size_t i = 1; i + 1 < points.size(); ++i)
            {
                const auto a = points[kept], b = points[i], c = points[i + 1];
                const float along = (b.x - a.x) / juce::jmax (0.001f, c.x - a.x);
                if (std::abs (a.y + (c.y - a.y) * along - b.y) > tolerance || ++run >= longestRun)
                {
                    points[++kept] = b;
                    run = 0;
                }
            }
            points[++kept] = points.back();
            points.resize (kept + 1);
        }
        const juce::String minusSign = juce::String::fromUTF8 ("\xe2\x88\x92");

        void drawLayer (juce::Graphics& g, const juce::Image& layer, float scale)
        {
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.setColour (juce::Colours::black);
            g.drawImageTransformed (layer, juce::AffineTransform::scale (1.0f / scale));
        }

        juce::Image makeLayer (int width, int height, float scale)
        {
            return juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) width * scale)), juce::jmax (1, juce::roundToInt ((float) height * scale)), false);
        }
    }

    juce::String minusText (float value, int places)
    {
        const auto digits = juce::String (std::abs (value), places);
        return (value < 0.0f && digits.containsAnyOf ("123456789") ? minusSign : juce::String()) + digits;
    }

    // ---- the last hit ---------------------------------------------------------------------------------------------

    HitView::HitView (PluginProcessor& p)
        : processor (p),
          ceilingAttachment (*p.apvts.getParameter ("ceiling"), [] (float) {}, nullptr),
          restingHint ("White is the hit as it came in. Orange is how it goes out.")
    {
        setComponentID ("hitView");
        setOpaque (true);
        hint = restingHint;
        fillExampleHit (exampleBefore.data(), hitBins);
    }

    bool HitView::feed (float before, float after)
    {
        const bool isNew = tracker.feed (before, after);

        if (isNew)
        {
            newHit = true;
            settingsAtHit = settings;
        }

        return isNew;
    }

    void HitView::chooseWhatToDraw (const Settings& now, bool quiet)
    {
        Showing want = Showing::example;

        if (tracker.hasHit())
            want = quiet && now != settingsAtHit ? Showing::preview : Showing::real;

        if (want != showing)
        {
            showing = want;
            staticDirty = true;
            curvesDirty = movedNow = true;
            repaint();
        }

        if (want == Showing::real)
        {
            auto& hit = tracker.latest();
            if (hit.length != drawLength || drawBefore != hit.before.data() || drawAfter != hit.after.data() || newHit)
            {
                hit.smooth();
                drawBefore = hit.before.data();
                drawAfter = hit.after.data();
                drawLength = hit.length;
                curvesDirty = movedNow = true;
            }
            return;
        }

        if (want == Showing::preview)
            tracker.latest().smooth();

        const float* source = want == Showing::example ? exampleBefore.data() : tracker.latest().before.data();
        const int length = want == Showing::example ? hitBins : tracker.latest().length;

        if (! previewValid || previewedWith != now || drawBefore != source || drawAfter != previewAfter.data() || drawLength != length)
        {
            preview.run (source, length, now, previewAfter.data());
            previewedWith = now;
            previewValid = true;
            drawBefore = source;
            drawAfter = previewAfter.data();
            drawLength = length;
            curvesDirty = movedNow = true;
        }
    }

    bool HitView::update (const Settings& now, bool quiet)
    {
        if (now.ceiling != settings.ceiling || now.ceilingOn != settings.ceilingOn)
        {
            staticDirty = true;
            repaint();
        }
        settings = now;

        if (newHit)
            ghostDirty = movedNow = true;

        chooseWhatToDraw (now, quiet);
        newHit = false;

        // The dirty flags wait for the next paint, which may not come while the window is hidden. Only what changed on this call counts as movement.
        if (movedNow)
            repaint (plot);
        const bool moved = movedNow;
        movedNow = false;
        return moved;
    }

    float HitView::valueAtColumn (const float* values, int column, int length) const noexcept
    {
        const float pos = columnBin[(size_t) column];
        const float next = column + 1 < (int) columnBin.size() ? columnBin[(size_t) column + 1] : pos;

        // The levels are already smoothed over several bins, so nothing is lost between bins this close, and it stays free of steps.
        if (next - pos <= 5.0f)
        {
            const int i = juce::jlimit (0, length - 1, (int) pos);
            const int j = juce::jmin (length - 1, i + 1);
            const float f = pos - (float) i;
            return values[i] + (values[j] - values[i]) * juce::jlimit (0.0f, 1.0f, f);
        }

        float highest = 0.0f;
        for (int i = (int) pos, last = juce::jmin (length - 1, (int) next); i <= last; ++i)
            highest = juce::jmax (highest, values[i]);
        return highest;
    }

    void HitView::rebuildGhost()
    {
        ghostPath.clear();
        ghostDirty = false;

        auto& old = tracker.lastOne();
        if (old.length < 2 || columnBin.empty())
            return;

        old.smooth();
        const float left = (float) plot.getX(), width = (float) plot.getWidth(), bottom = (float) plot.getBottom(), height = (float) plot.getHeight();
        const int columns = (int) columnBin.size();
        ghostPath.startNewSubPath (left, bottom);
        float lastX = left;

        for (int c = 0; c < columns && columnBin[(size_t) c] <= (float) (old.length - 1); ++c)
        {
            lastX = left + width * (float) c / (float) (columns - 1);
            ghostPath.lineTo (lastX, (float) plot.getY() + height * yForDb (toDb (valueAtColumn (old.after.data(), c, old.length))));
        }

        ghostPath.lineTo (lastX, bottom);
        ghostPath.closeSubPath();
    }

    void HitView::rebuildCurves()
    {
        curvesDirty = false;
        emberPath.clear();
        hotPath.clear();
        linePath.clear();
        linePoints.clear();
        cutsAnything = false;

        if (drawLength < 2 || columnBin.empty())
            return;

        const float left = (float) plot.getX(), top = (float) plot.getY(), width = (float) plot.getWidth(), height = (float) plot.getHeight(), bottom = (float) plot.getBottom();
        const int columns = (int) columnBin.size();
        float lastX = left;

        afterPoints.clear();

        for (int c = 0; c < columns && columnBin[(size_t) c] <= (float) (drawLength - 1); ++c)
        {
            const float x = left + width * (float) c / (float) (columns - 1);
            const float yBefore = top + height * yForDb (toDb (valueAtColumn (drawBefore, c, drawLength)));
            const float yAfter = top + height * yForDb (toDb (valueAtColumn (drawAfter, c, drawLength)));
            cutsAnything = cutsAnything || yAfter > yBefore + 0.5f;
            linePoints.push_back ({ x, yBefore });
            afterPoints.push_back ({ x, yAfter });
            lastX = x;
        }

        thinPoints (linePoints);
        thinPoints (afterPoints);

        const auto fillUnder = [&] (juce::Path& path, const std::vector<juce::Point<float>>& points)
        {
            path.startNewSubPath (left, bottom);
            for (const auto& point : points)
                path.lineTo (point);
            path.lineTo (lastX, bottom);
            path.closeSubPath();
        };
        fillUnder (hotPath, afterPoints);
        if (cutsAnything)
            fillUnder (emberPath, linePoints);

        // The white line as a thin filled shape, worked out here. Filling this is much cheaper per frame than stroking a path.
        const auto count = linePoints.size();
        if (count < 2)
            return;

        constexpr float halfWidth = 0.8f;
        const auto normalAt = [&] (size_t i)
        {
            const auto a = linePoints[i > 0 ? i - 1 : 0], b = linePoints[juce::jmin (i + 1, count - 1)];
            const auto d = b - a;
            const float length = juce::jmax (0.001f, std::hypot (d.x, d.y));
            return juce::Point<float> (-d.y / length * halfWidth, d.x / length * halfWidth);
        };

        for (size_t i = 0; i < count; ++i)
        {
            const auto p = linePoints[i] - normalAt (i);
            if (i == 0) linePath.startNewSubPath (p); else linePath.lineTo (p);
        }
        for (size_t i = count; i-- > 0;)
            linePath.lineTo (linePoints[i] + normalAt (i));
        linePath.closeSubPath();
    }

    void HitView::resized()
    {
        plot = getLocalBounds().withTrimmedTop (captionHeight).withTrimmedBottom (timeHeight).withTrimmedRight (gutterWidth);

        const int columns = juce::jmax (2, plot.getWidth() / columnStep);
        columnBin.resize ((size_t) columns);
        for (int c = 0; c < columns; ++c)
            columnBin[(size_t) c] = (float) (secondsForX ((float) c / (float) (columns - 1)) / binSeconds);

        staticDirty = true;
        curvesDirty = true;
        ghostDirty = true;
    }

    int HitView::getSplitX() const noexcept
    {
        return plot.getX() + juce::roundToInt ((float) plot.getWidth() * xForSeconds (0.03));
    }

    juce::String HitView::captionText() const { return captionFor (showing); }

    juce::String HitView::captionFor (Showing what)
    {
        switch (what)
        {
            // Short enough to sit beside the legend at the smallest window (0.6.2: the longer ones were cut off there).
            case Showing::example: return "Example hit. Play something to see your own.";
            case Showing::preview: return "Last hit, redrawn. Play to see it for real.";
            case Showing::real:    break;
        }
        return "Last hit";
    }

    float HitView::ceilingLineY() const noexcept
    {
        return (float) plot.getY() + (float) plot.getHeight() * yForDb (settings.ceiling);
    }

    void HitView::rebuildStatic (float scale)
    {
        staticLayer = makeLayer (getWidth(), getHeight(), scale);
        staticScale = scale;
        staticDirty = false;

        juce::Graphics g (staticLayer);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.fillAll (tone::black);

        const float left = (float) plot.getX(), width = (float) plot.getWidth(), top = (float) plot.getY(), height = (float) plot.getHeight();
        const float splitX = left + width * xForSeconds (0.03), tailX = left + width * xForSeconds (0.015);

        if (zone == Zone::start)
        {
            g.setColour (tone::lift);
            g.fillRect (juce::Rectangle<float> (left, top, splitX - left, height));
        }
        else if (zone == Zone::tail)
        {
            g.setColour (tone::lift);
            g.fillRect (juce::Rectangle<float> (tailX, top, left + width - tailX, height));
        }

        // Level, on the same scale as the In and Out meters beside it.
        g.setFont (font (Weight::regular, 11.0f, true));
        const auto gutter = juce::Rectangle<float> ((float) plot.getRight(), top, (float) gutterWidth, height);
        const float ceilingY = ceilingLineY();

        for (int db = 0; db >= -36; db -= 12)
        {
            const float y = top + height * yForDb ((float) db);
            g.setColour (db == 0 ? tone::grid0 : tone::grid);
            g.fillRect (juce::Rectangle<float> (left, y, width, 1.0f));

            if (! settings.ceilingOn || std::abs (y - ceilingY) > 11.0f)
            {
                g.setColour (tone::faint);
                g.drawText (db == 0 ? juce::String ("0") : minusSign + juce::String (-db),
                            gutter.withY (y - 8.0f).withHeight (16.0f).withTrimmedRight (6.0f), juce::Justification::centredRight, false);
            }
        }

        if (settings.ceilingOn)
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::medium, 11.0f, true));
            g.drawText (minusText (settings.ceiling, 1), gutter.withY (ceilingY - 8.0f).withHeight (16.0f).withTrimmedRight (6.0f), juce::Justification::centredRight, false);
        }

        // Time. The start of a hit gets the room it needs, so these are not evenly spaced.
        g.setFont (font (Weight::regular, 11.0f, true));
        const auto timeBand = juce::Rectangle<float> (left, (float) plot.getBottom(), width, (float) timeHeight);
        struct Mark { double seconds; const char* text; };
        for (const auto& mark : { Mark { 0.005, "5 ms" }, Mark { 0.03, "30 ms" }, Mark { 0.1, "100 ms" }, Mark { 0.2, "200 ms" }, Mark { 0.4, "400 ms" } })
        {
            const float x = left + width * xForSeconds (mark.seconds);
            g.setColour (tone::grid);
            g.fillRect (juce::Rectangle<float> (x - (mark.seconds >= 0.4 ? 1.0f : 0.0f), top, 1.0f, height));
            g.setColour (tone::faint);
            if (mark.seconds >= 0.4)
                g.drawText (mark.text, timeBand.withTrimmedRight (2.0f), juce::Justification::centredRight, false);
            else
                g.drawText (mark.text, juce::Rectangle<float> (x - 40.0f, timeBand.getY(), 80.0f, timeBand.getHeight()), juce::Justification::centred, false);
        }

        // What it is, and how to read it.
        auto band = juce::Rectangle<float> (left, 0.0f, width, (float) captionHeight - 4.0f);

        const auto legendFont = font (Weight::regular, 12.0f);
        const auto swatch = [&] (const char* name, juce::Colour colour, bool line)
        {
            auto item = band.removeFromRight (juce::GlyphArrangement::getStringWidth (legendFont, name) + 18.0f);
            band.removeFromRight (16.0f);
            const auto box = item.removeFromLeft (12.0f).withSizeKeepingCentre (12.0f, line ? 2.0f : 12.0f);
            g.setColour (colour);
            g.fillRect (box);
            g.setColour (tone::muted);
            g.setFont (legendFont);
            g.drawText (name, item.withTrimmedLeft (6.0f), juce::Justification::centredLeft, false);
        };
        swatch ("Taken off", tone::ember, false);
        swatch ("After", tone::hot, false);
        swatch ("Before", tone::text, true);

        g.setColour (showing == Showing::real ? tone::text : tone::muted);
        g.setFont (font (showing == Showing::real ? Weight::medium : Weight::regular, 13.0f));
        g.drawText (captionText(), band, juce::Justification::centredLeft, true);
    }

    void HitView::paint (juce::Graphics& g)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        ++paintCount;

        const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (staticDirty || std::abs (staticScale - scale) > 0.001f || ! staticLayer.isValid())
            rebuildStatic (scale);

        drawLayer (g, staticLayer, scale);

        if (ghostDirty)
            rebuildGhost();
        if (curvesDirty)
            rebuildCurves();

        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (plot);

            // The hit before this one, only ahead of the new one as it draws itself out. Once the new one is whole, this is gone.
            if (! ghostPath.isEmpty() && showing == Showing::real && linePoints.size() > 1)
            {
                const float head = linePoints.back().x;
                if (head < (float) plot.getRight() - 1.0f)
                {
                    juce::Graphics::ScopedSaveState ahead (g);
                    g.reduceClipRegion (juce::Rectangle<float> (head, (float) plot.getY(), (float) plot.getRight() - head, (float) plot.getHeight()).getSmallestIntegerContainer());
                    g.setColour (tone::ghost);
                    g.fillPath (ghostPath);
                }
            }

            if (! hotPath.isEmpty())
            {
                const bool example = showing == Showing::example;

                if (cutsAnything)
                {
                    g.setColour (tone::ember);
                    g.fillPath (emberPath);
                }

                g.setColour (example ? tone::dimHot : tone::hot);
                g.fillPath (hotPath);

                g.setColour (tone::text);
                g.fillPath (linePath);
            }

            if (settings.ceilingOn)
            {
                const float thickness = overLine || draggingLine ? 2.0f : 1.0f;
                g.setColour (tone::text);
                g.fillRect (juce::Rectangle<float> ((float) plot.getX(), ceilingLineY() - thickness * 0.5f, (float) plot.getWidth(), thickness));
            }
        }

        paintSeconds += (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    }

    void HitView::setZone (Zone z)
    {
        if (z != zone)
        {
            zone = z;
            staticDirty = true;
            repaint (plot);
        }
    }

    bool HitView::isNearCeilingLine (float y) const noexcept
    {
        return settings.ceilingOn && std::abs (y - ceilingLineY()) <= 6.0f;
    }

    void HitView::mouseMove (const juce::MouseEvent& e)
    {
        const bool near = plot.contains (e.getPosition()) && isNearCeilingLine (e.position.y);

        if (near != overLine)
        {
            overLine = near;
            hint = near ? juce::String ("Drag to set the Ceiling. Double-click to put it back.") : restingHint;
            setMouseCursor (near ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
            repaint (plot);
        }
    }

    void HitView::mouseExit (const juce::MouseEvent&)
    {
        if (overLine && ! draggingLine)
        {
            overLine = false;
            hint = restingHint;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint (plot);
        }
    }

    void HitView::setCeilingFromY (float y)
    {
        const float proportion = (y - (float) plot.getY()) / (float) juce::jmax (1, plot.getHeight());
        const float db = topDb - proportion * (topDb - bottomDb);
        ceilingAttachment.setValueAsPartOfGesture (juce::jlimit (-24.0f, 0.0f, db));
    }

    void HitView::mouseDown (const juce::MouseEvent& e)
    {
        if (e.getNumberOfClicks() == 1 && plot.contains (e.getPosition()) && isNearCeilingLine (e.position.y))
        {
            draggingLine = true;
            ceilingAttachment.beginGesture();
        }
    }

    void HitView::mouseDrag (const juce::MouseEvent& e)
    {
        if (draggingLine)
            setCeilingFromY (e.position.y);
    }

    void HitView::mouseUp (const juce::MouseEvent&)
    {
        if (draggingLine)
        {
            draggingLine = false;
            ceilingAttachment.endGesture();
            repaint (plot);
        }
    }

    void HitView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        if (plot.contains (e.getPosition()) && isNearCeilingLine (e.position.y))
        {
            auto* param = processor.apvts.getParameter ("ceiling");
            ceilingAttachment.setValueAsCompleteGesture (param->convertFrom0to1 (param->getDefaultValue()));
        }
    }

    // ---- the timeline ----------------------------------------------------------------------------------------------

    TimelineView::TimelineView()
    {
        setComponentID ("timeline");
        setOpaque (true);
        hint = "The last few seconds. The bright bars are the hit shown below.";
    }

    void TimelineView::feed (float before, float after)
    {
        peakBefore = juce::jmax (peakBefore, before);
        peakAfter = juce::jmax (peakAfter, after);

        if (++binsInBar >= barBins)
        {
            auto& slot = bars[written % (unsigned int) numBars];

            slot = { peakBefore, peakAfter };
            barsSinceSound = timelineY (peakAfter) < 1.0f || timelineY (peakBefore) < 1.0f ? 0 : barsSinceSound + 1;
            ++written;
            binsInBar = 0;
            peakBefore = peakAfter = 0.0f;
        }
    }

    void TimelineView::markHit() noexcept
    {
        hitBar = written;
        anyHit = true;
    }

    bool TimelineView::update()
    {
        if (written == paintedAt)
            return false;

        // Once the last sound has scrolled off the left, there is nothing to move.
        const bool moved = barsSinceSound <= getWidth() / 4 + 1;
        paintedAt = written;

        if (moved)
            repaint();

        return moved;
    }

    void TimelineView::paint (juce::Graphics& g)
    {
        g.fillAll (tone::black);

        const auto b = getLocalBounds().toFloat();
        constexpr float pitch = 4.0f, barWidth = 3.0f;
        const int visible = juce::jmin (numBars, (int) (b.getWidth() / pitch), (int) juce::jmin (written, (unsigned int) numBars));

        g.setColour (tone::well);
        g.fillRect (b.withTop (b.getBottom() - 1.0f));

        juce::RectangleList<float> far, near, whites;

        for (int i = 0; i < visible; ++i)
        {
            const unsigned int index = written - 1u - (unsigned int) i;
            const auto& bar = bars[index % (unsigned int) numBars];
            const float x = b.getRight() - barWidth - (float) i * pitch;
            const float afterY = b.getY() + b.getHeight() * timelineY (bar.after);
            const float beforeY = b.getY() + b.getHeight() * timelineY (bar.before);

            if (afterY < b.getBottom() - 0.5f)
            {
                const bool inHit = anyHit && index - hitBar < (unsigned int) barsPerHit;
                (inHit ? near : far).addWithoutMerging ({ x, afterY, barWidth, b.getBottom() - afterY });
            }

            if (std::abs (afterY - beforeY) > 0.6f && beforeY < b.getBottom() - 0.5f)
                whites.addWithoutMerging ({ x, beforeY - 0.5f, barWidth, 1.0f });
        }

        if (! far.isEmpty())   { g.setColour (tone::dimHot); g.fillRectList (far); }
        if (! near.isEmpty())  { g.setColour (tone::hot);    g.fillRectList (near); }
        if (! whites.isEmpty()) { g.setColour (tone::text);  g.fillRectList (whites); }
    }

    // ---- the meters ------------------------------------------------------------------------------------------------

    MeterPanel::MeterPanel()
    {
        setComponentID ("meters");
        setOpaque (true);
        hint = "In and Out share the scale on the left.";
    }

    std::array<juce::Rectangle<int>, 4> MeterPanel::getLanes() const
    {
        const int levelWidth = 26, reductionWidth = 60, gap = 18, bigGap = 22;
        const int h = getHeight();
        const juce::Rectangle<int> in (0, 0, levelWidth, h);
        const auto out = in.withX (in.getRight() + gap);
        const juce::Rectangle<int> impact (out.getRight() + bigGap, 0, reductionWidth, h);
        const auto ceiling = impact.withX (impact.getRight() + 8);
        return { in, out, impact, ceiling };
    }

    void MeterPanel::resized()
    {
        staticDirty = true;
        std::fill (shown.begin(), shown.end(), -100.0f);
    }

    void MeterPanel::setCeiling (float db, bool on)
    {
        if (db != ceilingDb || on != ceilingOn)
        {
            ceilingDb = db;
            ceilingOn = on;
            staticDirty = true;
            repaint();
        }
    }

    bool MeterPanel::setLevels (float inDb, float outDb, float impactDown, float ceilingDown)
    {
        const float h = (float) getHeight();
        const std::array<float, 4> pixels { h * (1.0f - yForDb (inDb)), h * (1.0f - yForDb (outDb)),
                                            h * juce::jlimit (0.0f, 1.0f, impactDown / reductionRange), h * juce::jlimit (0.0f, 1.0f, ceilingDown / reductionRange) };
        bool moved = false;

        for (size_t i = 0; i < 4; ++i)
            if (std::abs (pixels[i] - shown[i]) >= 0.25f)
            {
                shown[i] = pixels[i];
                moved = true;
            }

        if (moved)
            repaint();

        return moved;
    }

    void MeterPanel::rebuildStatic (float scale)
    {
        staticLayer = makeLayer (getWidth(), getHeight(), scale);
        staticScale = scale;
        staticDirty = false;

        juce::Graphics g (staticLayer);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.fillAll (tone::black);

        const auto lanes = getLanes();
        g.setColour (tone::well);
        for (const auto& lane : lanes)
            g.fillRect (lane);

        // The reduction lanes have their own scale, from the top down.
        g.setFont (font (Weight::regular, 11.0f, true));
        for (int laneIndex : { 2, 3 })
        {
            const auto lane = lanes[(size_t) laneIndex].toFloat();
            for (int db = 6; db <= 18; db += 6)
            {
                const float y = lane.getY() + lane.getHeight() * (float) db / reductionRange;
                g.setColour (tone::black);
                g.fillRect (juce::Rectangle<float> (lane.getX(), y, lane.getWidth(), 1.0f));
                g.setColour (tone::faint);
                g.drawText (juce::String (db), juce::Rectangle<float> (lane.getX(), y + 1.0f, lane.getWidth() - 5.0f, 15.0f), juce::Justification::centredRight, false);
            }
        }
    }

    void MeterPanel::paint (juce::Graphics& g)
    {
        const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (staticDirty || std::abs (staticScale - scale) > 0.001f || ! staticLayer.isValid())
            rebuildStatic (scale);

        drawLayer (g, staticLayer, scale);

        const auto lanes = getLanes();
        const float h = (float) getHeight();
        const auto level = [&] (size_t i, juce::Colour colour)
        {
            const float size = juce::jmax (0.0f, shown[i]);
            g.setColour (colour);
            g.fillRect (lanes[i].toFloat().withTop (h - size));
        };
        const auto hanging = [&] (size_t i)
        {
            const float size = juce::jmax (0.0f, shown[i]);
            g.setColour (tone::text);
            g.fillRect (lanes[i].toFloat().withHeight (size));
        };

        level (0, tone::text);
        level (1, tone::hot);
        hanging (2);
        hanging (3);

        if (ceilingOn)
        {
            g.setColour (tone::text);
            g.fillRect (lanes[1].toFloat().withY (h * yForDb (ceilingDb) - 0.5f).withHeight (1.0f));
        }
    }

    juce::String MeterPanel::hintAt (int x) const
    {
        const auto lanes = getLanes();
        if (x < lanes[1].getRight() + 6)  return "In and Out share the level scale on the left.";
        if (x < lanes[3].getX() - 6)      return "How far Impact is pulling the level down, in dB.";
        return "How far the Ceiling is pulling the level down, in dB.";
    }

    void MeterPanel::mouseMove (const juce::MouseEvent& e)  { hint = hintAt (e.x); }
    void MeterPanel::mouseExit (const juce::MouseEvent&)    {}

    // ---- the numbers -----------------------------------------------------------------------------------------------

    ReadoutRow::ReadoutRow()
    {
        setComponentID ("readouts");
        setOpaque (true);
        hint = "The same four, as numbers, in dB.";
    }

    void ReadoutRow::setLanes (const std::array<juce::Rectangle<int>, 4>& lanesInPanel)
    {
        lanes = lanesInPanel;
        repaint();
    }

    void ReadoutRow::setValues (float inDb, float outDb, float impactDown, float ceilingDown, bool ceilingOn)
    {
        const auto level = [] (float db) { return db <= -59.95f ? minusSign + juce::String::fromUTF8 ("\xe2\x88\x9e") : minusText (db, 1); };
        const std::array<juce::String, 4> now { level (inDb), level (outDb),
                                                minusText (-impactDown, 1),
                                                ceilingOn ? minusText (-ceilingDown, 1) : juce::String ("Off") };

        if (now != texts)
        {
            texts = now;
            repaint();
        }
    }

    void ReadoutRow::paint (juce::Graphics& g)
    {
        g.fillAll (tone::black);

        const std::array<const char*, 4> names { "In", "Out", "Impact", "Ceiling" };

        for (size_t i = 0; i < 4; ++i)
        {
            const bool big = i >= 2;
            const auto lane = lanes[i].toFloat();
            const auto column = juce::Rectangle<float> (lane.getX() - (big ? 2.0f : 9.0f), 0.0f, lane.getWidth() + (big ? 4.0f : 18.0f), (float) getHeight());

            // A reduction stands out only while something is being pulled down.
            g.setColour (big && (texts[i] == "0.0" || texts[i] == "Off") ? tone::faint : tone::text);
            g.setFont (font (Weight::medium, big ? 22.0f : 14.0f, true));
            g.drawText (texts[i], column.withHeight (32.0f), juce::Justification::centredBottom, false);

            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, 12.0f));
            g.drawText (names[i], column.withY (33.0f).withHeight (18.0f), juce::Justification::centredTop, false);
        }
    }

    void ReadoutRow::mouseMove (const juce::MouseEvent& e)
    {
        if (e.x < lanes[1].getRight() + 6)      hint = "How loud the sound is going in and coming out, in dB.";
        else if (e.x < lanes[3].getX() - 6)     hint = "How far Impact is pulling the level down right now.";
        else                                    hint = "How far the Ceiling is pulling the level down right now.";
    }

    void ReadoutRow::mouseExit (const juce::MouseEvent&) {}
}
