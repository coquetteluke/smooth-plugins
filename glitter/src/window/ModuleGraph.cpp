#include "ModuleGraph.h"
#include "../MotionPatterns.h"
#include "../Scales.h"

using namespace fxui;

namespace
{
    enum ModuleIndex { pitch, motion, throat, harmony, doubler, vocoder, distortion, filter, ring, delay, reverb };
    enum Mapping { plain, harmonyLeft, harmonyRight, upperHalf, filterPeak, ringFrequency, reverbTail, throatTube };

    constexpr float inset = 16.0f;
    constexpr float captionHeight = 24.0f;
    const juce::Colour wash { 0xff1c1c1c };

    const std::vector<const char*>& idsFor (int module)
    {
        static const std::array<std::vector<const char*>, numModules> ids { {
            { "pitchOn", "pitchOctave", "pitchSemitones", "pitchFormants" },
            { "motionOn", "motionPattern", "motionRate", "motionGlide" },
            { "throatOn", "throatLength" },
            { "harmonyOn", "harmonyVoices", "harmonyInterval1", "harmonyInterval2", "harmonyInKey", "harmonyLevel", "harmonyWidth" },
            { "doublerOn", "doublerLevel", "doublerTiming", "doublerDetune", "doublerWidth" },
            { "vocoderOn", "vocoderMix", "vocoderOctave", "vocoderWave" },
            { "distOn", "distType", "distDrive", "distTone", "distMix" },
            { "filterOn", "filterType", "filterCutoff", "filterResonance", "filterSweepDepth", "filterSweepRate" },
            { "ringOn", "ringFrequency", "ringMix" },
            { "delayOn", "delayTime", "delayFeedback", "delayTone", "delayMix", "delayPingPong" },
            { "reverbOn", "reverbType", "reverbDecay", "reverbPreDelay", "reverbMix" },
        } };
        return ids[(size_t) module];
    }

    const char* captionFor (int module)
    {
        switch (module)
        {
            case pitch:      return "Semitones left and right, octaves up and down";
            case motion:     return "The pattern, 16 steps, in your key";
            case throat:     return "Drag the end. The curve is where the voice's peaks land";
            case harmony:    return "Up and down for the note, out to the sides for width";
            case doubler:    return "Timing left and right, detune up and down";
            case vocoder:    return "Mix left and right, octave up and down";
            case distortion: return "Drive left and right, tone up and down";
            case filter:     return "Cutoff left and right, resonance up and down";
            case ring:       return "Frequency left and right, mix up and down";
            case delay:      return "Feedback left and right, mix up and down";
            default:         return "Decay left and right, mix up and down";
        }
    }

    juce::String noteName (int midi)
    {
        return juce::String (smoothtune::noteNames[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }

    void smallText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification j, juce::Colour c = tone::faint)
    {
        g.setColour (c);
        g.setFont (font (Weight::regular, 11.5f));
        g.drawText (text, area, j, false);
    }

    constexpr double reverbSeconds = 12.0;
    float tailProportion (double seconds) { return (float) std::sqrt (juce::jlimit (0.0, 1.0, seconds / reverbSeconds)); }

    float logProportion (double hz, double low, double high) { return (float) (std::log (hz / low) / std::log (high / low)); }

    // The filter picture's level scale, top to bottom, and the height of its peak for a Resonance setting: the same law as the processor, in decibels.
    constexpr float filterTopDb = 24.0f, filterBottomDb = -36.0f;
    float filterPeakDb (float resonance01) { return 20.0f * std::log10 (0.707f * std::pow (12.0f, resonance01)); }
    float filterYProportion (float db) { return (filterTopDb - db) / (filterTopDb - filterBottomDb); }
}

ModuleGraph::ModuleGraph (PluginProcessor& p) : processor (p)
{
    setOpaque (false);
    setTitle ("Module picture");
    setModule (juce::jlimit (0, numModules - 1, p.selectedModule.load (std::memory_order_relaxed)));
}

float ModuleGraph::raw (const char* id) const
{
    return processor.apvts.getRawParameterValue (id)->load (std::memory_order_relaxed);
}

juce::RangedAudioParameter* ModuleGraph::param (const char* id) const
{
    return processor.apvts.getParameter (id);
}

void ModuleGraph::setModule (int m)
{
    module = m;
    watched.clear();
    for (auto* id : idsFor (m))
        watched.push_back (processor.apvts.getRawParameterValue (id));
    seen.assign (watched.size(), -1.0e9f);

    handles.clear();
    const auto add = [this] (const char* x, const char* y, int mapping = plain)
    {
        handles.push_back ({ x != nullptr ? param (x) : nullptr, y != nullptr ? param (y) : nullptr, mapping });
    };

    switch (m)
    {
        case pitch:      add ("pitchSemitones", "pitchOctave"); break;
        case throat:     add ("throatLength", nullptr, throatTube); break;
        case harmony:    add ("harmonyWidth", "harmonyInterval1", harmonyLeft); add ("harmonyWidth", "harmonyInterval2", harmonyRight); break;
        case doubler:    add ("doublerTiming", "doublerDetune", upperHalf); break;
        case vocoder:    add ("vocoderMix", "vocoderOctave"); break;
        case distortion: add ("distDrive", "distTone"); break;
        case filter:     add ("filterCutoff", "filterResonance", filterPeak); break;
        case ring:       add ("ringFrequency", "ringMix", ringFrequency); break;
        case delay:      add ("delayFeedback", "delayMix"); break;
        case reverb:     add ("reverbDecay", "reverbMix", reverbTail); break;
        default: break;
    }

    hint = captionFor (m);
    hoveredHandle = draggedHandle = -1;
    stepShown = -1;
    filterPixel = -1;
    stale = true;
    setMouseCursor (handles.empty() ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
    repaint();
}

void ModuleGraph::resized()
{
    stale = true;
    repaint();
}

juce::Rectangle<float> ModuleGraph::plot() const
{
    return getLocalBounds().toFloat().reduced (inset).withTrimmedBottom (captionHeight - 6.0f);
}

int ModuleGraph::voiceNote() const
{
    const auto r = processor.getReadings();
    return r.voiced ? r.targetNote : -1;
}

bool ModuleGraph::refresh()
{
    bool moved = false;
    for (size_t i = 0; i < watched.size(); ++i)
    {
        const float v = watched[i]->load (std::memory_order_relaxed);
        if (v != seen[i])
        {
            seen[i] = v;
            stale = true;
        }
    }

    // Motion's roll and Ring's sidebands depend on the note and the scale too. Those change only as often as the notes do.
    if (module == motion || module == ring)
    {
        const int note = voiceNote(), scale = (int) processor.getScaleMask();
        if ((note >= 0 && note != noteSeen) || scale != scaleSeen)
        {
            if (note >= 0)
                noteSeen = note;
            scaleSeen = scale;
            stale = true;
        }
    }

    if (stale)
    {
        repaint();
        moved = true;
    }

    if (module == motion)
    {
        const int step = processor.getMotionStep();
        if (step != stepShown)
        {
            const auto dirty = [this] (int s) { if (s >= 0) repaint (motionCells[(size_t) s].expanded (3.0f).getSmallestIntegerContainer()); };
            dirty (stepShown);
            dirty (step);
            stepShown = step;
            moved = true;
        }
    }
    else if (module == filter)
    {
        const auto area = plot();
        const bool live = raw ("filterOn") >= 0.5f && raw ("filterSweepDepth") > 0.001f;
        const int pixel = live ? juce::roundToInt (area.getX() + area.getWidth() * logProportion (juce::jlimit (20.0f, 20000.0f, processor.getFilterHz()), 20.0, 20000.0)) : -1;
        // Half the frame rate at most, and only for a move you can see. It sits far from the strip, so each repaint of it costs a large merged area.
        filterClock = (filterClock + 1) % 2;
        if (pixel != filterPixel && (filterClock == 0 || pixel < 0) && (pixel < 0 || filterPixel < 0 || std::abs (pixel - filterPixel) >= 2))
        {
            const auto dirty = [&] (int x) { if (x >= 0) repaint (x - 2, (int) area.getY(), 4, (int) area.getHeight()); };
            dirty (filterPixel);
            dirty (pixel);
            filterPixel = pixel;
            moved = true;
        }
    }

    return moved;
}

// ------------------------------------------------------------------------------------------------------------------ pad

juce::Point<float> ModuleGraph::handlePosition (const Handle& h) const
{
    const auto a = plot();
    const float nx = h.x != nullptr ? h.x->getValue() : 0.5f, ny = h.y != nullptr ? h.y->getValue() : 0.5f;
    float px = nx, py = 1.0f - ny;

    switch (h.mapping)
    {
        case harmonyLeft:
        case harmonyRight:
        {
            const bool two = raw ("harmonyVoices") >= 0.5f;
            const float out = two ? 0.42f * nx : 0.0f;
            px = h.mapping == harmonyLeft ? 0.5f - out : 0.5f + out;
            break;
        }
        case upperHalf:
            py = 0.5f - 0.46f * ny;
            break;
        case filterPeak:
            px = logProportion (h.x->convertFrom0to1 (nx), 20.0, 20000.0);
            py = filterYProportion (filterPeakDb (ny));
            break;
        case ringFrequency:
            px = logProportion (h.x->convertFrom0to1 (nx), 20.0, 5000.0);
            break;
        case reverbTail:
            px = tailProportion ((double) (raw ("reverbPreDelay") * 0.001f + h.x->convertFrom0to1 (nx)));
            break;
        case throatTube:
            py = 0.84f;
            break;
        default:
            break;
    }

    return { a.getX() + a.getWidth() * px, a.getY() + a.getHeight() * py };
}

void ModuleGraph::moveHandle (const Handle& h, juce::Point<float> position)
{
    const auto a = plot();
    const float px = juce::jlimit (0.0f, 1.0f, (position.x - a.getX()) / a.getWidth());
    const float py = juce::jlimit (0.0f, 1.0f, (position.y - a.getY()) / a.getHeight());
    float nx = px, ny = 1.0f - py;

    switch (h.mapping)
    {
        case harmonyLeft:  nx = juce::jlimit (0.0f, 1.0f, (0.5f - px) / 0.42f); break;
        case harmonyRight: nx = juce::jlimit (0.0f, 1.0f, (px - 0.5f) / 0.42f); break;
        case upperHalf:    ny = juce::jlimit (0.0f, 1.0f, (0.5f - py) / 0.46f); break;
        case filterPeak:
        {
            nx = h.x->convertTo0to1 (20.0f * std::pow (1000.0f, px));
            const float db = filterTopDb - py * (filterTopDb - filterBottomDb);
            ny = juce::jlimit (0.0f, 1.0f, (std::log (std::pow (10.0f, db / 20.0f) / 0.707f)) / std::log (12.0f));
            break;
        }
        case ringFrequency: nx = h.x->convertTo0to1 (20.0f * std::pow (250.0f, px)); break;
        case reverbTail:   nx = h.x->convertTo0to1 (juce::jlimit (0.2f, 10.0f, (float) (reverbSeconds * px * px) - raw ("reverbPreDelay") * 0.001f)); break;
        default: break;
    }

    const auto set = [] (juce::RangedAudioParameter& p, float normalised)
    {
        // Through the parameter's own steps, so a whole-number control stays whole.
        const float snapped = p.convertTo0to1 (p.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised)));
        if (std::abs (snapped - p.getValue()) > 1.0e-6f)
            p.setValueNotifyingHost (snapped);
    };

    const bool widthCounts = (h.mapping != harmonyLeft && h.mapping != harmonyRight) || raw ("harmonyVoices") >= 0.5f;
    if (h.x != nullptr && widthCounts)
        set (*h.x, nx);
    if (h.y != nullptr)
        set (*h.y, ny);
}

int ModuleGraph::handleAt (juce::Point<float> p) const
{
    int best = -1;
    float bestDistance = 1.0e9f;
    for (int i = 0; i < (int) handles.size(); ++i)
    {
        if (module == harmony && i == 1 && raw ("harmonyVoices") < 0.5f)
            continue;
        const float d = handlePosition (handles[(size_t) i]).getDistanceFrom (p);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

void ModuleGraph::mouseMove (const juce::MouseEvent& e)
{
    const int now = handleAt (e.position);
    if (now != hoveredHandle)
    {
        hoveredHandle = now;
        repaint();
    }
}

void ModuleGraph::mouseExit (const juce::MouseEvent&)
{
    if (hoveredHandle != -1)
    {
        hoveredHandle = -1;
        repaint();
    }
}

void ModuleGraph::mouseDown (const juce::MouseEvent& e)
{
    draggedHandle = handleAt (e.position);
    if (draggedHandle < 0)
        return;

    const auto& h = handles[(size_t) draggedHandle];
    if (h.x != nullptr) h.x->beginChangeGesture();
    if (h.y != nullptr) h.y->beginChangeGesture();
    moveHandle (h, e.position);
    repaint();
}

void ModuleGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (draggedHandle >= 0)
    {
        moveHandle (handles[(size_t) draggedHandle], e.position);
        repaint();
    }
}

void ModuleGraph::mouseUp (const juce::MouseEvent&)
{
    if (draggedHandle >= 0)
    {
        const auto& h = handles[(size_t) draggedHandle];
        if (h.x != nullptr) h.x->endChangeGesture();
        if (h.y != nullptr) h.y->endChangeGesture();
    }
    draggedHandle = -1;
}

void ModuleGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int hit = handleAt (e.position);
    if (hit < 0)
        return;

    for (auto* p : { handles[(size_t) hit].x, handles[(size_t) hit].y })
        if (p != nullptr)
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
    repaint();
}

void ModuleGraph::dragHandleTo (int handle, juce::Point<float> proportion)
{
    const auto a = plot();
    const auto& h = handles[(size_t) handle];
    if (h.x != nullptr) h.x->beginChangeGesture();
    if (h.y != nullptr) h.y->beginChangeGesture();
    moveHandle (h, { a.getX() + a.getWidth() * proportion.x, a.getY() + a.getHeight() * proportion.y });
    if (h.x != nullptr) h.x->endChangeGesture();
    if (h.y != nullptr) h.y->endChangeGesture();
    repaint();
}

// -------------------------------------------------------------------------------------------------------------- pictures

void ModuleGraph::drawPitch (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    // Every setting the two controls can make, as a field of dots, with the way from the voice to the setting drawn in.
    const int semis = juce::roundToInt (raw ("pitchSemitones")), octs = juce::roundToInt (raw ("pitchOctave"));
    const auto at = [&] (int s, int o) { return juce::Point<float> (a.getX() + a.getWidth() * (float) (s + 12) / 24.0f, a.getY() + a.getHeight() * (float) (2 - o) / 4.0f); };

    // Where your voice is, before any of this.
    g.setColour (tone::white);
    g.fillEllipse (at (0, 0).x - 4.0f, at (0, 0).y - 4.0f, 8.0f, 8.0f);

    g.setColour (c.withAlpha (0.55f));
    const auto origin = at (0, 0), corner = at (semis, 0), end = at (semis, octs);
    g.drawLine ({ origin, corner }, 4.0f);
    g.drawLine ({ corner, end }, 4.0f);

    for (int o = -2; o <= 2; ++o)
    {
        for (int s = -12; s <= 12; ++s)
        {
            const auto p = at (s, o);
            const bool landmark = (s % 12 == 0);
            g.setColour (landmark ? tone::muted : tone::faint.withAlpha (0.8f));
            const float r = landmark ? 2.0f : 1.4f;
            g.fillEllipse (p.x - r, p.y - r, r * 2.0f, r * 2.0f);
        }
        smallText (g, o == 0 ? juce::String ("Your voice") : (o > 0 ? "+" : "") + juce::String (o) + (std::abs (o) == 1 ? " octave" : " octaves"), juce::Rectangle<float> (a.getX(), at (0, o).y - 17.0f, 90.0f, 14.0f), juce::Justification::centredLeft, o == 0 ? tone::muted : tone::faint);
    }

    const int total = 12 * octs + semis;
    if (total != 0)
    {
        // Beside the dot, on the side away from the line, so nothing crosses the words.
        const bool onRight = semis <= 0;
        const auto label = juce::Rectangle<float> (onRight ? end.x + 14.0f : end.x - 134.0f, end.y - 20.0f, 120.0f, 16.0f).constrainedWithin (a);
        smallText (g, (total > 0 ? "Up " : "Down ") + juce::String (std::abs (total)) + (std::abs (total) == 1 ? " note" : " notes"), label,
                   onRight ? juce::Justification::centredLeft : juce::Justification::centredRight, tone::white);
    }
}

void ModuleGraph::drawMotion (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    using namespace tunefx;
    const auto& pattern = motionPattern (juce::roundToInt (raw ("motionPattern")));
    const int base = noteSeen >= 0 ? noteSeen : 60 + juce::roundToInt (raw ("key"));
    const auto mask = processor.getScaleMask();

    std::array<int, 16> notes {};
    int low = 0, high = 0;
    for (size_t s = 0; s < 16; ++s)
    {
        notes[s] = walkScale (base, pattern[s].degrees, mask) - base + 12 * pattern[s].octaves;
        low = juce::jmin (low, notes[s]);
        high = juce::jmax (high, notes[s]);
    }
    if (high - low < 4)
    {
        low -= (4 - (high - low)) / 2;
        high = low + 4;
    }

    auto roll = a.withTrimmedLeft (34.0f);
    const float laneH = roll.getHeight() / (float) (high - low + 1);

    // Lanes for the notes of the key, named on the left when there is room.
    for (int n = low; n <= high; ++n)
    {
        const auto lane = juce::Rectangle<float> (roll.getX(), roll.getBottom() - laneH * (float) (n - low + 1), roll.getWidth(), laneH);
        const bool inKey = ((mask >> (((base + n) % 12 + 12) % 12)) & 1) != 0;
        if (inKey)
        {
            g.setColour (wash);
            g.fillRect (lane.reduced (0.0f, 0.5f));
            if (laneH >= 11.0f)
                smallText (g, noteName (base + n), juce::Rectangle<float> (a.getX(), lane.getY(), 30.0f, lane.getHeight()), juce::Justification::centredRight, n == 0 ? tone::white : tone::faint);
        }
    }

    const float stepW = roll.getWidth() / 16.0f;
    for (int s = 0; s < 16; ++s)
    {
        const auto cell = juce::Rectangle<float> (roll.getX() + stepW * (float) s, roll.getBottom() - laneH * (float) (notes[(size_t) s] - low + 1), stepW, laneH)
                              .reduced (1.5f, juce::jmin (1.0f, laneH * 0.08f));
        motionCells[(size_t) s] = cell;
        g.setColour (c);
        g.fillRoundedRectangle (cell, 2.5f);
    }
}

void ModuleGraph::drawThroat (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    const float length = raw ("throatLength");
    const auto curveArea = a.withHeight (a.getHeight() * 0.68f);

    // Where a typical voice's peaks land, against where they start. Longer moves them down by exactly the length.
    const auto shape = [] (float octave)
    {
        const float peaks[3][3] = { { 1.3f, 1.0f, 0.42f }, { 2.5f, 0.62f, 0.38f }, { 3.75f, 0.45f, 0.45f } };
        float v = 0.0f;
        for (auto& p : peaks)
            v = juce::jmax (v, p[1] * std::exp (-(octave - p[0]) * (octave - p[0]) / (2.0f * p[2] * p[2])));
        return v;
    };
    const float shift = std::log2 (100.0f / length);

    juce::Path before, after;
    for (float x = 0.0f; x <= curveArea.getWidth(); x += 3.0f)
    {
        const float octave = 5.0f * x / curveArea.getWidth();
        const float y0 = curveArea.getBottom() - curveArea.getHeight() * 0.92f * shape (octave);
        const float y1 = curveArea.getBottom() - curveArea.getHeight() * 0.92f * shape (octave - shift);
        if (x == 0.0f)
        {
            before.startNewSubPath (curveArea.getX(), y0);
            after.startNewSubPath (curveArea.getX(), y1);
        }
        else
        {
            before.lineTo (curveArea.getX() + x, y0);
            after.lineTo (curveArea.getX() + x, y1);
        }
    }
    g.setColour (tone::faint);
    g.strokePath (before, juce::PathStrokeType (1.2f));
    auto fill = after;
    fill.lineTo (curveArea.getRight(), curveArea.getBottom());
    fill.lineTo (curveArea.getX(), curveArea.getBottom());
    fill.closeSubPath();
    g.setColour (c.withAlpha (0.28f));
    g.fillPath (fill);
    g.setColour (c);
    g.strokePath (after, juce::PathStrokeType (2.2f));

    // The throat itself, as long as it is set.
    const float y = a.getY() + a.getHeight() * 0.84f, h = 22.0f;
    const auto range = param ("throatLength")->getNormalisableRange();
    const float end = a.getX() + a.getWidth() * range.convertTo0to1 (length);
    const float normal = a.getX() + a.getWidth() * range.convertTo0to1 (100.0f);
    g.setColour (wash);
    g.fillRoundedRectangle (a.getX(), y - h * 0.5f, a.getWidth(), h, 11.0f);
    g.setColour (c);
    g.fillRoundedRectangle (a.getX(), y - h * 0.5f, juce::jmax (22.0f, end - a.getX()), h, 11.0f);
    g.setColour (tone::white);
    g.fillRect (normal - 0.75f, y - h * 0.5f - 5.0f, 1.5f, h + 10.0f);
    smallText (g, "Normal", juce::Rectangle<float> (normal - 40.0f, y + h * 0.5f + 3.0f, 80.0f, 14.0f), juce::Justification::centred);
}

void ModuleGraph::drawHarmony (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    const bool two = raw ("harmonyVoices") >= 0.5f;
    const float centreY = a.getCentreY();
    const float level = juce::jmap (raw ("harmonyLevel"), -36.0f, 6.0f, 0.25f, 1.0f);

    for (int o = -2; o <= 2; ++o)
    {
        const float y = a.getY() + a.getHeight() * (float) (2 - o) / 4.0f;
        g.setColour (o == 0 ? tone::white : wash);
        g.fillRect (a.getX(), y - (o == 0 ? 1.0f : 0.5f), a.getWidth(), o == 0 ? 2.0f : 1.0f);
        if (std::abs (o) == 1)
            smallText (g, (o > 0 ? "+" : "") + juce::String (12 * o), juce::Rectangle<float> (a.getX(), y - 15.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
    }

    const int lead = noteSeen >= 0 ? noteSeen : voiceNote();
    smallText (g, lead >= 0 ? "Lead, " + noteName (lead) : juce::String ("Lead"), juce::Rectangle<float> (a.getX(), centreY - 16.0f, 90.0f, 14.0f), juce::Justification::centredLeft, tone::white);

    for (int v = 0; v < (two ? 2 : 1); ++v)
    {
        const auto p = handlePosition (handles[(size_t) v]);
        g.setColour (c.withAlpha (level));
        g.fillRoundedRectangle (p.x - 5.0f, juce::jmin (centreY, p.y), 10.0f, std::abs (p.y - centreY), 3.0f);

        const int interval = juce::roundToInt (raw (v == 0 ? "harmonyInterval1" : "harmonyInterval2"));
        const bool above = p.y < centreY + 4.0f;
        smallText (g, (interval > 0 ? "+" : "") + juce::String (interval), juce::Rectangle<float> (p.x - 30.0f, p.y + (above ? -30.0f : 14.0f), 60.0f, 16.0f).constrainedWithin (a),
                   juce::Justification::centred, tone::white);
    }

    smallText (g, "Left", a.withHeight (14.0f).withBottomY (a.getBottom()), juce::Justification::centredLeft);
    smallText (g, "Right", a.withHeight (14.0f).withBottomY (a.getBottom()), juce::Justification::centredRight);
}

void ModuleGraph::drawDoubler (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    // The lead and the two copies as sounds running in time: one later and higher, one a little less late and lower, the way the Doubler really sets them.
    const auto& range = param ("doublerTiming")->getNormalisableRange();
    const float timing = raw ("doublerTiming"), detune = param ("doublerDetune")->getValue();
    const float centreY = a.getCentreY(), reach = a.getHeight() * 0.46f;
    const float barH = juce::jmap (raw ("doublerLevel"), -36.0f, 6.0f, 6.0f, 22.0f);

    g.setColour (wash);
    g.fillRect (a.getX(), centreY - 0.5f, a.getWidth(), 1.0f);

    const float xA = a.getX() + a.getWidth() * range.convertTo0to1 (timing);
    const float xB = a.getX() + a.getWidth() * range.convertTo0to1 (juce::jmax (5.0f, 0.72f * timing));
    const float yA = centreY - reach * detune, yB = centreY + reach * detune;

    g.setColour (c);
    g.fillRoundedRectangle (xA, yA - barH * 0.5f, a.getRight() - xA, barH, juce::jmin (6.0f, barH * 0.5f));
    g.setColour (c.withAlpha (0.7f));
    g.fillRoundedRectangle (xB, yB - barH * 0.5f, a.getRight() - xB, barH, juce::jmin (6.0f, barH * 0.5f));
    g.setColour (tone::white);
    g.fillRoundedRectangle (a.getX(), centreY - 5.0f, a.getWidth(), 10.0f, 5.0f);

    smallText (g, "Your voice", juce::Rectangle<float> (a.getX() + 2.0f, centreY + 8.0f, 100.0f, 14.0f), juce::Justification::centredLeft, tone::white);
}

void ModuleGraph::drawVocoder (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    const int wave = juce::roundToInt (raw ("vocoderWave")), octave = juce::roundToInt (raw ("vocoderOctave"));
    const float mix = raw ("vocoderMix") * 0.01f;
    const float dry = tunefx::mixDry (mix), wet = tunefx::mixWet (mix);

    // Lanes for the four octaves.
    for (int o = -2; o <= 1; ++o)
    {
        const float y = a.getY() + a.getHeight() * (float) (1 - o) / 3.0f;
        g.setColour (o == octave ? wash.brighter (0.25f) : wash);
        g.fillRect (a.getX(), y - 0.5f, a.getWidth(), 1.0f);
        smallText (g, (o > 0 ? "+" : "") + juce::String (o), juce::Rectangle<float> (a.getRight() - 24.0f, y - (o == 1 ? -2.0f : 15.0f), 24.0f, 14.0f), juce::Justification::centredRight);
    }

    // The wave as set, faster for a higher octave, and as tall as the mix makes it.
    const int cycles = 2 << (octave + 2);
    const auto waveArea = a.withTrimmedRight (28.0f);
    const auto draw = [&] (double detune, float phase, float alpha)
    {
        juce::Path p;
        const int n = juce::roundToInt (waveArea.getWidth());
        const float amp = a.getHeight() * 0.38f * juce::jmax (0.04f, wet);
        for (int i = 0; i <= n; ++i)
        {
            const double t = std::fmod ((double) i / n * cycles * 0.5 * detune + phase + 8.0, 1.0);
            const float v = wave == 1 ? (t < 0.5 ? 0.8f : -0.8f) : (float) (2.0 * t - 1.0);
            const float x = a.getX() + (float) i, y = a.getCentreY() - amp * v;
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (c.withAlpha (alpha));
        g.strokePath (p, juce::PathStrokeType (2.0f));
    };

    if (wave == 2)
    {
        draw (1.05, 0.31f, 0.5f);
        draw (0.95, 0.67f, 0.5f);
    }
    draw (1.0, 0.0f, 1.0f);

    smallText (g, "Voice " + juce::String (juce::roundToInt (dry * 100.0f)) + "%", a.withHeight (14.0f).withBottomY (a.getBottom()), juce::Justification::centredLeft, tone::white);
    smallText (g, "Tone " + juce::String (juce::roundToInt (wet * 100.0f)) + "%", a.withHeight (14.0f).withBottomY (a.getBottom()).withTrimmedRight (30.0f), juce::Justification::centredRight, tone::white);
}

void ModuleGraph::drawDistortion (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    // The real curve the voice goes through: what comes out for each level going in.
    const int type = juce::roundToInt (raw ("distType"));
    const float gain = std::pow (10.0f, raw ("distDrive") * 0.01f * 36.0f / 20.0f), back = std::pow (gain, -0.62f), mix = raw ("distMix") * 0.01f;
    const auto shape = [&] (double u)
    {
        switch (type)
        {
            case 0:  return std::tanh (u + 0.25) - std::tanh (0.25);
            case 1:  return juce::jlimit (-1.0, 1.0, u);
            default: return std::sin (u);
        }
    };

    g.setColour (wash);
    g.fillRect (a.getCentreX() - 0.5f, a.getY(), 1.0f, a.getHeight());
    g.fillRect (a.getX(), a.getCentreY() - 0.5f, a.getWidth(), 1.0f);

    juce::Path straight, curve;
    const int n = juce::roundToInt (a.getWidth());
    const float scale = a.getHeight() * 0.48f;
    for (int i = 0; i <= n; ++i)
    {
        const float in = 2.0f * (float) i / (float) n - 1.0f;
        const float out = in + mix * ((float) shape ((double) in * gain) * back - in);
        const float x = a.getX() + (float) i;
        const float y = a.getCentreY() - scale * juce::jlimit (-1.04f, 1.04f, out);
        const float y0 = a.getCentreY() - scale * in;
        if (i == 0) { curve.startNewSubPath (x, y); straight.startNewSubPath (x, y0); }
        else        { curve.lineTo (x, y); straight.lineTo (x, y0); }
    }
    g.setColour (tone::faint);
    g.strokePath (straight, juce::PathStrokeType (1.0f));
    g.setColour (c);
    g.strokePath (curve, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved));

    smallText (g, "Level in", a.withHeight (14.0f).withBottomY (a.getBottom()), juce::Justification::centredRight);
    smallText (g, "Level out", a.withHeight (14.0f), juce::Justification::centredLeft);
}

void ModuleGraph::drawFilter (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    const int type = juce::roundToInt (raw ("filterType"));
    const float cutoff = raw ("filterCutoff"), depth = raw ("filterSweepDepth");
    const double q = 0.707 * std::pow (12.0, raw ("filterResonance") * 0.01);
    const float top = filterTopDb, bottom = filterBottomDb;
    const auto xOf = [&] (float hz) { return a.getX() + a.getWidth() * logProportion (hz, 20.0, 20000.0); };

    // How far the sweep reaches.
    if (depth > 0.001f)
    {
        const float x0 = xOf (juce::jmax (20.0f, cutoff * std::pow (2.0f, -depth))), x1 = xOf (juce::jmin (20000.0f, cutoff * std::pow (2.0f, depth)));
        g.setColour (c.withAlpha (0.14f));
        g.fillRect (x0, a.getY(), x1 - x0, a.getHeight());
    }

    for (const float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        g.setColour (wash);
        g.fillRect (xOf (hz) - 0.5f, a.getY(), 1.0f, a.getHeight());
        smallText (g, hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz), juce::Rectangle<float> (xOf (hz) + 4.0f, a.getBottom() - 14.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
    }

    // The real response of the filter at this setting.
    juce::Path curve;
    const int n = juce::roundToInt (a.getWidth());
    for (int i = 0; i <= n; ++i)
    {
        const double hz = 20.0 * std::pow (1000.0, (double) i / n), w = hz / (double) cutoff;
        const double denom = std::sqrt ((1.0 - w * w) * (1.0 - w * w) + (w / q) * (w / q));
        const double mag = type == 0 ? w * w / denom : (type == 1 ? (w / q) / denom : 1.0 / denom);
        const float db = juce::jlimit (bottom - 2.0f, top, (float) (20.0 * std::log10 (juce::jmax (1.0e-6, mag))));
        const float x = a.getX() + (float) i, y = a.getY() + a.getHeight() * (top - db) / (top - bottom);
        if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
    }
    auto fill = curve;
    fill.lineTo (a.getRight(), a.getBottom() + 4.0f);
    fill.lineTo (a.getX(), a.getBottom() + 4.0f);
    fill.closeSubPath();
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (a.toNearestInt());
        g.setColour (c.withAlpha (0.28f));
        g.fillPath (fill);
        g.setColour (c);
        g.strokePath (curve, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved));
    }
}

void ModuleGraph::drawRing (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    // A voice's first harmonics in white, and the new tones Ring makes from them in colour: each one plus and minus the frequency.
    const float hz = raw ("ringFrequency"), mix = raw ("ringMix") * 0.01f;
    const int note = noteSeen >= 0 ? noteSeen : 57;
    const float f0 = 440.0f * std::pow (2.0f, ((float) note - 69.0f) / 12.0f);
    const auto xOf = [&] (float f) { return a.getX() + a.getWidth() * logProportion (juce::jlimit (20.0f, 5000.0f, f), 20.0, 5000.0); };
    const float bottom = a.getBottom() - 16.0f, height = bottom - a.getY();

    for (const float f : { 100.0f, 1000.0f })
    {
        g.setColour (wash);
        g.fillRect (xOf (f) - 0.5f, a.getY(), 1.0f, height);
        smallText (g, f >= 1000.0f ? "1k" : "100", juce::Rectangle<float> (xOf (f) - 20.0f, bottom + 2.0f, 40.0f, 14.0f), juce::Justification::centred);
    }

    for (int k = 1; k <= 10; ++k)
    {
        const float f = f0 * (float) k, size = 1.0f / std::sqrt ((float) k);
        if (f < 5000.0f)
        {
            g.setColour (tone::white.withAlpha (0.25f + 0.65f * (1.0f - mix)));
            const float h = height * size * juce::jmax (0.08f, 1.0f - mix);
            g.fillRect (xOf (f) - 1.0f, bottom - h, 2.0f, h);
        }
        g.setColour (c);
        for (const float side : { f + hz, std::abs (f - hz) })
            if (side > 20.0f && side < 5000.0f)
            {
                const float h = height * size * mix * 0.86f;
                g.fillRect (xOf (side) - 1.5f, bottom - h, 3.0f, h);
            }
    }

    g.setColour (tone::white.withAlpha (0.35f));
    g.fillRect (xOf (hz) - 0.5f, a.getY(), 1.0f, height);
}

void ModuleGraph::drawDelay (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    static const double beats[] = { 0.125, 0.25, 1.0 / 3.0, 0.375, 0.5, 2.0 / 3.0, 0.75, 1.0, 1.5, 2.0, 4.0 };
    const double step = beats[juce::jlimit (0, 10, juce::roundToInt (raw ("delayTime")))];
    const float feedback = raw ("delayFeedback") * 0.01f, wet = tunefx::mixWet (raw ("delayMix") * 0.01f), dry = tunefx::mixDry (raw ("delayMix") * 0.01f);
    const bool pingPong = raw ("delayPingPong") >= 0.5f;
    const float centre = a.getCentreY(), reach = a.getHeight() * 0.48f;
    constexpr double beatsShown = 8.0;

    // The beats of two bars, then the voice and each repeat at its real place and real level.
    for (int beat = 0; beat <= 8; ++beat)
    {
        const float x = a.getX() + a.getWidth() * (float) beat / (float) beatsShown;
        g.setColour (wash);
        g.fillRect (x - 0.5f, a.getY(), beat % 4 == 0 ? 2.0f : 1.0f, a.getHeight());
    }
    g.setColour (wash);
    g.fillRect (a.getX(), centre - 0.5f, a.getWidth(), 1.0f);

    g.setColour (tone::white);
    const float dryH = reach * juce::jmax (0.06f, dry);
    g.fillRoundedRectangle (a.getX(), centre - dryH, 4.0f, dryH * 2.0f, 1.5f);

    const float barW = juce::jlimit (2.0f, 9.0f, (float) (a.getWidth() * step / beatsShown) * 0.6f);
    float level = wet;
    for (int k = 1; (double) k * step <= beatsShown && level > 0.003f; ++k)
    {
        const float x = a.getX() + a.getWidth() * (float) ((double) k * step / beatsShown);
        const float h = reach * level;
        g.setColour (c.withAlpha (juce::jmax (0.35f, 1.0f - 0.08f * (float) k)));
        if (pingPong)
            g.fillRoundedRectangle (x, k % 2 == 1 ? centre - h : centre, barW, h, 1.5f);
        else
            g.fillRoundedRectangle (x, centre - h, barW, h * 2.0f, 1.5f);
        level *= feedback;
    }

    if (pingPong)
    {
        smallText (g, "Left", juce::Rectangle<float> (a.getRight() - 40.0f, a.getY(), 40.0f, 14.0f), juce::Justification::centredRight);
        smallText (g, "Right", juce::Rectangle<float> (a.getRight() - 40.0f, a.getBottom() - 14.0f, 40.0f, 14.0f), juce::Justification::centredRight);
    }
}

void ModuleGraph::drawReverb (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c)
{
    // The tail as it falls away: how long before it starts, how long it takes to die, and how loud it is against the voice.
    const float pre = raw ("reverbPreDelay") * 0.001f, decay = raw ("reverbDecay"), mix = raw ("reverbMix") * 0.01f;
    const int type = juce::roundToInt (raw ("reverbType"));
    const float dry = tunefx::mixDry (mix);
    const auto xOf = [&] (float seconds) { return a.getX() + a.getWidth() * tailProportion ((double) seconds); };
    const float bottom = a.getBottom() - 16.0f, height = bottom - a.getY();
    const float peakY = a.getY() + a.getHeight() * (1.0f - mix);

    for (const float s : { 1.0f, 3.0f, 6.0f })
    {
        g.setColour (wash);
        g.fillRect (xOf (s) - 0.5f, a.getY(), 1.0f, height);
        smallText (g, juce::String ((int) s) + " s", juce::Rectangle<float> (xOf (s) - 20.0f, bottom + 2.0f, 40.0f, 14.0f), juce::Justification::centred);
    }

    g.setColour (tone::white);
    const float dryH = height * juce::jmax (0.08f, dry);
    g.fillRoundedRectangle (a.getX(), bottom - dryH, 4.0f, dryH, 1.5f);

    // A hall takes a moment to build, a room and a plate get going at once.
    const float build = type == 2 ? 0.08f : (type == 1 ? 0.02f : 0.005f);
    juce::Path tail;
    tail.startNewSubPath (xOf (pre), bottom);
    const int n = juce::roundToInt (a.getWidth());
    const float top = juce::jmin (bottom - 2.0f, peakY);
    for (int i = 0; i <= n; ++i)
    {
        const float seconds = (float) reverbSeconds * juce::square ((float) i / (float) n);
        if (seconds < pre || seconds > pre + decay)
            continue;
        const float t = seconds - pre;
        const float rise = juce::jmin (1.0f, t / build), fall = 1.0f - t / decay;     // a straight line in decibels falls this way on a level scale
        const float y = bottom - (bottom - top) * rise * fall * fall;
        tail.lineTo (a.getX() + (float) i, y);
    }
    tail.lineTo (xOf (pre + decay), bottom);
    tail.closeSubPath();
    g.setColour (c.withAlpha (0.35f));
    g.fillPath (tail);
    g.setColour (c);
    g.strokePath (tail, juce::PathStrokeType (2.0f));

    // A guide from the top of the tail out to its end, so the dot reads as how loud and how long.
    g.setColour (tone::faint);
    const auto end = handlePosition (handles[0]);
    g.fillRect (xOf (pre), peakY - 0.5f, juce::jmax (0.0f, end.x - xOf (pre)), 1.0f);
    g.fillRect (end.x - 0.5f, peakY, 1.0f, juce::jmax (0.0f, bottom - peakY));
}

void ModuleGraph::drawImage (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto& m = modules()[(size_t) module];
    const bool on = raw (m.onId) >= 0.5f;

    g.fillAll (tone::black);

    // An off module is still set up the same, but plainly not sounding.
    const auto colour = on ? m.colour : m.colour.interpolatedWith (tone::black, 0.55f);
    const auto a = plot();

    switch (module)
    {
        case pitch:      drawPitch (g, a, colour); break;
        case motion:     drawMotion (g, a, colour); break;
        case throat:     drawThroat (g, a, colour); break;
        case harmony:    drawHarmony (g, a, colour); break;
        case doubler:    drawDoubler (g, a, colour); break;
        case vocoder:    drawVocoder (g, a, colour); break;
        case distortion: drawDistortion (g, a, colour); break;
        case filter:     drawFilter (g, a, colour); break;
        case ring:       drawRing (g, a, colour); break;
        case delay:      drawDelay (g, a, colour); break;
        default:         drawReverb (g, a, colour); break;
    }

    smallText (g, captionFor (module), juce::Rectangle<float> (inset, area.getBottom() - captionHeight, area.getWidth() - inset * 2.0f, 16.0f), juce::Justification::centredLeft);
}

void ModuleGraph::rebuild (float pixelScale)
{
    const int w = juce::roundToInt ((float) getWidth() * pixelScale), h = juce::roundToInt ((float) getHeight() * pixelScale);
    if (w <= 0 || h <= 0)
        return;

    // Black with its corners cut, on a clear background, so it sits on the panel's tint.
    juce::Image layer (juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g (layer);
        g.addTransform (juce::AffineTransform::scale (pixelScale));
        const auto area = getLocalBounds().toFloat();
        juce::Path round;
        round.addRoundedRectangle (area, 8.0f);
        g.reduceClipRegion (round);
        drawImage (g, area);
    }
    image = layer;
    imageScale = pixelScale;
    stale = false;
}

void ModuleGraph::paint (juce::Graphics& g)
{
    const float pixelScale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (stale || imageScale != pixelScale || ! image.isValid())
        rebuild (pixelScale);

    if (image.isValid())
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / imageScale));

    const auto& m = modules()[(size_t) module];
    const bool on = raw (m.onId) >= 0.5f;

    // Motion's step right now, in white on the cached roll.
    if (module == motion && stepShown >= 0)
    {
        g.setColour (tone::white);
        g.fillRoundedRectangle (motionCells[(size_t) stepShown], 2.5f);
    }

    // Where the sweep has the cutoff right now.
    if (module == filter && filterPixel >= 0)
    {
        const auto a = plot();
        g.setColour (tone::white);
        g.fillRect ((float) filterPixel - 0.75f, a.getY(), 1.5f, a.getHeight());
    }

    for (int i = 0; i < (int) handles.size(); ++i)
    {
        if (module == harmony && i == 1 && raw ("harmonyVoices") < 0.5f)
            continue;

        const auto p = handlePosition (handles[(size_t) i]);
        const bool lit = i == hoveredHandle || i == draggedHandle;

        // A free dot gets a cross through it, so it reads as a pad and not as a point on the picture.
        if (handles[(size_t) i].mapping == plain && module != pitch)
        {
            const auto a = plot();
            g.setColour (tone::white.withAlpha (lit ? 0.3f : 0.16f));
            g.fillRect (a.getX(), p.y - 0.5f, a.getWidth(), 1.0f);
            g.fillRect (p.x - 0.5f, a.getY(), 1.0f, a.getHeight());
        }

        const float r = lit ? 9.0f : 7.5f;
        g.setColour (tone::black);
        g.fillEllipse (p.x - r - 2.0f, p.y - r - 2.0f, (r + 2.0f) * 2.0f, (r + 2.0f) * 2.0f);
        g.setColour (on || lit ? tone::white : tone::muted);
        g.fillEllipse (p.x - r, p.y - r, r * 2.0f, r * 2.0f);
    }
}
