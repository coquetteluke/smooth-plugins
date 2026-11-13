#include "SliceViews.h"

using namespace ui;

namespace
{
    bool isBlack (int note) noexcept
    {
        const int n = note % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }

    juce::Image makeLayer (int width, int height, float scale)
    {
        return juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) width * scale)), juce::jmax (1, juce::roundToInt ((float) height * scale)), true);
    }

    juce::String shortSeconds (double s, double step)
    {
        if (step >= 1.0)  return juce::String (juce::roundToInt (s)) + " s";
        if (step >= 0.1)  return juce::String (s, 1) + " s";
        return juce::String (juce::roundToInt (s * 1000.0)) + " ms";
    }

    constexpr float attackShare = 0.26f, decayShare = 0.26f, releaseShare = 0.26f, floorShare = 0.03f;
}

// ---- waveform -----------------------------------------------------------------------------------------------------

bool SliceWaveform::Look::operator== (const Look& o) const
{
    return table == o.table && selected == o.selected && mode == o.mode && width == o.width && height == o.height
        && viewFrom == o.viewFrom && viewTo == o.viewTo && startMs == o.startMs && sample == o.sample
        && bypassed == o.bypassed && zoomed == o.zoomed && level == o.level && pitch == o.pitch && reverse == o.reverse;
}

SliceWaveform::SliceWaveform (PluginProcessor& p) : processor (p)
{
    setOpaque (true);
    setComponentID ("waveform");
    hint = "Click a slice to hear it. Drag a marker to move it. Double-click to add one. Option-click to remove one.";
    startAttachment = std::make_unique<juce::ParameterAttachment> (*p.apvts.getParameter ("slice_start"), [] (float) {}, nullptr);
}

void SliceWaveform::resized()
{
    stale = true;
}

float SliceWaveform::xForFrame (int frame) const
{
    const int span = viewTo - viewFrom;
    return span > 0 ? (float) getWidth() * (float) ((double) (frame - viewFrom) / (double) span) : 0.0f;
}

int SliceWaveform::frameAtX (float x) const
{
    const int frames = sample != nullptr ? sample->frames() : 0;
    const int span = viewTo - viewFrom;
    if (frames == 0 || getWidth() <= 0 || span <= 0) return 0;
    return juce::jlimit (0, frames - 1, viewFrom + (int) ((double) x / getWidth() * span));
}

int SliceWaveform::sliceAtFrame (int frame) const
{
    for (int i = 0; i < table.count; ++i)
        if (frame >= table.start[i] && frame < table.start[i + 1]) return i;
    return -1;
}

int SliceWaveform::markerNearX (float x) const
{
    int best = -1;
    float distance = 5.0f;
    for (int i = 0; i < table.count; ++i)
    {
        const float d = std::abs (xForFrame (table.start[i]) - x);
        if (d <= distance) { distance = d; best = i; }
    }
    return best;
}

float SliceWaveform::startFrame() const
{
    if (sample == nullptr || sample->isEmpty()) return 0.0f;
    const float ms = processor.apvts.getRawParameterValue ("slice_start")->load();
    return juce::jlimit (0.0f, (float) sample->frames(), (float) (ms * 0.001 * sample->rate));
}

juce::Rectangle<float> SliceWaveform::startTab() const
{
    const float x = juce::jlimit (0.0f, (float) getWidth() - 38.0f, xForFrame ((int) startFrame()));
    return { x, (float) getHeight() - 17.0f, 38.0f, 15.0f };
}

bool SliceWaveform::startTabShows() const
{
    if (look.mode == PluginProcessor::modeManual || sample == nullptr || sample->isEmpty())
        return false;
    const float frame = startFrame();
    return (frame >= (float) viewFrom && frame <= (float) viewTo) || viewFrom == 0;
}

juce::Point<float> SliceWaveform::startHandlePosition() const
{
    return startTab().getCentre();
}

double SliceWaveform::timeStepSeconds() const
{
    if (sample == nullptr || sample->isEmpty())
        return 1.0;
    const double seconds = (double) (viewTo - viewFrom) / juce::jmax (1.0, sample->rate);
    const double wanted = seconds / juce::jmax (1.0, (double) getWidth() / 110.0);
    double step = 600.0;   // a sample at the load limit is about 35 minutes
    for (double s : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
        if (s >= wanted) { step = s; break; }
    return step;
}

void SliceWaveform::update (const slicecore::SliceTable& t, int newSelected, bool zoomed)
{
    auto s = processor.getSample();
    table = t;
    selected = newSelected;

    const int frames = s != nullptr ? s->frames() : 0;
    int from = 0, to = frames;
    if (zoomed && juce::isPositiveAndBelow (selected, t.count))
    {
        const int a = t.start[selected], b = t.start[selected + 1];
        const int margin = juce::jmax (32, (b - a) / 16);
        from = juce::jmax (0, a - margin);
        to = juce::jmin (frames, b + margin);
    }

    // The zoomed view holds still under a dragged marker or Start tab, which move the slice it shows. Otherwise the frame under the
    // mouse would move with them and the drag would run away. It settles when the mouse is let go.
    if (zoomed && isDraggingSomething() && s.get() == sample.get() && viewTo > viewFrom)
    {
        from = viewFrom;
        to = viewTo;
    }
    viewFrom = from;
    viewTo = to;

    Look now;
    now.table = t;
    now.selected = selected;
    now.mode = (int) processor.apvts.getRawParameterValue ("slice_mode")->load();
    now.width = getWidth();
    now.height = getHeight();
    now.viewFrom = from;
    now.viewTo = to;
    now.startMs = processor.apvts.getRawParameterValue ("slice_start")->load();
    now.sample = s.get();
    now.bypassed = processor.apvts.getRawParameterValue ("bypass")->load() > 0.5f;
    now.zoomed = zoomed;
    for (int i = 0; i < t.count; ++i)
    {
        now.level[(size_t) i] = processor.apvts.getRawParameterValue (PluginProcessor::slotId (i, "level"))->load();
        now.pitch[(size_t) i] = processor.apvts.getRawParameterValue (PluginProcessor::slotId (i, "pitch"))->load();
        now.reverse[(size_t) i] = processor.apvts.getRawParameterValue (PluginProcessor::slotId (i, "reverse"))->load() > 0.5f;
    }

    sample = s;

    if (! (now == look))
    {
        look = now;
        stale = true;
    }

    if (stale)
        repaint();
}

void SliceWaveform::measureColumns (int columns)
{
    if (sample == nullptr || sample->isEmpty() || columns <= 0)
        return;

    if (columnSample != sample.get())
    {
        largest = 0.01f;
        for (size_t i = 0; i < sample->peakMax.size(); ++i)
            largest = std::max ({ largest, sample->peakMax[i], -sample->peakMin[i] });
    }

    if (columnSample == sample.get() && columnFrom == viewFrom && columnTo == viewTo && columnCount == columns)
        return;

    columnSample = sample.get();
    columnFrom = viewFrom;
    columnTo = viewTo;
    columnCount = columns;
    columnLow.assign ((size_t) columns, 0.0f);
    columnHigh.assign ((size_t) columns, 0.0f);

    const int frames = sample->frames();
    const int bins = (int) sample->peakMax.size();
    const double span = (double) (viewTo - viewFrom);
    const double framesPerColumn = span / (double) columns;
    const double framesPerBin = (double) frames / (double) juce::jmax (1, bins);

    if (framesPerColumn >= framesPerBin && bins > 0)
    {
        for (int c = 0; c < columns; ++c)
        {
            const double a = (double) viewFrom + (double) c * framesPerColumn;
            const int from = juce::jlimit (0, bins - 1, (int) (a / framesPerBin));
            const int to = juce::jlimit (from + 1, bins, (int) ((a + framesPerColumn) / framesPerBin) + 1);
            float lo = 0.0f, hi = 0.0f;
            for (int b = from; b < to; ++b)
            {
                lo = std::min (lo, sample->peakMin[(size_t) b]);
                hi = std::max (hi, sample->peakMax[(size_t) b]);
            }
            columnLow[(size_t) c] = lo;
            columnHigh[(size_t) c] = hi;
        }
        return;
    }

    // Zoomed in further than the overview goes: read the audio. The sample never changes once loaded, so this is safe from here.
    for (int ch = 0; ch < sample->audio.getNumChannels(); ++ch)
    {
        const auto* x = sample->audio.getReadPointer (ch);
        for (int c = 0; c < columns; ++c)
        {
            const int from = juce::jlimit (0, frames - 1, viewFrom + (int) ((double) c * framesPerColumn));
            const int to = juce::jlimit (from + 1, frames, viewFrom + (int) ((double) (c + 1) * framesPerColumn) + 1);
            float lo = columnLow[(size_t) c], hi = columnHigh[(size_t) c];
            for (int i = from; i < to; ++i)
            {
                lo = std::min (lo, x[i]);
                hi = std::max (hi, x[i]);
            }
            columnLow[(size_t) c] = lo;
            columnHigh[(size_t) c] = hi;
        }
    }
}

void SliceWaveform::drawEmpty (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.fillAll (tone::well);

    // A picture of what is about to happen: something cut into coloured pieces.
    const float shape[] = { 0.32f, 0.72f, 0.5f, 0.9f, 0.62f, 0.38f, 0.82f, 0.55f, 0.28f, 0.68f, 0.92f, 0.46f, 0.6f, 0.34f, 0.76f, 0.42f };
    const int pieces = (int) std::size (shape);
    const float width = juce::jmin (b.getWidth() - 80.0f, 620.0f);
    const float maxHeight = juce::jmin (120.0f, b.getHeight() * 0.32f);
    const float pieceWidth = width / (float) pieces;
    const float left = b.getCentreX() - width * 0.5f;
    const float mid = b.getCentreY() - juce::jmin (46.0f, b.getHeight() * 0.18f);

    for (int i = 0; i < pieces; ++i)
    {
        const float h = shape[i] * maxHeight;
        g.setColour (sliceColour (i).withAlpha (0.9f));
        g.fillRoundedRectangle (juce::Rectangle<float> (left + (float) i * pieceWidth + 1.5f, mid - h * 0.5f, pieceWidth - 3.0f, h), 3.0f);
    }

    const float textY = mid + maxHeight * 0.5f + 12.0f;
    g.setColour (tone::text);
    g.setFont (font (Weight::medium, 28.0f));
    g.drawText ("Drop a vocal here", juce::Rectangle<float> (b.getX(), textY, b.getWidth(), 36.0f), juce::Justification::centred, false);
    g.setColour (tone::muted);
    g.setFont (font (Weight::regular, 14.0f));
    g.drawText ("Or click to pick a file. It gets cut at the words and laid out across the keys.",
                juce::Rectangle<float> (b.getX(), textY + 37.0f, b.getWidth(), 20.0f), juce::Justification::centred, false);
}

void SliceWaveform::drawLayer (juce::Graphics& g, float scale, bool lit)
{
    const float w = (float) getWidth(), h = (float) getHeight();
    g.fillAll (tone::well);

    const float strip = (float) stripHeight;
    const float axis = (float) axisHeight;
    const float mid = strip + (h - strip - axis) * 0.5f;
    const float reach = (h - strip - axis) * 0.5f - 4.0f;
    const float peakScale = reach * 0.96f / largest;

    // The slices' grounds.
    for (int i = 0; i < table.count; ++i)
    {
        const float a = xForFrame (table.start[i]), b = xForFrame (table.start[i + 1]);
        if (b < 0.0f || a > w) continue;
        const auto colour = sliceColour (i);
        g.setColour (colour.withAlpha (lit ? 0.22f : (i == selected ? 0.17f : 0.05f)));
        g.fillRect (juce::Rectangle<float> (a, strip, juce::jmax (1.0f, b - a), h - strip));
    }

    // The waveform, one column per physical pixel, each slice in its own colour. A reversed slice is drawn reversed, and Level sets its height.
    const int columns = columnCount;
    const float columnWidth = 1.0f / scale;
    int slice = -1, sliceFromColumn = 0, sliceToColumn = 0;
    float gain = 1.0f;
    bool reversed = false;
    g.setColour (tone::faint.withAlpha (lit ? 0.9f : 0.6f));

    for (int c = 0; c < columns && ! columnLow.empty(); ++c)
    {
        const int frame = viewFrom + (int) ((double) c / (double) columns * (double) (viewTo - viewFrom));

        if (slice < 0 || frame >= sliceToColumn || frame < sliceFromColumn)
        {
            slice = sliceAtFrame (frame);
            if (slice >= 0)
            {
                sliceFromColumn = table.start[slice];
                sliceToColumn = table.start[slice + 1];
                gain = juce::Decibels::decibelsToGain (look.level[(size_t) slice], -70.0f);
                reversed = look.reverse[(size_t) slice];
                const auto colour = sliceColour (slice);
                g.setColour (lit ? sliceColourLit (slice) : (slice == selected ? colour : colour.withMultipliedBrightness (0.88f)));
            }
            else
            {
                sliceFromColumn = 0;
                sliceToColumn = table.count > 0 ? table.start[0] : (viewTo + 1);
                gain = 1.0f;
                reversed = false;
                g.setColour (tone::faint.withAlpha (lit ? 0.9f : 0.6f));
            }
        }

        int read = c;
        if (reversed && slice >= 0)
        {
            const double a = (double) (sliceFromColumn - viewFrom) / (double) (viewTo - viewFrom) * columns;
            const double b = (double) (sliceToColumn - viewFrom) / (double) (viewTo - viewFrom) * columns;
            read = juce::jlimit (0, columns - 1, (int) (a + b) - c - 1);
        }

        const float hi = juce::jmin (reach, columnHigh[(size_t) read] * peakScale * gain);
        const float lo = juce::jmax (-reach, columnLow[(size_t) read] * peakScale * gain);
        const float top = mid - juce::jmax (hi, 0.5f);
        const float bottom = mid - juce::jmin (lo, -0.5f);
        g.fillRect (juce::Rectangle<float> ((float) c * columnWidth, top, columnWidth, bottom - top));
    }

    if (lit)
        return;   // the rest stays out of the lit copy, which only ever shows under the voices' wipes

    // Markers and the numbered strip.
    for (int i = 0; i < table.count; ++i)
    {
        const float a = xForFrame (table.start[i]), b = xForFrame (table.start[i + 1]);
        if (b < 0.0f || a > w) continue;
        const auto colour = sliceColour (i);
        const bool chosen = i == selected;
        const float left = juce::jmax (0.0f, a), right = juce::jmin (w, b);

        if (chosen)
        {
            g.setColour (colour);
            g.fillRect (juce::Rectangle<float> (left, 0.0f, right - left, strip));
        }
        else
        {
            g.setColour (colour);
            g.fillRect (juce::Rectangle<float> (left, 0.0f, right - left, 3.0f));
        }

        g.setColour (colour.withAlpha (chosen ? 0.95f : 0.42f));
        g.fillRect (juce::Rectangle<float> (a, 0.0f, 1.0f, h));

        const float room = right - left;
        juce::String label (i + 1);
        const auto labelFont = font (Weight::medium, 11.5f, true);
        const float numberWidth = juce::GlyphArrangement::getStringWidth (labelFont, label);

        if (room >= numberWidth + 3.0f)
        {
            const float pitchHere = look.pitch[(size_t) i];
            if (room >= 52.0f && std::abs (pitchHere) >= 0.005f)
            {
                const auto rounded = juce::String (std::abs (pitchHere), std::abs (pitchHere - std::round (pitchHere)) < 0.005f ? 0 : 2);
                label << "  " << (pitchHere > 0.0f ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) << rounded;
            }
            const auto ink = chosen ? tone::ink : colour.interpolatedWith (tone::text, 0.25f);
            const float inset = room >= numberWidth + 10.0f ? 5.0f : (room - numberWidth) * 0.5f;
            g.setColour (ink);
            g.setFont (labelFont);
            g.drawText (label, juce::Rectangle<float> (left + inset, chosen ? 0.0f : 3.0f, room - inset, strip - (chosen ? 0.0f : 3.0f)), juce::Justification::centredLeft, false);

            if (look.reverse[(size_t) i] && room >= 34.0f)
            {
                g.setColour (ink);
                juce::Path arrow;
                const float ax = right - 12.0f, ay = strip * 0.5f + (chosen ? 0.0f : 1.5f);
                arrow.startNewSubPath (ax + 6.0f, ay);
                arrow.lineTo (ax, ay);
                arrow.startNewSubPath (ax + 3.0f, ay - 3.0f);
                arrow.lineTo (ax, ay);
                arrow.lineTo (ax + 3.0f, ay + 3.0f);
                g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
    }

    // The time scale.
    const double seconds = (double) (viewTo - viewFrom) / juce::jmax (1.0, sample->rate);
    const double step = timeStepSeconds();

    const double firstSecond = (double) viewFrom / sample->rate;
    g.setFont (font (Weight::regular, 11.0f, true));
    for (double t = std::ceil (firstSecond / step) * step; t < firstSecond + seconds; t += step)
    {
        const float x = (float) ((t - firstSecond) / seconds) * w;
        g.setColour (tone::muted.withAlpha (0.55f));
        g.fillRect (juce::Rectangle<float> (x, h - axis + 3.0f, 1.0f, 5.0f));
        g.setColour (tone::muted);
        g.drawText (shortSeconds (t, step), juce::Rectangle<float> (x + 4.0f, h - axis + 1.0f, 64.0f, 16.0f), juce::Justification::centredLeft, false);
    }

    // Where slicing starts. Manual markers ignore it, so it only shows when it does something.
    if (look.mode != PluginProcessor::modeManual)
    {
        const float x = xForFrame ((int) startFrame());
        if (x > 0.5f)
        {
            g.setColour (tone::well.withAlpha (0.62f));
            g.fillRect (juce::Rectangle<float> (0.0f, strip, juce::jmin (x, w), h - strip));
        }
        if (x > 0.5f && x <= w)
        {
            g.setColour (tone::text);
            g.fillRect (juce::Rectangle<float> (x, strip, 1.0f, h - strip));
        }
        if (startTabShows())
        {
            const auto tab = startTab();
            g.setColour (tone::text);
            g.fillRoundedRectangle (tab, 3.0f);
            g.setColour (tone::ink);
            g.setFont (font (Weight::medium, 10.5f));
            g.drawText ("Start", tab, juce::Justification::centred, false);
        }
    }

    if (look.bypassed)
    {
        g.setColour (tone::well.withAlpha (0.55f));
        g.fillAll();
    }
}

void SliceWaveform::rebuildLayers (float scale)
{
    ++rebuilds;
    layerScale = scale;
    stale = false;
    still = makeLayer (getWidth(), getHeight(), scale);
    litLayer = {};

    juce::Graphics g (still);
    g.addTransform (juce::AffineTransform::scale (scale));

    if (sample == nullptr || sample->isEmpty())
    {
        drawEmpty (g);
        return;
    }

    measureColumns (juce::roundToInt ((float) getWidth() * scale));
    drawLayer (g, scale, false);

    litLayer = makeLayer (getWidth(), getHeight(), scale);
    juce::Graphics lg (litLayer);
    lg.addTransform (juce::AffineTransform::scale (scale));
    drawLayer (lg, scale, true);
}

int SliceWaveform::getVoiceCount() const noexcept
{
    int n = 0;
    for (const auto& w : wipes)
        if (w.on) ++n;
    return n;
}

bool SliceWaveform::updateVoices()
{
    const int h = getHeight();
    bool any = false;
    const bool hasAudio = sample != nullptr && ! sample->isEmpty();

    for (int i = 0; i < PluginProcessor::kPlayheads; ++i)
    {
        auto& w = wipes[(size_t) i];
        const auto v = processor.getPlayheadView (i);
        Wipe now;

        if (hasAudio && v.position >= 0.0f && juce::isPositiveAndBelow (v.slot, table.count))
        {
            any = true;
            const float head = xForFrame (juce::roundToInt ((double) v.position * sample->frames()));
            const bool rev = look.reverse[(size_t) v.slot];
            const float a = xForFrame (table.start[v.slot]), b = xForFrame (table.start[v.slot + 1]);
            now.on = true;
            now.slot = v.slot;
            now.head = head;
            now.left = juce::roundToInt (rev ? juce::jmin (head, b) : a);
            now.right = juce::roundToInt (rev ? b : juce::jmax (a, head));
            now.level = juce::jlimit (0.0f, 1.0f, std::sqrt (juce::jmax (0.0f, v.level)));
        }

        // Nothing moved by more than a quarter of a pixel, nothing got much louder or quieter: leave it alone.
        const bool moved = now.on != w.on || std::abs (now.head - w.head) > 0.25f || now.left != w.left || now.right != w.right
                        || std::abs (now.level - w.level) > 0.04f;
        if (! moved)
            continue;

        if (w.on)
            repaint (juce::jmax (0, w.left - 1), stripHeight, juce::jmin (getWidth(), w.right + 2) - juce::jmax (0, w.left - 1), h - stripHeight);
        if (now.on)
            repaint (juce::jmax (0, now.left - 1), stripHeight, juce::jmin (getWidth(), now.right + 2) - juce::jmax (0, now.left - 1), h - stripHeight);
        w = now;
    }

    return any;
}

void SliceWaveform::paint (juce::Graphics& g)
{
    const double started = juce::Time::getMillisecondCounterHiRes();
    const float scale = (float) g.getInternalContext().getPhysicalPixelScaleFactor();

    if (stale || scale != layerScale || still.isNull())
        rebuildLayers (scale);

    const auto whole = getLocalBounds().toFloat();
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.setOpacity (1.0f);
    g.drawImage (still, whole);

    // What each voice has played so far shows in the lit copy, as bright as the voice is loud.
    if (litLayer.isValid())
    {
        for (const auto& w : wipes)
        {
            if (! w.on || w.right <= w.left || w.level < 0.02f)
                continue;
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (w.left, stripHeight, w.right - w.left, getHeight() - stripHeight - axisHeight);
            g.setOpacity (w.level);
            g.drawImage (litLayer, whole);
        }

        for (const auto& w : wipes)
        {
            if (! w.on || w.level < 0.02f)
                continue;
            g.setColour (juce::Colours::white.withAlpha (juce::jmax (0.35f, w.level)));
            g.fillRect (juce::Rectangle<float> (w.head - 0.5f, (float) stripHeight, 1.5f, (float) (getHeight() - stripHeight - axisHeight)));
        }
    }

    if (hoverMarker >= 0 && hoverMarker < table.count)
    {
        g.setColour (tone::text);
        g.fillRect (juce::Rectangle<float> (xForFrame (table.start[hoverMarker]) - 0.5f, 0.0f, 2.0f, (float) getHeight()));
    }

    if (dropActive)
    {
        g.setColour (tone::well.withAlpha (0.82f));
        g.fillAll();
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 24.0f));
        g.drawText ("Drop to load this file", getLocalBounds(), juce::Justification::centred, false);
    }

    ++paints;
    paintSeconds += (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    paintedArea += (double) g.getClipBounds().getWidth() * g.getClipBounds().getHeight();
}

void SliceWaveform::setDropActive (bool active)
{
    if (active != dropActive)
    {
        dropActive = active;
        repaint();
    }
}

void SliceWaveform::mouseDown (const juce::MouseEvent& e)
{
    dragging = -1;
    draggingStart = false;

    if (sample == nullptr || sample->isEmpty())
    {
        if (onEmptyClick != nullptr)
            onEmptyClick();
        return;
    }

    if (startTabShows() && startTab().expanded (2.0f).contains (e.position))
    {
        draggingStart = true;
        startAttachment->beginGesture();
        return;
    }

    const int marker = markerNearX ((float) e.x);
    if (marker >= 0 && (e.mods.isAltDown() || e.mods.isPopupMenu()))
    {
        processor.removeMarker (marker);
        return;
    }
    if (marker >= 0)
    {
        dragging = marker;
        return;
    }
    const int slice = sliceAtFrame (frameAtX ((float) e.x));
    if (slice >= 0)
    {
        processor.selectSlice (slice);
        processor.audition (slice);
    }
}

void SliceWaveform::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingStart)
    {
        const double ms = (double) frameAtX (e.position.x) / sample->rate * 1000.0;
        startAttachment->setValueAsPartOfGesture ((float) juce::jmin (60000.0, ms));
    }
    else if (dragging >= 0 && e.getDistanceFromDragStart() > 1)
        processor.moveMarker (dragging, frameAtX ((float) e.x));
}

void SliceWaveform::mouseUp (const juce::MouseEvent&)
{
    if (draggingStart)
        startAttachment->endGesture();
    draggingStart = false;
    dragging = -1;
}

void SliceWaveform::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (sample == nullptr || sample->isEmpty())
        return;

    if (startTabShows() && startTab().expanded (2.0f).contains (e.position))
    {
        startAttachment->setValueAsCompleteGesture (0.0f);
        return;
    }
    if (markerNearX ((float) e.x) < 0)
        processor.addMarker (frameAtX ((float) e.x));
}

void SliceWaveform::mouseMove (const juce::MouseEvent& e)
{
    const bool onStart = startTabShows() && startTab().expanded (2.0f).contains (e.position);
    const int marker = onStart ? -1 : markerNearX ((float) e.x);
    setMouseCursor (onStart || marker >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                    : (sample == nullptr || sample->isEmpty() ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));

    if (marker != hoverMarker)
    {
        hoverMarker = marker;
        repaint();
    }
}

void SliceWaveform::mouseExit (const juce::MouseEvent&)
{
    if (hoverMarker >= 0)
    {
        hoverMarker = -1;
        repaint();
    }
}

// ---- ribbons and keys ---------------------------------------------------------------------------------------------

KeyMap::KeyMap (PluginProcessor& p, SliceWaveform& w) : processor (p), waveform (w)
{
    setOpaque (true);
    setComponentID ("keys");
    hint = "Click a key to hear its slice. Drag the Root tab to move every slice up or down the keyboard.";
    rootAttachment = std::make_unique<juce::ParameterAttachment> (*p.apvts.getParameter ("root_note"), [] (float) {}, nullptr);
}

void KeyMap::resized()
{
    stale = true;
    placeKeys();
}

void KeyMap::chooseRange (int newRoot, int count, bool isChromatic)
{
    const int octaveStart = newRoot - newRoot % 12;
    int first, last;

    if (isChromatic)
    {
        first = octaveStart - 24;
        last = first + 60;
    }
    else
    {
        // As few keys as show every slice, so the ribbons run nearly straight down. A few slices sit mid keyboard, with room around them.
        const int needed = newRoot + juce::jmax (1, count) - 1;
        const int top = needed + (12 - needed % 12) % 12;
        first = octaveStart;
        last = juce::jmax (first + 24, top);
        if (last - first <= 24 && count <= 12)
        {
            first -= 12;
            last = first + 36;
        }
    }

    if (first < 0)
    {
        last -= first;
        first = 0;
    }
    if (last > 127)
    {
        first = juce::jmax (0, first - (last - 127));
        first -= first % 12;
        last = 127;
    }
    firstKey = first;
    lastKey = last;
}

void KeyMap::placeKeys()
{
    int whites = 0;
    for (int n = firstKey; n <= lastKey; ++n)
        if (! isBlack (n)) ++whites;

    const float top = (float) ribbonHeight, height = (float) getHeight() - top;
    const float whiteWidth = (float) getWidth() / (float) juce::jmax (1, whites);
    const float blackWidth = whiteWidth * 0.6f, blackHeight = height * 0.6f;

    int white = 0;
    for (int n = firstKey; n <= lastKey; ++n)
    {
        auto& k = keys[(size_t) n];
        k.black = isBlack (n);
        if (k.black)
            k.face = { (float) white * whiteWidth - blackWidth * 0.5f, top, blackWidth, blackHeight };
        else
            k.face = { (float) white++ * whiteWidth, top, whiteWidth, height };
    }

    const float bottomOfBlacks = top + blackHeight;
    for (int n = firstKey; n <= lastKey; ++n)
    {
        auto& k = keys[(size_t) n];
        k.lower = {};
        if (k.black)
        {
            k.upper = k.face.getSmallestIntegerContainer();
            continue;
        }

        // The part of a white key between its black neighbours, and the whole width under them.
        float left = k.face.getX(), right = k.face.getRight();
        if (n > firstKey && isBlack (n - 1)) left = keys[(size_t) n - 1].face.getRight();
        if (n < lastKey && isBlack (n + 1))  right = keys[(size_t) n + 1].face.getX();
        k.upper = juce::Rectangle<float> (left, top, right - left, bottomOfBlacks - top).toNearestInt();
        k.lower = juce::Rectangle<float> (k.face.getX(), bottomOfBlacks, k.face.getWidth(), k.face.getBottom() - bottomOfBlacks).toNearestInt();
    }
}

int KeyMap::slotForNote (int note) const noexcept
{
    const int slot = note - root;
    return juce::isPositiveAndBelow (slot, table.count) ? slot : -1;
}

juce::Rectangle<float> KeyMap::rootTab() const
{
    const auto face = keys[(size_t) juce::jlimit (firstKey, lastKey, root)].face;
    return juce::Rectangle<float> (40.0f, 15.0f).withCentre ({ juce::jlimit (20.0f, (float) getWidth() - 20.0f, face.getCentreX()), (float) ribbonHeight - 8.0f });
}

juce::Point<float> KeyMap::rootTabCentre() const
{
    return rootTab().getCentre();
}

juce::Point<float> KeyMap::keyCentre (int note) const
{
    const auto face = keys[(size_t) juce::jlimit (firstKey, lastKey, note)].face;
    return { face.getCentreX(), face.getBottom() - (isBlack (note) ? 6.0f : 10.0f) };
}

int KeyMap::noteAt (juce::Point<float> p) const
{
    if (p.y < (float) ribbonHeight)
        return -1;
    for (int n = firstKey; n <= lastKey; ++n)
        if (keys[(size_t) n].black && keys[(size_t) n].face.contains (p)) return n;
    for (int n = firstKey; n <= lastKey; ++n)
        if (! keys[(size_t) n].black && keys[(size_t) n].face.contains (p)) return n;
    return -1;
}

void KeyMap::update (const slicecore::SliceTable& t, int newSelected)
{
    const int newRoot = (int) processor.apvts.getRawParameterValue ("root_note")->load();
    const bool newChromatic = (int) processor.apvts.getRawParameterValue ("key_mode")->load() == PluginProcessor::keyChromatic;
    auto s = processor.getSample();
    const bool sampleThere = s != nullptr && ! s->isEmpty();
    const int from = waveform.getViewFrom(), to = waveform.getViewTo();
    const bool nowBypassed = processor.apvts.getRawParameterValue ("bypass")->load() > 0.5f;

    if (t == table && newSelected == selected && newRoot == root && newChromatic == chromatic && sampleThere == hasSample && from == viewFrom && to == viewTo
        && nowBypassed == bypassed && ! stale && ! needsRange)
        return;

    bypassed = nowBypassed;

    table = t;
    selected = newSelected;
    root = newRoot;
    chromatic = newChromatic;
    hasSample = sampleThere;
    viewFrom = from;
    viewTo = to;

    // The keys hold still under a dragged Root tab, or the key under the mouse would run away from it.
    if (! draggingRoot)
    {
        chooseRange (root, t.count, chromatic);
        placeKeys();
        needsRange = false;
    }

    stale = true;
    repaint();
}

juce::Colour KeyMap::keyColour (int note, bool litVersion) const
{
    const bool black = isBlack (note);

    if (! hasSample)
        return black ? tone::keyTop : tone::key;

    if (chromatic)
    {
        const int slot = juce::jlimit (0, juce::jmax (0, table.count - 1), selected);
        const auto colour = sliceColour (slot);
        if (litVersion) return sliceColourLit (slot);
        if (note == root) return colour;
        return black ? colour.withRotatedHue (-0.025f).withMultipliedBrightness (0.56f) : colour.withMultipliedBrightness (0.78f);
    }

    const int slot = slotForNote (note);
    if (slot < 0)
        return litVersion ? tone::text.withAlpha (0.6f) : (black ? tone::keyTop : tone::key);

    if (litVersion)
        return sliceColourLit (slot);

    const auto colour = sliceColour (slot);
    if (black)   // darker, and turned towards red as it darkens, so yellows go amber and not olive
        return colour.withRotatedHue (-0.025f).withMultipliedBrightness (slot == selected ? 0.86f : 0.66f);
    return slot == selected ? colour : colour.withMultipliedBrightness (0.92f);
}

void KeyMap::drawLayer (juce::Graphics& g, float, bool litVersion)
{
    g.fillAll (litVersion ? tone::base : tone::base);

    const float top = (float) ribbonHeight;

    if (! litVersion)
    {
        const float h = (float) ribbonHeight;

        // The ribbons: each slice flows from where it sits on the waveform down to its key.
        const auto ribbon = [&] (int slot, float x0, float x1, float kx0, float kx1, float alpha)
        {
            juce::Path p;
            const float bend = h * 0.55f;
            p.startNewSubPath (x0, 0.0f);
            p.lineTo (x1, 0.0f);
            p.cubicTo (x1, bend, kx1, h - bend, kx1, h);
            p.lineTo (kx0, h);
            p.cubicTo (kx0, h - bend, x0, bend, x0, 0.0f);
            p.closeSubPath();
            ribbons[(size_t) slot] = p;
            // Dim by the waveform and full by the key, drawn once here. A ribbon reads as the slice arriving at its key.
            const auto colour = sliceColour (slot);
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (alpha * 0.25f), 0.0f, 0.0f, colour.withAlpha (juce::jmin (1.0f, alpha * 1.5f)), 0.0f, h, false));
            g.fillPath (p);
        };

        for (auto& r : ribbons)
            r.clear();

        const float w = (float) getWidth();

        if (hasSample && chromatic && table.count > 0)
        {
            const int slot = juce::jlimit (0, table.count - 1, selected);
            const float x0 = juce::jlimit (0.0f, w, waveform.xForFrame (table.start[slot]));
            const float x1 = juce::jlimit (0.0f, w, waveform.xForFrame (table.start[slot + 1]));
            ribbon (slot, x0, x1, 0.0f, w, 0.5f);
        }
        else if (hasSample)
        {
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < table.count; ++i)
                {
                    if ((i == selected) != (pass == 1)) continue;
                    const int note = root + i;
                    if (note < firstKey || note > lastKey) continue;
                    const auto face = keys[(size_t) note].face;
                    const float x0 = juce::jlimit (0.0f, w, waveform.xForFrame (table.start[i]));
                    const float x1 = juce::jlimit (0.0f, w, waveform.xForFrame (table.start[i + 1]));
                    if (x1 - x0 < 0.25f && (x0 <= 0.0f || x0 >= w)) continue;
                    ribbon (i, x0, x1, face.getX() + 0.5f, face.getRight() - 0.5f, i == selected ? 0.8f : 0.46f);
                }
        }
    }

    // White keys, then black ones over them, with a gap of the window's own colour between them.
    for (int pass = 0; pass < 2; ++pass)
        for (int n = firstKey; n <= lastKey; ++n)
        {
            const auto& k = keys[(size_t) n];
            if (k.black != (pass == 1)) continue;

            auto face = k.face;
            if (k.black)
            {
                g.setColour (tone::base);
                g.fillRect (face.expanded (1.0f, 0.0f).withTrimmedBottom (-1.0f));
            }
            else
                face = face.withTrimmedRight (1.0f);

            g.setColour (keyColour (n, litVersion));
            if (k.black)
                g.fillRoundedRectangle (face.withTrimmedBottom (0.0f), 2.0f);
            else
                g.fillRoundedRectangle (face, 3.0f);
        }

    if (litVersion)
        return;

    // Numbers on the keys, note names on the Cs.
    for (int n = firstKey; n <= lastKey; ++n)
    {
        const auto& k = keys[(size_t) n];
        const int slot = hasSample && ! chromatic ? slotForNote (n) : -1;
        const auto face = k.face.withTrimmedRight (k.black ? 0.0f : 1.0f);

        if (slot >= 0 && face.getWidth() >= 13.0f)
        {
            g.setColour (k.black ? tone::text : tone::ink);
            g.setFont (font (Weight::medium, face.getWidth() >= 24.0f ? 11.5f : 9.5f, true));
            g.drawText (juce::String (slot + 1), face.withTrimmedBottom (k.black ? 5.0f : 7.0f), juce::Justification::centredBottom, false);
        }

        if (slot < 0 && ! k.black && n % 12 == 0 && ! (hasSample && chromatic))
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, 10.5f));
            g.drawText (juce::MidiMessage::getMidiNoteName (n, true, true, 3), face.withTrimmedBottom (5.0f), juce::Justification::centredBottom, false);
        }

        if (slot == selected && slot >= 0)
        {
            g.setColour (tone::text);
            g.fillRoundedRectangle (juce::Rectangle<float> (face.getX() + 2.0f, face.getBottom() - 4.0f, face.getWidth() - 4.0f, 3.0f), 1.5f);
        }
    }

    if (bypassed)
    {
        g.setColour (tone::base.withAlpha (0.55f));
        g.fillAll();
    }
}

void KeyMap::drawRootTab (juce::Graphics& g)
{
    const auto tab = rootTab();
    const float top = (float) ribbonHeight;
    g.setColour (hoverRoot || draggingRoot ? juce::Colours::white : tone::text);
    g.fillRoundedRectangle (tab, 3.0f);
    juce::Path point;
    const float cx = keys[(size_t) juce::jlimit (firstKey, lastKey, root)].face.getCentreX();
    point.addTriangle (cx - 4.0f, tab.getBottom() - 0.5f, cx + 4.0f, tab.getBottom() - 0.5f, cx, top);
    g.fillPath (point);
    g.setColour (tone::ink);
    g.setFont (font (Weight::medium, 10.5f));
    g.drawText ("Root", tab, juce::Justification::centred, false);
}

void KeyMap::rebuildLayers (float scale)
{
    layerScale = scale;
    stale = false;
    still = makeLayer (getWidth(), getHeight(), scale);
    {
        juce::Graphics g (still);
        g.addTransform (juce::AffineTransform::scale (scale));
        drawLayer (g, scale, false);
    }
    litLayer = makeLayer (getWidth(), getHeight(), scale);
    {
        juce::Graphics g (litLayer);
        g.addTransform (juce::AffineTransform::scale (scale));
        drawLayer (g, scale, true);
    }
}

void KeyMap::updateVoices()
{
    std::array<std::uint8_t, 128> nowLit {};
    std::array<std::uint8_t, PluginProcessor::kSlots> nowSlots {};

    for (int i = 0; i < PluginProcessor::kPlayheads; ++i)
    {
        const auto v = processor.getPlayheadView (i);
        if (v.position < 0.0f)
            continue;

        const auto step = (std::uint8_t) juce::jlimit (1, 4, 1 + (int) (std::sqrt (juce::jmax (0.0f, v.level)) * 3.99f));
        const int note = v.note >= 0 ? v.note : (chromatic ? root : root + v.slot);   // the window's own preview has no note of its own

        if (juce::isPositiveAndBelow (note, 128))
            nowLit[(size_t) note] = juce::jmax (nowLit[(size_t) note], step);
        if (juce::isPositiveAndBelow (v.slot, PluginProcessor::kSlots))
            nowSlots[(size_t) v.slot] = juce::jmax (nowSlots[(size_t) v.slot], step);
    }

    for (int n = firstKey; n <= lastKey; ++n)
        if (nowLit[(size_t) n] != lit[(size_t) n])
        {
            const auto& k = keys[(size_t) n];
            repaint (k.face.getSmallestIntegerContainer());
            lit[(size_t) n] = nowLit[(size_t) n];
        }

    for (int s = 0; s < PluginProcessor::kSlots; ++s)
        if (nowSlots[(size_t) s] != litSlots[(size_t) s])
        {
            repaint (ribbons[(size_t) s].getBounds().getSmallestIntegerContainer().withHeight (ribbonHeight));
            litSlots[(size_t) s] = nowSlots[(size_t) s];
        }
}

void KeyMap::paint (juce::Graphics& g)
{
    const float scale = (float) g.getInternalContext().getPhysicalPixelScaleFactor();

    if (stale || scale != layerScale || still.isNull())
        rebuildLayers (scale);

    const auto whole = getLocalBounds().toFloat();
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.setOpacity (1.0f);
    g.drawImage (still, whole);

    // A ribbon only redraws when a voice starts, stops or steps in level, not every frame.
    for (int s = 0; s < PluginProcessor::kSlots; ++s)
        if (litSlots[(size_t) s] > 0 && ! ribbons[(size_t) s].isEmpty())
        {
            g.setColour (sliceColourLit (s).withAlpha ((chromatic ? 0.12f : 0.2f) * (float) litSlots[(size_t) s]));
            g.fillPath (ribbons[(size_t) s]);
        }

    if (g.clipRegionIntersects (rootTab().getSmallestIntegerContainer().expanded (1)))
        drawRootTab (g);

    for (int n = firstKey; n <= lastKey; ++n)
    {
        if (lit[(size_t) n] == 0)
            continue;
        const auto& k = keys[(size_t) n];
        juce::Graphics::ScopedSaveState save (g);
        juce::RectangleList<int> parts (k.upper);
        if (! k.lower.isEmpty())
            parts.addWithoutMerging (k.lower);
        g.reduceClipRegion (parts);
        g.setOpacity (0.25f * (float) lit[(size_t) n]);
        g.drawImage (litLayer, whole);
    }
}

void KeyMap::mouseDown (const juce::MouseEvent& e)
{
    if (rootTab().expanded (3.0f).contains (e.position))
    {
        draggingRoot = true;
        rootAttachment->beginGesture();
        return;
    }

    const int note = noteAt (e.position);
    if (note < 0 || ! hasSample)
        return;

    if (chromatic)
    {
        // Each key plays the selected slice at its own pitch, as MIDI does, and that key lights.
        if (table.count > 0)
            processor.audition (juce::jlimit (0, table.count - 1, selected), note - root);
        return;
    }

    const int slot = slotForNote (note);
    if (slot >= 0)
    {
        processor.selectSlice (slot);
        processor.audition (slot);
    }
}

void KeyMap::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingRoot)
        return;

    auto at = e.position;
    at.y = (float) ribbonHeight + 2.0f;
    int note = noteAt (at);
    if (note < 0)
        note = e.position.x < 0.0f ? firstKey : lastKey;
    rootAttachment->setValueAsPartOfGesture ((float) note);
}

void KeyMap::mouseUp (const juce::MouseEvent&)
{
    if (draggingRoot)
    {
        rootAttachment->endGesture();
        draggingRoot = false;
        stale = true;
        needsRange = true;   // the keys were held still for the drag; they settle on the next tick, even if a paint comes first
        repaint();
    }
}

void KeyMap::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (rootTab().expanded (3.0f).contains (e.position))
        rootAttachment->setValueAsCompleteGesture (60.0f);
}

void KeyMap::mouseMove (const juce::MouseEvent& e)
{
    const bool on = rootTab().expanded (3.0f).contains (e.position);
    setMouseCursor (on ? juce::MouseCursor::LeftRightResizeCursor : (noteAt (e.position) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
    if (on != hoverRoot)
    {
        hoverRoot = on;
        repaint (rootTab().getSmallestIntegerContainer().expanded (1));
    }
}

void KeyMap::mouseExit (const juce::MouseEvent&)
{
    if (hoverRoot)
    {
        hoverRoot = false;
        repaint (rootTab().getSmallestIntegerContainer().expanded (1));
    }
}

// ---- envelope graphs ----------------------------------------------------------------------------------------------

void EnvelopeGraph::setAccent (juce::Colour c)
{
    if (c != accent)
    {
        accent = c;
        repaint();
    }
}

void EnvelopeGraph::bindLink (Link& link, juce::RangedAudioParameter& p)
{
    link.attachment.reset();
    link.param = &p;
    link.attachment = std::make_unique<juce::ParameterAttachment> (p, [this] (float) { repaint(); }, nullptr);
    repaint();
}

void EnvelopeGraph::setPlain (Link& link, float plainValue)
{
    link.attachment->setValueAsPartOfGesture (plainValue);
}

void EnvelopeGraph::setNorm (Link& link, float normalised)
{
    link.attachment->setValueAsPartOfGesture (link.param->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised)));
}

void EnvelopeGraph::reset (Link& link)
{
    link.attachment->setValueAsCompleteGesture (link.param->convertFrom0to1 (link.param->getDefaultValue()));
}

void EnvelopeGraph::mouseDown (const juce::MouseEvent& e)
{
    grabbed = -1;
    float best = 13.0f;
    for (int i = 0; i < numHandles(); ++i)
    {
        const float d = handlePosition (i).getDistanceFrom (e.position);
        if (d < best) { best = d; grabbed = i; }
    }

    if (grabbed >= 0)
    {
        Link* a = nullptr; Link* b = nullptr;
        linksFor (grabbed, a, b);
        if (a != nullptr) a->attachment->beginGesture();
        if (b != nullptr) b->attachment->beginGesture();
    }
}

void EnvelopeGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (grabbed >= 0)
        dragHandle (grabbed, e.position);
}

void EnvelopeGraph::mouseUp (const juce::MouseEvent&)
{
    if (grabbed >= 0)
    {
        Link* a = nullptr; Link* b = nullptr;
        linksFor (grabbed, a, b);
        if (a != nullptr) a->attachment->endGesture();
        if (b != nullptr) b->attachment->endGesture();
    }
    grabbed = -1;
    repaint();
}

void EnvelopeGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (int i = 0; i < numHandles(); ++i)
        if (handlePosition (i).getDistanceFrom (e.position) < 13.0f)
        {
            Link* a = nullptr; Link* b = nullptr;
            linksFor (i, a, b);
            if (a != nullptr) reset (*a);
            if (b != nullptr) reset (*b);
            return;
        }
}

void EnvelopeGraph::mouseMove (const juce::MouseEvent& e)
{
    int now = -1;
    float best = 13.0f;
    for (int i = 0; i < numHandles(); ++i)
    {
        const float d = handlePosition (i).getDistanceFrom (e.position);
        if (d < best) { best = d; now = i; }
    }
    setMouseCursor (now >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    if (now != hovered)
    {
        hovered = now;
        repaint();
    }
}

void EnvelopeGraph::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0)
    {
        hovered = -1;
        repaint();
    }
}

void EnvelopeGraph::drawHandles (juce::Graphics& g)
{
    for (int i = 0; i < numHandles(); ++i)
    {
        const float r = i == hovered || i == grabbed ? 5.5f : 4.0f;
        const auto c = handlePosition (i);
        g.setColour (tone::base);
        g.fillEllipse (juce::Rectangle<float> (2.0f * (r + 1.5f), 2.0f * (r + 1.5f)).withCentre (c));
        g.setColour (tone::text);
        g.fillEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (c));
    }
}

AmpGraph::AmpGraph (juce::AudioProcessorValueTreeState& s) : state (s)
{
    setComponentID ("amp_graph");
    hint = "Drag the dots. Attack and Release belong to the selected slice. Decay and Sustain apply to every slice.";
    bindLink (decay, *s.getParameter ("decay"));
    bindLink (sustain, *s.getParameter ("sustain"));
    bindSlot (0);
}

void AmpGraph::bindSlot (int slot)
{
    bindLink (attack, *state.getParameter (PluginProcessor::slotId (slot, "attack")));
    bindLink (release, *state.getParameter (PluginProcessor::slotId (slot, "release")));
}

float AmpGraph::attackEnd() const
{
    const auto p = plot();
    return p.getX() + p.getWidth() * (floorShare + attackShare * attack.norm());
}

float AmpGraph::decayEnd() const
{
    const auto p = plot();
    return attackEnd() + p.getWidth() * (floorShare + decayShare * decay.norm());
}

float AmpGraph::releaseStart() const
{
    const auto p = plot();
    return p.getRight() - p.getWidth() * (floorShare + releaseShare * release.norm());
}

juce::Point<float> AmpGraph::handlePosition (int i) const
{
    const auto p = plot();
    const float level = p.getBottom() - p.getHeight() * sustain.norm();
    if (i == 0) return { attackEnd(), p.getY() };
    if (i == 1) return { decayEnd(), level };
    return { releaseStart(), level };
}

void AmpGraph::linksFor (int h, Link*& first, Link*& second)
{
    first = h == 0 ? &attack : (h == 1 ? &decay : &release);
    second = h == 1 ? &sustain : nullptr;
}

void AmpGraph::dragHandle (int h, juce::Point<float> at)
{
    const auto p = plot();
    if (h == 0)
        setNorm (attack, ((at.x - p.getX()) / p.getWidth() - floorShare) / attackShare);
    else if (h == 1)
    {
        setNorm (decay, ((at.x - attackEnd()) / p.getWidth() - floorShare) / decayShare);
        setNorm (sustain, (p.getBottom() - at.y) / p.getHeight());
    }
    else
        setNorm (release, ((p.getRight() - at.x) / p.getWidth() - floorShare) / releaseShare);
}

void AmpGraph::paint (juce::Graphics& g)
{
    const auto p = plot();
    const float level = p.getBottom() - p.getHeight() * sustain.norm();

    juce::Path curve;
    curve.startNewSubPath (p.getX(), p.getBottom());
    curve.lineTo (attackEnd(), p.getY());
    curve.lineTo (decayEnd(), level);
    curve.lineTo (juce::jmax (decayEnd(), releaseStart()), level);
    curve.lineTo (p.getRight(), p.getBottom());

    auto fill = curve;
    fill.closeSubPath();
    g.setColour (accent.withAlpha (0.2f));
    g.fillPath (fill);
    g.setColour (accent);
    g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (tone::text.withAlpha (0.13f));
    g.fillRect (juce::Rectangle<float> (p.getX(), p.getBottom(), p.getWidth(), 1.0f));

    drawHandles (g);
}

PitchGraph::PitchGraph (juce::AudioProcessorValueTreeState& s)
{
    setComponentID ("pitch_graph");
    hint = "Each note starts this far off pitch and glides back. Drag up or down for how far, and across for how long.";
    bindLink (amount, *s.getParameter ("penv_amount"));
    bindLink (time, *s.getParameter ("penv_time"));
}

float PitchGraph::yFor (float semis) const
{
    const auto p = plot();
    return p.getCentreY() - p.getHeight() * 0.5f * juce::jlimit (-1.0f, 1.0f, semis / 24.0f);
}

juce::Point<float> PitchGraph::handlePosition (int i) const
{
    const auto p = plot();
    if (i == 0) return { p.getX() + 5.0f, yFor (amount.plain()) };
    return { p.getX() + 5.0f + (p.getWidth() - 10.0f) * time.norm(), p.getCentreY() };
}

void PitchGraph::linksFor (int h, Link*& first, Link*& second)
{
    first = h == 0 ? &amount : &time;
    second = nullptr;
}

void PitchGraph::dragHandle (int h, juce::Point<float> at)
{
    const auto p = plot();
    if (h == 0)
        setPlain (amount, juce::jlimit (-24.0f, 24.0f, (p.getCentreY() - at.y) / (p.getHeight() * 0.5f) * 24.0f));
    else
        setNorm (time, (at.x - p.getX() - 5.0f) / (p.getWidth() - 10.0f));
}

void PitchGraph::paint (juce::Graphics& g)
{
    const auto p = plot();
    const auto start = handlePosition (0), end = handlePosition (1);

    g.setColour (tone::text.withAlpha (0.13f));
    g.fillRect (juce::Rectangle<float> (p.getX(), p.getCentreY() - 0.5f, p.getWidth(), 1.0f));

    juce::Path fill;
    fill.startNewSubPath (start.x, p.getCentreY());
    fill.lineTo (start);
    fill.lineTo (end);
    fill.closeSubPath();
    g.setColour (tone::text.withAlpha (0.14f));
    g.fillPath (fill);

    juce::Path curve;
    curve.startNewSubPath (start);
    curve.lineTo (end);
    curve.lineTo (p.getRight(), p.getCentreY());
    g.setColour (tone::text);
    g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    drawHandles (g);
}
