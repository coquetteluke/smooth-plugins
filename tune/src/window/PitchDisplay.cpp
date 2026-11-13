#include "PitchDisplay.h"

using namespace ui;

namespace
{
    constexpr int pad = 24;              // the display's inner margin
    constexpr int levelWidth = 5;
    constexpr int tickZone = 26;         // height of the ruler's ticks
    constexpr int traceGap = 10;         // between the ruler and the trace
    constexpr int labelZone = 18;        // the cents labels above them
    constexpr float noteLock = 8.0f;     // within this many cents of the note, the note turns lime
    constexpr double holdNote = 2.5;     // the last note stays, dimmed, this long after the voice stops
    constexpr double textEvery = 0.12;   // numbers change about 8 times a second: faster can't be read

    // Cents to ruler position. More room is given to small amounts, where almost all tuning happens: ticks stay evenly
    // spaced and 25, 50 and 100 cents each land exactly on a tick.
    constexpr float tickFor25 = 15.0f, tickFor50 = 25.0f, tickFor100 = 40.0f;

    juce::String noteName (int note)
    {
        return juce::String (smoothtune::noteNames[note % 12]) + juce::String (note / 12 - 1);
    }

    float levelToHeight (float linear)
    {
        const float db = juce::Decibels::gainToDecibels (linear, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }
}

float PitchDisplay::positionForCents (float cents) noexcept
{
    const float a = juce::jmin (std::abs (cents), 100.0f);
    const float tick = a <= 25.0f ? a / 25.0f * tickFor25
                     : a <= 50.0f ? tickFor25 + (a - 25.0f) / 25.0f * (tickFor50 - tickFor25)
                                  : tickFor50 + (a - 50.0f) / 50.0f * (tickFor100 - tickFor50);
    return 0.5f + (cents < 0.0f ? -tick : tick) / (2.0f * tickFor100);
}

PitchDisplay::PitchDisplay (PluginProcessor& p)
    : processor (p),
      bypassValue (p.apvts.getRawParameterValue ("bypass")),
      transposeValue (p.apvts.getRawParameterValue ("transpose")),
      flexValue (p.apvts.getRawParameterValue ("flex"))
{
    hint = "White is what was sung, lime is what comes out. The middle line is the note.";
    setOpaque (true);
    setInterceptsMouseClicks (true, false);
    seenCount = shownCount = processor.getPitchHistoryCount();
    updateText (PluginProcessor::Readings{});
}

void PitchDisplay::resized()
{
    const auto b = getLocalBounds();
    const int w = b.getWidth(), h = b.getHeight();

    noteSize = juce::jlimit (72.0f, 190.0f, (float) h * 0.345f);
    const int capHeight = juce::roundToInt (noteSize * 0.727f);
    const int noteTop = 20;
    const int noteBottom = noteTop + capHeight;

    const int side = pad + 34;
    const int rulerTop = juce::jmax (noteBottom + 10, 108);   // clear of the readouts too
    const int traceTop = rulerTop + labelZone + tickZone + traceGap;
    const int traceBottom = h - 16;

    noteArea = { 0, noteTop - 8, w, capHeight + 18 };
    noteArea = noteArea.withSizeKeepingCentre (juce::roundToInt (noteSize * 1.9f), noteArea.getHeight());
    leftArea = { pad, 12, noteArea.getX() - pad, noteArea.getBottom() - 12 };
    rightArea = { noteArea.getRight(), 12, w - pad - noteArea.getRight(), noteArea.getBottom() - 12 };
    rulerArea = { side - 8, rulerTop, w - 2 * (side - 8), labelZone + tickZone };
    traceArea = { side, traceTop, w - 2 * side, juce::jmax (20, traceBottom - traceTop) };
    inArea = { pad, rulerTop + labelZone, levelWidth, h - 34 - rulerTop - labelZone };
    outArea = { w - pad - levelWidth, inArea.getY(), levelWidth, inArea.getHeight() };
    tracePoints = juce::roundToInt ((float) traceArea.getWidth() / pointsPerPoint);

    staticScale = ringScale = 0.0f;   // both are rebuilt at the right size the next time they are painted
    repaint();
}

float PitchDisplay::traceY (float cents) const noexcept
{
    return (1.0f - positionForCents (cents)) * (float) traceArea.getHeight();
}

float PitchDisplay::rulerX (float cents) const noexcept
{
    return (float) traceArea.getX() + positionForCents (cents) * (float) traceArea.getWidth();
}

// ---- reading the processor ----------------------------------------------------------------------------------

bool PitchDisplay::advance (double seconds)
{
    // When the host stops calling the plug-in, the last reading is not live any more, even in the middle of a note.
    auto r = processor.getReadings();
    staleFor = processor.getPitchHistoryCount() != seenCount ? 0.0 : staleFor + seconds;
    if (staleFor > 0.25)
    {
        r.voiced = false;
        r.targetNote = -1;
        r.centsOffTarget = r.correctionCents = 0.0f;
    }

    const bool nowBypassed = bypassValue->load (std::memory_order_relaxed) >= 0.5f;
    const float transposeCents = 100.0f * (float) juce::roundToInt (transposeValue->load (std::memory_order_relaxed));
    bool moved = false;

    // The trace moves whenever there is a voiced point in view, and not otherwise.
    const auto count = processor.getPitchHistoryCount();
    for (auto i = juce::jmax (seenCount, count - PluginProcessor::pitchHistorySize + 1); i < count; ++i)
        if (processor.getPitchPoint (i).inputNote.load (std::memory_order_relaxed) > 0.0f)
            lastVoicedPoint = i;
    seenCount = count;

    if (count != shownCount && count - lastVoicedPoint <= (long long) tracePoints + 8)
    {
        shownCount = count;
        repaint (traceArea);
        moved = true;
    }

    const float flexNow = flexValue->load (std::memory_order_relaxed);
    if (std::abs (flexNow - flexDrawn) > 0.01f)
    {
        flexDrawn = flexNow;
        repaint (traceArea);
    }

    // Right now, on the ruler.
    voiced = r.voiced;
    bypassed = nowBypassed;
    noTargets = r.voiced && r.targetNote < 0;
    sungCents = r.voiced ? r.centsOffTarget : 0.0f;
    outCents = sungCents + ((r.voiced && ! nowBypassed && ! noTargets) ? r.correctionCents - transposeCents : 0.0f);

    const float wasSung = shownSung, wasOut = shownOut;
    const float glide = (float) (1.0 - std::exp (-seconds / 0.018));
    shownSung += (sungCents - shownSung) * glide;
    shownOut += (outCents - shownOut) * glide;
    if (std::abs (sungCents - shownSung) < 0.1f) shownSung = sungCents;
    if (std::abs (outCents - shownOut) < 0.1f) shownOut = outCents;

    const bool markersNow = voiced && ! noTargets;
    if (markersNow != markersShown
        || std::abs (rulerX (wasSung) - rulerX (shownSung)) > 0.2f
        || std::abs (rulerX (wasOut) - rulerX (shownOut)) > 0.2f)
    {
        markersShown = markersNow;
        repaint (rulerArea);
        moved = true;
    }

    // The levels. Up at once, down at a readable rate.
    float inNow = 0.0f, outNow = 0.0f;
    processor.takeLevels (inNow, outNow);
    const float fall = (float) (seconds / 1.2);
    const auto step = [fall] (float& shown, float now)
    {
        const float target = levelToHeight (now);
        const float next = target >= shown ? target : juce::jmax (target, shown - fall);
        if (std::abs (next - shown) < 0.006f)   // under a point of bar at the usual size
            return false;
        shown = next;
        return true;
    };
    if (step (inLevel, inNow))  { repaint (inArea); moved = true; }
    if (step (outLevel, outNow)) { repaint (outArea); moved = true; }

    // The note, held dimmed for a moment after the voice stops.
    int noteNow = shownNote;
    if (voiced && r.targetNote >= 0)
    {
        noteNow = r.targetNote;
        heldFor = 0.0;
    }
    else if ((heldFor += seconds) > holdNote)
    {
        noteNow = -1;
    }

    locked = voiced && ! nowBypassed && ! noTargets && std::abs (outCents) <= noteLock;
    const auto colourNow = locked ? tone::lime : (voiced && r.targetNote >= 0 ? tone::text : tone::faint);
    if (noteNow != shownNote || colourNow != noteColour)
    {
        shownNote = noteNow;
        noteColour = colourNow;
        repaint (noteArea);
        repaint (rulerArea);    // the centre tick shares the note's colour
    }

    if ((textTimer += seconds) >= textEvery)
    {
        textTimer = 0.0;
        updateText (r);
    }

    return moved || (voiced && ! noTargets);
}

void PitchDisplay::updateText (const PluginProcessor::Readings& r)
{
    const int transposeNow = juce::roundToInt (transposeValue->load (std::memory_order_relaxed));

    juce::String a, b, c, d;

    if (r.voiced)
    {
        a = juce::String (r.detectedHz, 1) + " Hz";
        const int off = juce::roundToInt (r.centsOffTarget);

        if (r.targetNote < 0)      b = "No notes to tune to";
        else if (std::abs (off) < 3) b = "On the note";
        else                         b = juce::String (std::abs (off)) + (off > 0 ? " cents sharp" : " cents flat");
    }
    else
    {
        a = "No pitch";
    }

    if (bypassed)
    {
        c = "Off";
        d = "Bypassed";
    }
    else
    {
        const int fix = juce::roundToInt (r.voiced ? r.correctionCents - 100.0f * (float) transposeNow : 0.0f);
        c = (fix > 0 ? juce::String ("+") : (fix < 0 ? juce::String::fromUTF8 ("\xe2\x88\x92") : juce::String())) + juce::String (std::abs (fix)) + (std::abs (fix) == 1 ? " cent" : " cents");

        if (r.voiced && r.targetNote >= 0)
        {
            if (fix < 0)                                         d = "Pulling down";
            else if (fix > 0)                                    d = "Pulling up";
            else if (std::abs (r.centsOffTarget) >= 3.0f)        d = "Left as sung";
            else                                                 d = "Nothing to do";
        }

        if (r.voiced && r.targetNote >= 0 && transposeNow != 0 && r.targetNote + transposeNow >= 0)
            d += ", out as " + noteName (r.targetNote + transposeNow);
    }

    if (a != sungValue || b != sungSub)
    {
        sungValue = a;
        sungSub = b;
        repaint (leftArea);
    }

    if (c != fixValue || d != fixSub)
    {
        fixValue = c;
        fixSub = d;
        repaint (rightArea);
    }
}

// ---- cached layers ------------------------------------------------------------------------------------------

void PitchDisplay::rebuildStatic (float scale)
{
    const int w = getWidth(), h = getHeight();
    staticLayer = juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) w * scale)), juce::jmax (1, juce::roundToInt ((float) h * scale)), false);
    staticScale = scale;

    juce::Graphics g (staticLayer);
    g.addTransform (juce::AffineTransform::scale (scale));

    g.fillAll (tone::base);
    g.setColour (tone::well);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 14.0f);

    // Readout names.
    g.setColour (tone::muted);
    g.setFont (font (Weight::regular, 13.0f));
    g.drawText ("Sung", leftArea.withHeight (18).translated (0, 6), juce::Justification::centredLeft, false);
    g.drawText ("Correction", rightArea.withHeight (18).translated (0, 6), juce::Justification::centredRight, false);

    // The ruler's scale.
    const int tickBottom = rulerArea.getBottom();
    g.setFont (font (Weight::regular, 11.5f, true));
    g.setColour (tone::faint);
    for (int cents : { -100, -50, -25, 25, 50, 100 })
        g.drawText ((cents > 0 ? juce::String ("+") : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (cents)), juce::Rectangle<float> (rulerX ((float) cents) - 26.0f, (float) rulerArea.getY(), 52.0f, 16.0f), juce::Justification::centred, false);

    for (int i = 0; i < numTicks; ++i)
    {
        const auto x = (float) traceArea.getX() + (float) traceArea.getWidth() * (float) i / (float) (numTicks - 1);
        const int fromCentre = std::abs (i - (numTicks - 1) / 2);
        const bool major = fromCentre == (int) tickFor25 || fromCentre == (int) tickFor50 || fromCentre == (int) tickFor100;
        const float tall = fromCentre == 0 ? (float) tickZone - 4.0f : (major ? 18.0f : 10.0f);
        g.setColour (tone::text.withAlpha (fromCentre == 0 ? 0.5f : (major ? 0.32f : 0.16f)));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) tickBottom - tall, 2.0f, tall));
    }

    // The two levels, in at the left and out at the right.
    g.setFont (font (Weight::regular, 11.5f));
    g.setColour (tone::faint);
    g.drawText ("In", juce::Rectangle<int> (pad, inArea.getBottom() + 4, 26, 16), juce::Justification::centredLeft, false);
    g.drawText ("Out", juce::Rectangle<int> (getWidth() - pad - 26, inArea.getBottom() + 4, 26, 16), juce::Justification::centredRight, false);
    g.setColour (tone::text.withAlpha (0.08f));
    g.fillRoundedRectangle (inArea.toFloat(), 2.5f);
    g.fillRoundedRectangle (outArea.toFloat(), 2.5f);

    // Old trace sinks into the display from the left. One gradient, drawn once.
    const int fadeWidth = traceArea.getWidth() / 4;
    fadeLayer = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) fadeWidth * scale)), juce::jmax (1, juce::roundToInt ((float) traceArea.getHeight() * scale)), true);
    juce::Graphics f (fadeLayer);
    f.setGradientFill (juce::ColourGradient (tone::well, 0.0f, 0.0f, tone::well.withAlpha (0.0f), (float) fadeLayer.getWidth(), 0.0f, false));
    f.fillAll();
}

void PitchDisplay::prepareTrace (float scale)
{
    const int w = juce::jmax (1, juce::roundToInt ((float) traceArea.getWidth() * scale));
    const int h = juce::jmax (1, juce::roundToInt ((float) traceArea.getHeight() * scale));

    ring = juce::Image (juce::Image::RGB, w, h, false);
    ring.clear (ring.getBounds(), tone::well);
    ringScale = scale;
    ringHead = 0;
    columnsPerPoint = (double) (pointsPerPoint * scale);
    columnCredit = 0.0;
    columnsTotal = 0;
    noteStartColumn = 0;
    noteLabelled = true;
    hadColumn = false;
    spanNote = -1;
    ringDone = juce::jmax (0LL, processor.getPitchHistoryCount() - juce::jmin ((long long) tracePoints, (long long) PluginProcessor::pitchHistorySize - 8));
}

void PitchDisplay::emitColumn (bool voicedHere, float sungY, float outY)
{
    const int column = ringHead;
    ringHead = (ringHead + 1) % ring.getWidth();
    ++columnsTotal;

    const float s = ringScale;
    const auto x = (float) column;
    juce::Graphics g (ring);

    g.setColour (tone::well);
    g.fillRect (column, 0, 1, ring.getHeight());

    if (voicedHere)
    {
        // A line only joins the column before when it stayed close. A leap, such as a note change, leaves a clean break.
        const float joinLimit = 0.3f * (float) ring.getHeight();
        const auto drawLine = [&] (float y, float last, juce::Colour colour, float thick)
        {
            float low = y, high = y;
            if (hadColumn && std::abs (last - y) < joinLimit)
            {
                low = juce::jmin (low, last);
                high = juce::jmax (high, last);
            }
            g.setColour (colour);
            g.fillRect (juce::Rectangle<float> (x, low - thick * 0.5f, 1.0f, high - low + thick));
        };

        if (! bypassed)
        {
            // What was moved: the fill between what was sung and what comes out.
            const float a = juce::jmin (sungY, outY), b = juce::jmax (sungY, outY);
            if (b - a > 0.5f)
            {
                g.setColour (tone::lime.withAlpha (0.15f));
                g.fillRect (juce::Rectangle<float> (x, a, 1.0f, b - a));
            }
        }

        drawLine (sungY, lastSungY, tone::sung.withAlpha (0.9f), 1.5f * s);
        if (! bypassed)
            drawLine (outY, lastOutY, tone::lime, 2.0f * s);

        lastSungY = sungY;
        lastOutY = outY;
        hadColumn = true;
    }
    else
    {
        hadColumn = false;
    }

    // Names each note that lasted long enough to read, lying over its own columns.
    const auto labelFont = font (Weight::regular, 11.5f * s);
    const int labelColumns = juce::roundToInt (22.0f * s);
    if (! noteLabelled && spanNote >= 0 && columnsTotal - noteStartColumn >= labelColumns)
    {
        noteLabelled = true;
        const auto name = noteName (spanNote);
        const auto area = juce::Rectangle<float> (x - (float) labelColumns, 2.0f * s, (float) labelColumns + 6.0f * s, 15.0f * s);
        g.setFont (labelFont);
        g.setColour (tone::muted);
        g.drawText (name, area, juce::Justification::centredLeft, false);
        g.drawText (name, area.translated ((float) ring.getWidth(), 0.0f), juce::Justification::centredLeft, false);
    }
}

void PitchDisplay::writeNewRows()
{
    const auto count = processor.getPitchHistoryCount();
    const float s = ringScale;
    const float transposeNotes = (float) juce::roundToInt (transposeValue->load (std::memory_order_relaxed));

    if (ringDone < count - PluginProcessor::pitchHistorySize + 8)
        ringDone = count - PluginProcessor::pitchHistorySize + 8;

    for (; ringDone < count; ++ringDone)
    {
        const auto& point = processor.getPitchPoint (ringDone);
        const float in = point.inputNote.load (std::memory_order_relaxed);
        const float out = point.outputNote.load (std::memory_order_relaxed);
        const float target = point.targetNote.load (std::memory_order_relaxed);
        const bool voicedHere = in > 0.0f && target >= 0.0f;

        float sungY = 0.0f, outY = 0.0f;
        if (voicedHere)
        {
            const int note = juce::roundToInt (target);
            if (note != spanNote)
            {
                // A new note restarts the scale, so the line breaks instead of slashing across.
                spanNote = note;
                noteStartColumn = columnsTotal;
                noteLabelled = false;
                hadColumn = false;
                lastInPoint = (in - target) * 100.0f;
                lastOutPoint = (out - transposeNotes - target) * 100.0f;
            }

            // A touch of smoothing for the eye only, so tracker noise does not make the line furry.
            sungY = lastInPoint + ((in - target) * 100.0f - lastInPoint) * 0.45f;
            outY = lastOutPoint + ((out - transposeNotes - target) * 100.0f - lastOutPoint) * 0.45f;
        }
        else
        {
            if (spanNote >= 0)
                hadColumn = false;
            spanNote = -1;
        }

        columnCredit += columnsPerPoint;
        const int columns = (int) columnCredit;
        columnCredit -= columns;

        for (int c = 1; c <= columns; ++c)
        {
            const float t = (float) c / (float) columns;
            emitColumn (voicedHere,
                        traceY (lastInPoint + (sungY - lastInPoint) * t) * s,
                        traceY (lastOutPoint + (outY - lastOutPoint) * t) * s);
        }

        if (voicedHere)
        {
            lastInPoint = sungY;
            lastOutPoint = outY;
        }
    }
}

// ---- painting -----------------------------------------------------------------------------------------------

void PitchDisplay::paint (juce::Graphics& g)
{
    ++paintCount;
    const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

    if (std::abs (staticScale - scale) > 0.001f || ! staticLayer.isValid())
        rebuildStatic (scale);

    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.setColour (juce::Colours::black);
    const auto toPoints = juce::AffineTransform::scale (1.0f / scale);
    g.drawImageTransformed (staticLayer, toPoints);

    if (g.clipRegionIntersects (traceArea))
    {
        if (std::abs (ringScale - scale) > 0.001f || ! ring.isValid())
            prepareTrace (scale);

        writeNewRows();

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (traceArea);
        const auto origin = juce::AffineTransform::scale (1.0f / scale).translated ((float) traceArea.getX(), (float) traceArea.getY());
        g.drawImageTransformed (ring, juce::AffineTransform::translation ((float) -ringHead, 0.0f).followedBy (origin));
        if (ringHead > 0)
            g.drawImageTransformed (ring, juce::AffineTransform::translation ((float) (ring.getWidth() - ringHead), 0.0f).followedBy (origin));

        // The band Flex leaves alone, and the note's own line through the middle.
        const float flexCents = flexValue->load (std::memory_order_relaxed);
        if (flexCents > 0.5f && ! bypassed)
        {
            const float a = (float) traceArea.getY() + traceY (flexCents), b = (float) traceArea.getY() + traceY (-flexCents);
            g.setColour (tone::text.withAlpha (0.06f));
            g.fillRect (juce::Rectangle<float> ((float) traceArea.getX(), a, (float) traceArea.getWidth(), b - a));
        }
        g.setColour (tone::text.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> ((float) traceArea.getX(), (float) traceArea.getY() + traceY (0.0f) - 0.5f, (float) traceArea.getWidth(), 1.0f));

        g.setColour (juce::Colours::black);
        g.drawImageTransformed (fadeLayer, toPoints.translated ((float) traceArea.getX(), (float) traceArea.getY()));
    }

    if (g.clipRegionIntersects (rulerArea))
        paintRuler (g);

    if (g.clipRegionIntersects (noteArea))
        paintNote (g);

    if (g.clipRegionIntersects (leftArea))
        paintSung (g);

    if (g.clipRegionIntersects (rightArea))
        paintCorrection (g);

    if (g.clipRegionIntersects (inArea) || g.clipRegionIntersects (outArea))
        paintLevels (g);
}

void PitchDisplay::paintRuler (juce::Graphics& g)
{
    const float bottom = (float) rulerArea.getBottom();
    const int centreTick = (numTicks - 1) / 2;
    const auto tickX = [this] (int i) { return (float) traceArea.getX() + (float) traceArea.getWidth() * (float) i / (float) (numTicks - 1); };

    if (locked)
    {
        g.setColour (tone::lime);
        g.fillRect (juce::Rectangle<float> (tickX (centreTick) - 1.0f, bottom - ((float) tickZone - 4.0f), 2.0f, (float) tickZone - 4.0f));
    }

    if (! markersShown)
        return;

    const float xSung = rulerX (shownSung), xOut = rulerX (shownOut);
    const float from = juce::jmin (xSung, xOut), to = juce::jmax (xSung, xOut);

    if (! bypassed && to - from > 1.0f)
    {
        g.setColour (tone::lime);
        for (int i = 0; i < numTicks; ++i)
        {
            const float x = tickX (i);
            if (x > from && x < to)
                g.fillRect (juce::Rectangle<float> (x - 1.0f, bottom - 16.0f, 2.0f, 16.0f));
        }
    }

    const float tall = (float) tickZone;
    g.setColour (tone::sung);
    g.fillRoundedRectangle (juce::Rectangle<float> (xSung - 1.5f, bottom - tall, 3.0f, tall), 1.5f);

    if (! bypassed)
    {
        g.setColour (tone::lime);
        g.fillRoundedRectangle (juce::Rectangle<float> (xOut - 2.0f, bottom - tall, 4.0f, tall), 2.0f);
    }
}

void PitchDisplay::paintNote (juce::Graphics& g)
{
    const float zero = rulerX (0.0f);

    if (shownNote < 0)
    {
        // No note: a quiet bar holds the note's place, so the display does not look broken at rest.
        g.setColour (tone::faint.withAlpha (0.55f));
        const float width = noteSize * 0.34f, height = noteSize * 0.065f;
        g.fillRoundedRectangle (juce::Rectangle<float> (width, height).withCentre ({ zero, (float) noteArea.getY() + 8.0f + noteSize * 0.727f * 0.5f }), height * 0.5f);
        return;
    }

    const juce::String name (smoothtune::noteNames[shownNote % 12]);
    const juce::String letter = name.substring (0, 1);
    const auto letterFont = font (Weight::medium, noteSize);
    const float letterWidth = juce::GlyphArrangement::getStringWidth (letterFont, letter);
    const int baseline = noteArea.getY() + 8 + juce::roundToInt (noteSize * 0.727f);
    const int x = juce::roundToInt (zero - letterWidth * 0.5f);

    g.setColour (noteColour);
    g.setFont (letterFont);
    g.drawSingleLineText (letter, x, baseline);

    const int sideX = x + juce::roundToInt (letterWidth + noteSize * 0.06f);

    if (name.length() > 1)
    {
        g.setFont (font (Weight::medium, noteSize * 0.40f));
        g.drawSingleLineText ("#", sideX, noteArea.getY() + 8 + juce::roundToInt (noteSize * 0.30f));
    }

    g.setColour (noteColour.withMultipliedAlpha (0.65f));
    g.setFont (font (Weight::regular, noteSize * 0.30f));
    g.drawSingleLineText (juce::String (shownNote / 12 - 1), sideX, baseline);
}

float PitchDisplay::readoutValueSize() const noexcept
{
    return juce::jlimit (30.0f, 44.0f, noteSize * 0.27f);
}

void PitchDisplay::paintSung (juce::Graphics& g)
{
    const float valueSize = readoutValueSize();
    const int valueHeight = juce::roundToInt (valueSize * 1.28f);
    const auto l = leftArea.translated (0, 26);

    g.setColour (voiced ? tone::text : tone::faint);
    g.setFont (font (Weight::medium, valueSize, true));
    g.drawText (sungValue, l.withHeight (valueHeight), juce::Justification::centredLeft, false);
    g.setColour (tone::muted);
    g.setFont (font (Weight::regular, 14.0f));
    g.drawText (sungSub, l.withTrimmedTop (valueHeight + 2).withHeight (20), juce::Justification::centredLeft, false);
}

void PitchDisplay::paintCorrection (juce::Graphics& g)
{
    const float valueSize = readoutValueSize();
    const int valueHeight = juce::roundToInt (valueSize * 1.28f);
    const auto r = rightArea.translated (0, 26);
    const bool fixing = ! bypassed && fixValue != "0 cents";

    g.setColour (fixing ? tone::lime : (voiced && ! bypassed ? tone::text : tone::faint));
    g.setFont (font (Weight::medium, valueSize, true));
    g.drawText (fixValue, r.withHeight (valueHeight), juce::Justification::centredRight, false);
    g.setColour (tone::muted);
    g.setFont (font (Weight::regular, 14.0f));
    g.drawText (fixSub, r.withTrimmedTop (valueHeight + 2).withHeight (20), juce::Justification::centredRight, false);
}

void PitchDisplay::paintLevels (juce::Graphics& g)
{
    const auto bar = [&] (juce::Rectangle<int> area, float level, juce::Colour colour)
    {
        const float height = (float) area.getHeight() * level;
        if (height >= 1.0f)
        {
            g.setColour (colour);
            g.fillRoundedRectangle (area.toFloat().removeFromBottom (height), 2.5f);
        }
    };

    bar (inArea, inLevel, tone::sung.withAlpha (0.8f));
    bar (outArea, outLevel, bypassed ? tone::sung.withAlpha (0.8f) : tone::lime);
}
