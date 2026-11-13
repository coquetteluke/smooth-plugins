#include "PitchStrip.h"

using namespace smoothtune;
using namespace fxui;

namespace
{
    constexpr bool isSharp[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    constexpr float maxJump = 1.5f;    // notes; a bigger step between points is a jump to a new note, so the line breaks and hard tuning reads as blocks
    const juce::Colour laneNatural { 0xff101010 }, laneSharp { 0xff0a0a0a };

    int wrapped (int note) noexcept { return ((note % 12) + 12) % 12; }

    float inputOf (const PluginProcessor::PitchPoint& p) noexcept    { return p.inputNote.load (std::memory_order_relaxed); }
    float outputOf (const PluginProcessor::PitchPoint& p) noexcept   { return p.outputNote.load (std::memory_order_relaxed); }
    float harmony1Of (const PluginProcessor::PitchPoint& p) noexcept { return p.harmony1.load (std::memory_order_relaxed); }
    float harmony2Of (const PluginProcessor::PitchPoint& p) noexcept { return p.harmony2.load (std::memory_order_relaxed); }
}

PitchStrip::PitchStrip (PluginProcessor& p) : processor (p)
{
    hint = "White is your voice. Magenta is what comes out. The colour of Harmony is its added voices.";
    setOpaque (true);
    setTitle ("Pitch");
}

void PitchStrip::resized()
{
    span = juce::jlimit (12, 24, juce::roundToInt ((float) getHeight() / (9.6f * typeScale)));
    wholeRingStale = true;
    ring = {};
    ringScale = 0.0f;

    // Rounded corners, as four small flat masks that never change.
    const auto b = traceArea().toFloat();
    const float r = 8.0f * typeScale;
    for (int i = 0; i < 4; ++i)
    {
        const bool right = i == 1 || i == 2, bottom = i >= 2;
        const float x = right ? b.getRight() : b.getX(), y = bottom ? b.getBottom() : b.getY();
        const float dx = right ? -r : r, dy = bottom ? -r : r;
        auto& corner = corners[(size_t) i];
        corner.clear();
        corner.startNewSubPath (x, y);
        corner.lineTo (x + dx, y);
        corner.quadraticTo (x, y, x, y + dy);
        corner.closeSubPath();
    }
    pianoStale = true;
    repaint();
}

// Keep what comes out on screen, with the added voices too when there is room. It moves in whole notes, so a held note stays put between moves.
void PitchStrip::followPitch (long long count)
{
    int histogram[128] {}, outputs[128] {};
    int voiced = 0;
    for (int k = 0; k < pointsShown && k < count; ++k)
    {
        const auto& point = processor.getPitchPoint (count - 1 - k);
        const float n = outputOf (point);
        if (n > 0.0f)
        {
            ++histogram[juce::jlimit (0, 127, juce::roundToInt (n))];
            ++outputs[juce::jlimit (0, 127, juce::roundToInt (n))];
            ++voiced;
            for (const float h : { harmony1Of (point), harmony2Of (point) })
                if (h > 0.0f)
                    ++histogram[juce::jlimit (0, 127, juce::roundToInt (h))];
        }
    }
    voicedOnScreen = voiced;

    if (voiced == 0)
        return;

    // By share of what is on screen, not the very top and bottom, so one wild tracker reading cannot push the voice off the display.
    const int skip = voiced / 25;
    const auto spread = [skip] (const int* h, int& lowest, int& highest)
    {
        lowest = 0;
        highest = 127;
        for (int seen = 0; lowest < 127 && (seen += h[lowest]) <= skip; ++lowest) {}
        for (int seen = 0; highest > 0 && (seen += h[highest]) <= skip; --highest) {}
    };
    int lowest = 0, highest = 127;
    spread (histogram, lowest, highest);
    // The added voices only when there is room for them as well: otherwise the view centred between them and could lose the voice coming out (1.2.5).
    if (highest - lowest > span - 3)
        spread (outputs, lowest, highest);

    if ((float) lowest < viewTarget + 1.0f || (float) highest > viewTarget + (float) span - 2.0f)
        viewTarget = std::round (0.5f * (float) (lowest + highest) - 0.5f * (float) (span - 1));
}

bool PitchStrip::scaleChanged()
{
    const auto scale = processor.getScaleMask();
    const bool changed = scale != scaleShown;
    scaleShown = scale;
    return changed;
}

bool PitchStrip::advance (double dtSeconds)
{
    const auto count = processor.getPitchHistoryCount();
    const bool newPoints = count != lastSeenCount;
    lastSeenCount = count;

    const int voicedBefore = voicedOnScreen;
    if (newPoints)
        followPitch (count);

    bool moved = false;
    const auto piano = getLocalBounds().removeFromRight (pianoWidth());

    if (scaleChanged())
    {
        wholeRingStale = true;
        pianoStale = true;
        moved = true;
        repaint();
    }

    // The lanes glide to a new range rather than jumping, redrawing the whole ring only for those few frames.
    if (viewLow != viewTarget)
    {
        const float next = viewLow + (viewTarget - viewLow) * (1.0f - std::exp (-(float) dtSeconds / 0.06f));
        viewLow = std::abs (next - viewTarget) < 0.02f ? viewTarget : next;
        wholeRingStale = true;
        pianoStale = true;
        moved = true;
        repaint();
    }

    // The note coming out, on the piano.
    const auto r = processor.getReadings();
    int note = -1;
    if (r.voiced && count > 0)
    {
        const float out = outputOf (processor.getPitchPoint (count - 1));
        note = out > 0.0f ? juce::roundToInt (out) : -1;
    }
    if (note != outputShown)
    {
        outputShown = note;
        pianoStale = true;
        repaint (piano);
        moved = true;
    }

    // New points only matter if some of what is on screen is a voice. Otherwise the picture is the same empty lanes.
    // And once more as the last of a voice leaves, so its end goes and the hint comes back (1.2.5; the strip froze on it).
    if (newPoints && (voicedOnScreen > 0 || voicedBefore > 0 || drawnCount < 0))
    {
        repaint (traceArea());
        moved = true;
    }

    return moved;
}

// The strip is drawn straight into the ring's pixels, one column at a time, with no paths at all: on this Mac a stroked path through a graphics context was
// most of the window's whole cost. A line is its span of height in each column, widened to its thickness, with the top and bottom pixels shaded to keep it smooth.
void PitchStrip::rebuildLanes()
{
    laneColours.assign ((size_t) ringHeight, juce::PixelARGB (255, 0, 0, 0));
    const int firstNote = (int) std::floor (viewLow) - 1;

    for (int n = firstNote; n <= firstNote + span + 2; ++n)
    {
        const int pc = wrapped (n);
        if (((scaleShown >> pc) & 1) == 0)
            continue;

        const auto colour = (isSharp[pc] ? laneSharp : laneNatural).getPixelARGB();
        const int top = juce::roundToInt (yOf ((float) n + 0.5f)), bottom = juce::roundToInt (yOf ((float) n - 0.5f));
        for (int y = juce::jmax (0, top); y < juce::jmin (ringHeight, bottom); ++y)
            laneColours[(size_t) y] = colour;
    }
}

template <typename Read>
void PitchStrip::drawLineInColumn (juce::Image::BitmapData& pixels, int ringX, long long x, long long oldest, long long count, Read read, juce::Colour colour, float thickness)
{
    const double from = (double) x - (double) (thickness - 1.0f) * 0.5, to = (double) x + 1.0 + (double) (thickness - 1.0f) * 0.5;
    const auto first = juce::jmax (oldest, (long long) std::floor (from / pixelsPerPoint));
    const auto last = juce::jmin (count - 2, (long long) std::ceil (to / pixelsPerPoint));

    const auto r = colour.getRed(), g = colour.getGreen(), b = colour.getBlue();

    // One unbroken run of line inside this column. A jump to a new note starts another run, so nothing is ever filled across the jump.
    const auto fillRun = [&] (float low, float high)
    {
        const float top = yOf (high) - thickness * 0.5f, bottom = yOf (low) + thickness * 0.5f;
        const int yStart = juce::jmax (0, (int) std::floor (top)), yEnd = juce::jmin (ringHeight, (int) std::ceil (bottom));

        for (int y = yStart; y < yEnd; ++y)
        {
            const float coverage = juce::jmin (1.0f, (float) (y + 1) - top) * juce::jmin (1.0f, bottom - (float) y);
            auto* p = reinterpret_cast<juce::PixelARGB*> (pixels.getPixelPointer (ringX, y));
            if (coverage >= 0.999f)
            {
                p->setARGB (255, r, g, b);
            }
            else if (coverage > 0.0f)
            {
                const auto mix = [coverage] (juce::uint8 under, juce::uint8 over) { return (juce::uint8) ((float) under + ((float) over - (float) under) * coverage); };
                p->setARGB (255, mix (p->getRed(), r), mix (p->getGreen(), g), mix (p->getBlue(), b));
            }
        }
    };

    float low = 1.0e9f, high = -1.0e9f;
    for (auto k = first; k <= last; ++k)
    {
        const float a = read (processor.getPitchPoint (k)), c = read (processor.getPitchPoint (k + 1));
        if (a <= 0.0f || c <= 0.0f || std::abs (a - c) >= maxJump)
        {
            if (high >= low)
                fillRun (low, high);
            low = 1.0e9f;
            high = -1.0e9f;
            continue;
        }

        // Only the part of this piece of line that falls inside the column, so a steep line does not smear into its neighbours.
        const double start = (double) k * pixelsPerPoint, length = pixelsPerPoint;
        if (to <= start || from >= start + length)
            continue;
        const auto t0 = (float) juce::jlimit (0.0, 1.0, (from - start) / length), t1 = (float) juce::jlimit (0.0, 1.0, (to - start) / length);
        const float n0 = a + (c - a) * t0, n1 = a + (c - a) * t1;
        low = juce::jmin (low, n0, n1);
        high = juce::jmax (high, n0, n1);
    }

    if (high >= low)
        fillRun (low, high);
}

void PitchStrip::drawColumn (juce::Image::BitmapData& pixels, long long x, long long count)
{
    const int ringX = (int) (((x % ringWidth) + ringWidth) % ringWidth);

    for (int y = 0; y < ringHeight; ++y)
        *reinterpret_cast<juce::PixelARGB*> (pixels.getPixelPointer (ringX, y)) = laneColours[(size_t) y];

    const auto oldest = juce::jmax (0LL, count - (long long) PluginProcessor::pitchHistorySize + 8);
    const auto k = (long long) std::floor (((double) x + 0.5) / pixelsPerPoint);

    if (count < 2 || k < oldest || k >= count)
        return;

    // The note coming out, as a block in its lane, so the notes themselves read at a glance and Motion's steps show. A note held only a moment
    // is the tracker finding its feet at the start of a sound, so it gets no block.
    const auto laneOf = [this] (long long i) { const float out = outputOf (processor.getPitchPoint (i)); return out > 0.0f ? juce::roundToInt (out) : -1; };
    const int lane = laneOf (k);
    if (lane >= 0 && ((k >= oldest + 3 && laneOf (k - 3) == lane) || (k + 3 < count && laneOf (k + 3) == lane)))
    {
        const int top = juce::jmax (0, juce::roundToInt (yOf ((float) lane + 0.5f))), bottom = juce::jmin (ringHeight, juce::roundToInt (yOf ((float) lane - 0.5f)));
        const auto tint = tone::magenta;
        for (int y = top; y < bottom; ++y)
        {
            auto* p = reinterpret_cast<juce::PixelARGB*> (pixels.getPixelPointer (ringX, y));
            const auto mix = [] (juce::uint8 under, juce::uint8 over) { return (juce::uint8) ((int) under + (((int) over - (int) under) * 66) / 255); };
            p->setARGB (255, mix (p->getRed(), tint.getRed()), mix (p->getGreen(), tint.getGreen()), mix (p->getBlue(), tint.getBlue()));
        }
    }

    const auto harmonyColour = modules()[3].colour;
    const float unit = ringScale * typeScale;
    drawLineInColumn (pixels, ringX, x, oldest, count, harmony1Of, harmonyColour, 2.0f * unit);
    drawLineInColumn (pixels, ringX, x, oldest, count, harmony2Of, harmonyColour, 2.0f * unit);
    drawLineInColumn (pixels, ringX, x, oldest, count, outputOf, tone::magenta, 3.0f * unit);
    drawLineInColumn (pixels, ringX, x, oldest, count, inputOf, tone::white, 1.1f * unit);
}

void PitchStrip::bringRingUpToDate (float pixelScale)
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
        ring = juce::Image (juce::Image::ARGB, ringWidth, ringHeight, false);
        wholeRingStale = true;
    }

    const auto count = processor.getPitchHistoryCount();
    const auto newest = columnOf (juce::jmax (0LL, count - 1));

    long long fromX;
    if (wholeRingStale || drawnCount < 0 || newest - columnOf (juce::jmax (0LL, drawnCount - 1)) >= ringWidth - 16)
    {
        fromX = newest - ringWidth + 1;
        wholeRingStale = false;
        rebuildLanes();
    }
    else
    {
        if (drawnCount == count)
            return;
        // Back over the last few columns too, because a line's width reaches into columns whose next point had not arrived yet.
        fromX = columnOf (juce::jmax (0LL, drawnCount - 1)) - (long long) (ringScale * typeScale * 2.0f + 3.0f);
    }

    juce::Image::BitmapData pixels (ring, juce::Image::BitmapData::readWrite);
    for (auto x = fromX; x <= newest + 1; ++x)
        drawColumn (pixels, x, count);

    drawnCount = count;
}

void PitchStrip::paintPiano (juce::Graphics& g)
{
    const auto piano = getLocalBounds().removeFromRight (pianoWidth()).toFloat();
    const auto gap = juce::Rectangle<float> ((float) traceArea().getRight(), 0.0f, piano.getX() - (float) traceArea().getRight(), (float) getHeight());
    g.setColour (tone::black);
    g.fillRect (piano.getUnion (gap));

    const float laneHeight = (float) getHeight() / (float) span;
    const auto yOfNote = [&] (float n) { return (float) getHeight() * (1.0f - (n - viewLow + 0.5f) / (float) span); };
    const int firstNote = (int) std::floor (viewLow) - 1;
    const auto labelFont = font (Weight::medium, juce::jmin (10.0f * typeScale, laneHeight * 0.95f));
    const juce::Colour naturalIn { 0xffe6e6e6 }, naturalOut { 0xff333333 }, sharpIn { 0xff6e6e6e }, sharpOut { 0xff1d1d1d };

    for (int n = firstNote; n <= firstNote + span + 2; ++n)
    {
        const int pc = wrapped (n);
        const bool inScale = ((scaleShown >> pc) & 1) != 0;
        const float top = yOfNote ((float) n + 0.5f);
        auto cell = juce::Rectangle<float> (piano.getX(), top, piano.getWidth(), laneHeight);
        const auto key = cell.withTrimmedBottom (juce::jmin (1.0f, laneHeight * 0.1f)).withTrimmedRight (isSharp[pc] ? 8.0f * typeScale : 0.0f);

        const bool isOutput = n == outputShown;
        g.setColour (isOutput ? tone::magenta : (isSharp[pc] ? (inScale ? sharpIn : sharpOut) : (inScale ? naturalIn : naturalOut)));
        g.fillRect (key);

        if (pc == 0)
        {
            g.setColour (isOutput ? tone::white : (inScale ? tone::black : tone::muted));
            g.setFont (labelFont);
            g.drawText ("C" + juce::String (n / 12 - 1), cell.translated (3.0f * typeScale, -0.5f), juce::Justification::centredLeft, false);
        }
    }
}

void PitchStrip::paint (juce::Graphics& g)
{
    ++paintCount;
    const auto area = traceArea();
    const float pixelScale = g.getInternalContext().getPhysicalPixelScaleFactor();

    if (g.clipRegionIntersects (area))
    {
        bringRingUpToDate (pixelScale);

        if (ring.isValid())
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (area);

            // One to one with the screen, at a whole pixel offset, so the copy needs no resampling.
            g.addTransform (juce::AffineTransform::scale (1.0f / ringScale));
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

            // Placed by the count the ring was drawn to, not a second read that audio may have moved on past the drawn columns (1.2.5).
            const auto count = drawnCount;
            placedCount = count;
            const auto newest = columnOf (juce::jmax (0LL, count - 1)) + 1;
            const auto leftColumn = newest - visibleColumns;
            const int ringLeft = (int) (((leftColumn % ringWidth) + ringWidth) % ringWidth);
            const int firstPart = juce::jmin (visibleColumns, ringWidth - ringLeft);
            g.drawImage (ring, 0, 0, firstPart, ringHeight, ringLeft, 0, firstPart, ringHeight);
            if (firstPart < visibleColumns)
                g.drawImage (ring, firstPart, 0, visibleColumns - firstPart, ringHeight, 0, 0, visibleColumns - firstPart, ringHeight);
        }

        g.setColour (tone::black);
        for (const auto& corner : corners)
            g.fillPath (corner);

        if (voicedOnScreen == 0)
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, 15.0f * typeScale));
            g.drawText ("Play a vocal and its pitch shows here", area.toFloat(), juce::Justification::centred, false);
        }
    }

    // The piano changes only with the note, and macOS merges nearby repaints, so it is kept as one image rather than drawn key by key each frame.
    const auto pianoArea = getLocalBounds().removeFromRight (pianoWidth() + juce::roundToInt (3.0f * typeScale));
    if (g.clipRegionIntersects (pianoArea))
    {
        if (pianoStale || pianoScale != pixelScale || ! pianoImage.isValid())
        {
            pianoScale = pixelScale;
            pianoStale = false;
            pianoImage = juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) pianoArea.getWidth() * pixelScale)), juce::jmax (1, juce::roundToInt ((float) pianoArea.getHeight() * pixelScale)), true);
            juce::Graphics ig (pianoImage);
            ig.addTransform (juce::AffineTransform::scale (pixelScale).translated (-(float) pianoArea.getX() * pixelScale, 0.0f));
            paintPiano (ig);
        }

        g.drawImageTransformed (pianoImage, juce::AffineTransform::scale (1.0f / pianoScale).translated ((float) pianoArea.getX(), 0.0f));
    }
}
