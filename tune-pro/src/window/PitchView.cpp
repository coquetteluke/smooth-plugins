#include "PitchView.h"

using namespace smoothtune;
using namespace tuneui;

namespace
{
    constexpr bool isSharp[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    constexpr float maxJump = 5.0f;    // notes; a bigger step between points is a new phrase, not a slide
    constexpr int minBlockPoints = 8;  // 40 ms; a target held for less is the tracker settling, not a note

    juce::String noteName (int midi)
    {
        return midi < 0 ? juce::String() : juce::String (noteNames[midi % 12]) + juce::String (midi / 12 - 1);
    }

    int wrapped (int note) noexcept { return ((note % 12) + 12) % 12; }

    float inputOf (const PluginProcessor::PitchPoint& p) noexcept  { return p.inputNote.load (std::memory_order_relaxed); }
    float outputOf (const PluginProcessor::PitchPoint& p) noexcept { return p.outputNote.load (std::memory_order_relaxed); }
    float targetOf (const PluginProcessor::PitchPoint& p) noexcept { return p.targetNote.load (std::memory_order_relaxed); }
    bool bypassedAt (const PluginProcessor::PitchPoint& p) noexcept { return p.bypassed.load (std::memory_order_relaxed); }
}

PitchView::PitchView (PluginProcessor& p) : processor (p)
{
    hint = "White is your voice. Coral is what comes out. The filled gap is the correction.";
    setOpaque (true);
    setTitle ("Pitch");
    for (int i = 0; i < 12; ++i)
        noteParams[(size_t) i] = processor.apvts.getRawParameterValue ("note" + juce::String (i));
    followKeyParam = processor.apvts.getRawParameterValue ("followKey");
    bypassParam = processor.apvts.getRawParameterValue ("bypass");
    addAndMakeVisible (legend);
    addAndMakeVisible (gutter);
    legend.setBufferedToImage (true);
    gutter.setBufferedToImage (true);
}

void PitchView::layoutChildren()
{
    legend.setBounds (0, 0, juce::roundToInt (228.0f * typeScale), juce::roundToInt ((legend.follow.isNotEmpty() ? 68.0f : 50.0f) * typeScale));
    gutter.setBounds (getLocalBounds().removeFromRight (gutterWidth()));
}

void PitchView::setTypeScale (float s)
{
    typeScale = s;
    legend.scale = s;
    legend.repaint();
    gutter.repaint();
    repaint();
}

void PitchView::resized()
{
    span = juce::jlimit (14, 24, juce::roundToInt ((float) getHeight() / (20.0f * typeScale)));
    wholeRingStale = true;
    ring = {};
    ringScale = 0.0f;
    layoutChildren();
    gutter.repaint();
    repaint();
}

// Keep what was sung lately on screen. It moves in whole notes, so a held note stays put between moves.
void PitchView::followPitch (long long count)
{
    // By share of what is on screen, not the very top and bottom, so one wild tracker reading or a slide at the end of a word cannot push the voice off the display.
    int histogram[128] {};
    int voiced = 0;
    // Both lines that are drawn count: the voice as sung, and where it was not bypassed the corrected voice. Up to 1.8.1 only the
    // corrected voice counted, so with Transpose the voice as sung could sit off screen, and bypassed it was the only line drawn.
    for (int k = 0; k < pointsShown && k < count; ++k)
    {
        const auto& point = processor.getPitchPoint (count - 1 - k);
        const float in = inputOf (point), out = outputOf (point);
        if (in > 0.0f)
        {
            ++histogram[juce::jlimit (0, 127, juce::roundToInt (in))];
            ++voiced;
        }
        if (out > 0.0f && ! bypassedAt (point))
        {
            ++histogram[juce::jlimit (0, 127, juce::roundToInt (out))];
            ++voiced;
        }
    }
    voicedOnScreen = voiced;

    if (voiced == 0)
        return;

    const int skip = voiced / 25;
    int lowest = 0, highest = 127;
    for (int seen = 0; lowest < 127 && (seen += histogram[lowest]) <= skip; ++lowest) {}
    for (int seen = 0; highest > 0 && (seen += histogram[highest]) <= skip; --highest) {}

    if ((float) lowest < viewTarget + 1.0f || (float) highest > viewTarget + (float) span - 2.0f)
        viewTarget = std::round (0.5f * (float) (lowest + highest) - 0.5f * (float) (span - 1));
}

bool PitchView::laneStateChanged()
{
    std::uint16_t removed = 0, bypassed = 0;
    for (int i = 0; i < 12; ++i)
    {
        const int mode = juce::roundToInt (noteParams[(size_t) i]->load (std::memory_order_relaxed));
        if (mode == noteRemove) removed = (std::uint16_t) (removed | (1u << i));
        if (mode == noteBypass) bypassed = (std::uint16_t) (bypassed | (1u << i));
    }
    const auto scale = processor.getInScaleMask();
    const bool changed = removed != removedShown || bypassed != bypassedShown || scale != scaleShown;
    removedShown = removed;
    bypassedShown = bypassed;
    scaleShown = scale;
    return changed;
}

bool PitchView::advance (double dtSeconds)
{
    const auto count = processor.getPitchHistoryCount();
    const bool newPoints = count != lastSeenCount;
    lastSeenCount = count;

    const int voicedBefore = voicedOnScreen;
    if (newPoints)
        followPitch (count);

    bool moved = false;

    // The last of the voice has just scrolled off: one more frame to clear it.
    if (voicedBefore > 0 && voicedOnScreen == 0)
    {
        repaint (traceArea());
        moved = true;
    }

    if (laneStateChanged())
    {
        wholeRingStale = true;
        moved = true;
        repaint (traceArea());
        gutter.repaint();
    }

    // The lanes glide to a new range rather than jumping, redrawing the whole ring only for those few frames.
    if (viewLow != viewTarget)
    {
        const float next = viewLow + (viewTarget - viewLow) * (1.0f - std::exp (-(float) dtSeconds / 0.06f));
        viewLow = std::abs (next - viewTarget) < 0.02f ? viewTarget : next;
        wholeRingStale = true;
        moved = true;
        repaint (traceArea());
        gutter.repaint();
    }

    auto r = processor.getReadings();
    if (stale)
        r.voiced = false;
    bypassedNow = bypassParam->load (std::memory_order_relaxed) > 0.5f;
    const int target = r.voiced && ! bypassedNow ? r.targetNote : -1;
    if (target != targetShown)
    {
        gutter.repaint (gutter.cellFor (targetShown).getSmallestIntegerContainer());
        gutter.repaint (gutter.cellFor (target).getSmallestIntegerContainer());
        targetShown = target;
        moved = true;
    }

    // Numbers change slowly enough to read.
    readoutClock += dtSeconds;
    if (readoutClock >= 0.2)
    {
        readoutClock = 0.0;
        const auto voice = r.voiced ? juce::String (r.detectedHz, 1) + " Hz" : juce::String();
        const auto corrected = r.voiced ? juce::String (r.outputHz, 1) + " Hz" : juce::String();
        // Follow Key on names the key it follows only once there is one from the key file. Up to 1.8.1 it named the Key and Scale menus'
        // own key, as if it came from the file (and could name Chromatic, which the Key plug-in never gives).
        const bool following = followKeyParam->load (std::memory_order_relaxed) > 0.5f;
        const auto follow = ! following ? juce::String()
                          : r.following ? juce::String ("Following ") + noteNames[juce::jlimit (0, 11, r.activeKey)] + " " + scaleNames[juce::jlimit (0, numScales - 1, r.activeScale)]
                                        : juce::String ("No key from the key file yet");
        const bool bypassed = bypassedNow;

        if (voice != legend.voice || corrected != legend.corrected || follow != legend.follow || bypassed != legend.bypassed)
        {
            const bool resize = follow.isEmpty() != legend.follow.isEmpty();
            legend.voice = voice;
            legend.corrected = corrected;
            legend.follow = follow;
            legend.bypassed = bypassed;
            legend.repaint();
            if (resize)
                legend.setSize (legend.getWidth(), juce::roundToInt ((follow.isNotEmpty() ? 68.0f : 50.0f) * typeScale));
            moved = true;
        }
    }

    // New points only matter if some of what is on screen is a voice. Otherwise the picture is the same empty lanes.
    if (newPoints && (voicedOnScreen > 0 || drawnCount < 0))
    {
        repaint (traceArea());
        moved = true;
    }

    return moved;
}

void PitchView::strokeLine (juce::Graphics& g, long long firstPoint, long long lastPoint, float (*read) (const PluginProcessor::PitchPoint&) noexcept, juce::Colour colour, float width)
{
    line.clear();
    bool open = false;
    float previous = 0.0f;
    for (auto k = firstPoint; k <= lastPoint; ++k)
    {
        const float n = read (processor.getPitchPoint (k));
        if (n <= 0.0f)
        {
            open = false;
            continue;
        }
        const auto x = (float) columnOf (k), y = yOf (n);
        if (open && std::abs (n - previous) < maxJump)
            line.lineTo (x, y);
        else
            line.startNewSubPath (x, y);
        open = true;
        previous = n;
    }
    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// Draws columns fromX up to toX, in numbering that does not wrap, into a graphics context already shifted so that those numbers land in the ring.
void PitchView::drawColumns (juce::Graphics& g, long long fromX, long long toX, long long count)
{
    const auto left = (float) fromX, width = (float) (toX - fromX);

    // Lanes: white-key notes a little lighter than black-key notes, notes outside the scale darkest.
    g.setColour (tone::inkDeep);
    g.fillRect (left, 0.0f, width, (float) ringHeight);

    const int firstNote = (int) std::floor (viewLow) - 1;
    for (int n = firstNote; n <= firstNote + span + 2; ++n)
    {
        const int pc = wrapped (n);
        const bool removed = ((removedShown >> pc) & 1) != 0, bypassed = ((bypassedShown >> pc) & 1) != 0;
        const bool inScale = (((scaleShown >> pc) & 1) != 0 && ! removed) || bypassed;
        if (! inScale)
            continue;
        const float top = std::round (yOf ((float) n + 0.5f)), bottom = std::round (yOf ((float) n - 0.5f));
        g.setColour (isSharp[pc] ? tone::laneSharp : tone::laneNatural);
        g.fillRect (left, top, width, bottom - top);
    }

    // The points whose strokes can touch these columns. A little extra each side, so joins and round ends come out whole.
    const auto pad = (double) (ringScale * 6.0f + 2.0f);
    auto firstPoint = (long long) std::floor (((double) fromX - pad) / pixelsPerPoint);
    auto lastPoint = (long long) std::ceil (((double) toX + pad) / pixelsPerPoint);
    firstPoint = juce::jmax (firstPoint, juce::jmax (0LL, count - (long long) PluginProcessor::pitchHistorySize + 8));
    lastPoint = juce::jmin (lastPoint, count - 1);

    if (lastPoint < firstPoint)
        return;

    // Each stretch as it was heard: bypassed, the voice as sung is all there was, so that is all that is drawn there. Each point keeps
    // whether it was bypassed, so drawing the whole display again keeps it (up to 1.8.1 the live Bypass switch decided for all 8 s).
    long long runStart = firstPoint;
    for (auto k = firstPoint + 1; k <= lastPoint + 1; ++k)
        if (k > lastPoint || bypassedAt (processor.getPitchPoint (k)) != bypassedAt (processor.getPitchPoint (runStart)))
        {
            drawPoints (g, runStart, k - 1, count, bypassedAt (processor.getPitchPoint (runStart)));
            runStart = k;
        }
}

void PitchView::drawPoints (juce::Graphics& g, long long firstPoint, long long lastPoint, long long count, bool bypassed)
{
    if (bypassed)
    {
        strokeLine (g, firstPoint, lastPoint, inputOf, tone::white, 1.6f * ringScale * typeScale);
        return;
    }

    // The note being hit, as a block in its lane, so the notes themselves read at a glance. A target held only a moment is the
    // tracker finding its feet at the start of a sound, so it gets no block. One point per pixel column, so overlaps cannot stripe it.
    g.setColour (tone::coral.withAlpha (0.20f));
    for (auto k = firstPoint; k <= lastPoint; ++k)
    {
        const auto x0 = columnOf (k), x1 = columnOf (k + 1);
        if (x1 == x0)
            continue;
        const float t = targetOf (processor.getPitchPoint (k));
        if (t < 0.0f || outputOf (processor.getPitchPoint (k)) <= 0.0f)
            continue;
        int held = 1;
        for (auto j = k - 1; j >= 0 && j > k - minBlockPoints && targetOf (processor.getPitchPoint (j)) == t; --j) ++held;
        for (auto j = k + 1; j < count && j < k + minBlockPoints && targetOf (processor.getPitchPoint (j)) == t; ++j) ++held;
        if (held < minBlockPoints)
            continue;
        const float top = std::round (yOf (t + 0.5f)), bottom = std::round (yOf (t - 0.5f));
        g.fillRect ((float) x0, top, (float) (x1 - x0), bottom - top);
    }

    // The gap: how far the voice was moved at each moment.
    g.setColour (tone::coral.withAlpha (0.34f));
    for (auto k = firstPoint; k <= lastPoint; ++k)
    {
        const auto x0 = columnOf (k), x1 = columnOf (k + 1);
        if (x1 == x0)
            continue;
        const auto& p = processor.getPitchPoint (k);
        const float in = inputOf (p), out = outputOf (p);
        if (in <= 0.0f || out <= 0.0f || std::abs (in - out) > maxJump)
            continue;
        const float y0 = yOf (juce::jmax (in, out)), y1 = yOf (juce::jmin (in, out));
        if (y1 - y0 > 0.5f)
            g.fillRect ((float) x0, y0, (float) (x1 - x0), y1 - y0);
    }

    // Coral under, white over, so the voice as sung stays readable even where the two lie together.
    strokeLine (g, firstPoint, lastPoint, outputOf, tone::coral, 2.8f * ringScale * typeScale);
    strokeLine (g, firstPoint, lastPoint, inputOf, tone::white, 1.1f * ringScale * typeScale);
}

void PitchView::bringRingUpToDate (float pixelScale)
{
    const auto area = traceArea();
    if (area.isEmpty())
        return;

    const int wantWidth = juce::roundToInt ((float) area.getWidth() * pixelScale);
    const int wantHeight = juce::roundToInt ((float) area.getHeight() * pixelScale);

    if (! ring.isValid() || ringScale != pixelScale || visibleColumns != wantWidth || ringHeight != wantHeight)
    {
        ringScale = pixelScale;
        visibleColumns = wantWidth;
        ringHeight = wantHeight;
        ringWidth = wantWidth + 64;
        pixelsPerPoint = (double) wantWidth / (double) pointsShown;
        ring = juce::Image (juce::Image::RGB, ringWidth, ringHeight, false);
        wholeRingStale = true;
    }

    const auto count = processor.getPitchHistoryCount();
    const auto newest = columnOf (juce::jmax (0LL, count - 1));

    long long fromX;
    if (wholeRingStale || drawnCount < 0 || newest - columnOf (juce::jmax (0LL, drawnCount - 1)) >= ringWidth - 16)
    {
        fromX = newest - ringWidth + 1;
        wholeRingStale = false;
    }
    else
    {
        if (drawnCount == count)
            return;
        // Back over the last few columns too, so the end of the line that was drawn last time joins the new part cleanly.
        fromX = columnOf (juce::jmax (0LL, drawnCount - 1)) - (long long) (ringScale * 5.0f + 2.0f);
    }
    const long long toX = newest + 2;

    juce::Graphics g (ring);
    const auto wrapStart = [this] (long long x) { return x >= 0 ? x / ringWidth : -((-x + ringWidth - 1) / ringWidth); };

    for (auto wrap = wrapStart (fromX); wrap * ringWidth < toX; ++wrap)
    {
        const auto shift = wrap * ringWidth;
        const auto a = juce::jmax (fromX, shift), b = juce::jmin (toX, shift + ringWidth);
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion ((int) (a - shift), 0, (int) (b - a), ringHeight);
        g.addTransform (juce::AffineTransform::translation ((float) -shift, 0.0f));
        drawColumns (g, a, b, count);
    }

    drawnCount = count;
}

juce::Rectangle<float> PitchView::Gutter::cellFor (int note) const noexcept
{
    const float laneHeight = (float) getHeight() / (float) owner.span;
    return { 0.0f, (float) getHeight() * (1.0f - ((float) note + 0.5f - owner.viewLow + 0.5f) / (float) owner.span), (float) getWidth(), laneHeight };
}

void PitchView::Gutter::paint (juce::Graphics& g)
{
    g.fillAll (tone::ink);

    const float s = owner.typeScale;
    const float laneHeight = (float) getHeight() / (float) owner.span;
    const float size = juce::jmin (11.5f * s, laneHeight * 0.72f);
    const int firstNote = (int) std::floor (owner.viewLow) - 1;

    for (int n = firstNote; n <= firstNote + owner.span + 2; ++n)
    {
        const auto cell = cellFor (n);
        if (! g.clipRegionIntersects (cell.getSmallestIntegerContainer()))
            continue;

        const int pc = wrapped (n);
        const bool removed = ((owner.removedShown >> pc) & 1) != 0, bypassed = ((owner.bypassedShown >> pc) & 1) != 0;
        const bool inScale = (((owner.scaleShown >> pc) & 1) != 0 && ! removed) || bypassed;

        if (n == owner.targetShown)
        {
            g.setColour (tone::coral);
            g.fillRect (cell);
            g.setColour (tone::ink);
            g.setFont (font (Weight::medium, size));
        }
        else if (! inScale)
        {
            continue;
        }
        else
        {
            g.setColour (pc == 0 ? tone::white : (isSharp[pc] ? tone::faint : tone::muted));
            g.setFont (font (pc == 0 ? Weight::medium : Weight::regular, size));
        }

        g.drawText (noteName (n), cell.reduced (8.0f * s, 0.0f), juce::Justification::centredLeft, false);
    }
}

void PitchView::Legend::paint (juce::Graphics& g)
{
    g.fillAll (tone::ink);

    const float s = scale;
    auto plate = getLocalBounds().toFloat().withTrimmedLeft (24.0f * s).withTrimmedRight (12.0f * s).withTrimmedTop (2.0f * s);

    const auto row = [&] (const juce::String& name, const juce::String& value, juce::Colour colour, float thickness)
    {
        auto r = plate.removeFromTop (22.0f * s);
        g.setColour (colour);
        if (thickness > 0.0f)
            g.fillRect (juce::Rectangle<float> (r.getX(), r.getCentreY() - thickness * 0.5f * s, 14.0f * s, thickness * s));
        r.removeFromLeft (22.0f * s);
        g.setColour (tone::white);
        g.setFont (font (Weight::regular, 13.0f * s));
        g.drawText (name, r, juce::Justification::centredLeft, false);
        g.setFont (font (Weight::medium, 13.0f * s));
        g.drawText (value, r, juce::Justification::centredRight, false);
    };

    row ("Your voice", voice, tone::white, 1.5f);
    if (bypassed)
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f * s));
        g.drawText ("Bypassed, tuning is off", plate.removeFromTop (22.0f * s), juce::Justification::centredLeft, false);
    }
    else
    {
        row ("Corrected", corrected, tone::coral, 2.5f);
    }

    if (follow.isNotEmpty())
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 12.5f * s));
        g.drawText (follow, plate.removeFromTop (18.0f * s), juce::Justification::centredLeft, false);
    }
}

void PitchView::paint (juce::Graphics& g)
{
    ++paintCount;
    const auto area = traceArea();
    const float pixelScale = g.getInternalContext().getPhysicalPixelScaleFactor();

    if (g.clipRegionIntersects (area))
    {
        bringRingUpToDate (pixelScale);

        if (ring.isValid())
        {
            // Copy the ring out at whole pixels, in up to two pieces where it wraps. No resampling, no rebuilding.
            const auto newest = columnOf (juce::jmax (0LL, processor.getPitchHistoryCount() - 1));
            const auto clamped = juce::jmin (newest, columnOf (juce::jmax (0LL, drawnCount - 1)));
            const auto leftmost = clamped - visibleColumns + 1;
            auto start = leftmost % ringWidth;
            if (start < 0)
                start += ringWidth;

            juce::Graphics::ScopedSaveState state (g);
            g.addTransform (juce::AffineTransform::scale (1.0f / pixelScale));
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.setOpacity (1.0f);

            const int firstWidth = juce::jmin ((int) (ringWidth - start), visibleColumns);
            {
                juce::Graphics::ScopedSaveState piece (g);
                g.reduceClipRegion (0, 0, firstWidth, ringHeight);
                g.drawImageAt (ring, (int) -start, 0);
            }
            if (firstWidth < visibleColumns)
            {
                juce::Graphics::ScopedSaveState piece (g);
                g.reduceClipRegion (firstWidth, 0, visibleColumns - firstWidth, ringHeight);
                g.drawImageAt (ring, firstWidth, 0);
            }
        }

        if (voicedOnScreen == 0)
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, 15.0f * typeScale));
            g.drawText ("Play a vocal and its pitch shows here", area, juce::Justification::centred, false);
        }
    }

}
