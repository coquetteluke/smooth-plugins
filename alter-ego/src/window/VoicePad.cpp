#include "VoicePad.h"

using namespace ui;

VoicePad::VoicePad (PluginProcessor& p)
    : processorRef (p),
      pitchParam (*p.apvts.getParameter ("pitch")),
      formantParam (*p.apvts.getParameter ("formant")),
      pitchAttachment (pitchParam, [this] (float v) { pitch = v; changed(); }, nullptr),
      formantAttachment (formantParam, [this] (float v) { formant = v; changed(); }, nullptr),
      characterAttachment (*p.apvts.getParameter ("character"), [this] (float v) { character = juce::roundToInt (v); changed(); }, nullptr),
      amountAttachment (*p.apvts.getParameter ("characterAmount"), [this] (float v) { amount = v * 0.01f; changed(); }, nullptr),
      bypassAttachment (*p.apvts.getParameter ("bypass"), [this] (float v) { bypassed = v >= 0.5f; changed(); }, nullptr)
{
    setOpaque (true);
    setComponentID ("pad");
    setTitle ("Pitch and Formant Shift");
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    hint = "Drag the dot. Hold Shift for whole semitones.";

    pitchAttachment.sendInitialUpdate();
    formantAttachment.sendInitialUpdate();
    characterAttachment.sendInitialUpdate();
    amountAttachment.sendInitialUpdate();
    bypassAttachment.sendInitialUpdate();
}

// The map is 48 semitones across and 24 up, kept at that shape and centred, so a semitone is the same distance either way at any window
// size. It filled the pad to 1.2.5, and the two only matched near the default size (1600 x 1200: 1.65 times as far up as across) (1.2.6).
juce::Rectangle<float> VoicePad::map() const noexcept
{
    const auto room = getLocalBounds().toFloat().reduced (20.0f, 18.0f);
    const float perSemitone = juce::jmax (0.0f, juce::jmin (room.getWidth() / (float) (2.0 * pitchReach), room.getHeight() / (float) (2.0 * formantReach)));
    return juce::Rectangle<float> (perSemitone * (float) (2.0 * pitchReach), perSemitone * (float) (2.0 * formantReach)).withCentre (room.getCentre());
}

juce::Point<float> VoicePad::pointFor (double p, double f) const noexcept
{
    const auto m = map();
    return { m.getCentreX() + (float) (juce::jlimit (-pitchReach, pitchReach, p) / pitchReach) * m.getWidth() * 0.5f,
             m.getCentreY() - (float) (juce::jlimit (-formantReach, formantReach, f) / formantReach) * m.getHeight() * 0.5f };
}

double VoicePad::voicePitch() const noexcept
{
    return (double) pitch + (double) amount * smoothmorph::characters[juce::jlimit (0, smoothmorph::numCharacters - 1, character)].pitchSemitones;
}

double VoicePad::voiceFormant() const noexcept
{
    return (double) formant + (double) amount * smoothmorph::characters[juce::jlimit (0, smoothmorph::numCharacters - 1, character)].formantSemitones;
}

void VoicePad::changed()
{
    layerValid = false;
    repaint();
}

void VoicePad::showCharacter (int index)
{
    if (index != shownCharacter)
    {
        shownCharacter = index;
        changed();
    }
}

void VoicePad::resized()
{
    layerValid = false;
    discCentre = pointFor (0.0, voiceFormant());
    for (auto& t : trail)
        t = discCentre;
}

// ---- the part that only moves with a parameter ------------------------------------------------------------------

void VoicePad::drawLayer (juce::Graphics& g) const
{
    const auto b = getLocalBounds().toFloat();
    g.fillAll (tone::base);
    g.setColour (tone::well);
    g.fillRoundedRectangle (b, 14.0f);

    const auto m = map();
    const auto centre = m.getCentre();

    // One dot for every three semitones. Lines at the octaves, and for formants at the half way mark.
    g.setColour (tone::text.withAlpha (0.26f));
    for (int x = -24; x <= 24; x += 3)
        for (int y = -12; y <= 12; y += 3)
        {
            const auto at = pointFor ((double) x, (double) y);
            g.fillEllipse (at.x - 1.0f, at.y - 1.0f, 2.0f, 2.0f);
        }

    g.setColour (tone::text.withAlpha (0.13f));
    g.fillRect (juce::Rectangle<float> (m.getX(), centre.y - 0.5f, m.getWidth(), 1.0f));
    g.fillRect (juce::Rectangle<float> (centre.x - 0.5f, m.getY(), 1.0f, m.getHeight()));

    // Where the ends go.
    g.setFont (font (Weight::regular, 12.0f));
    g.setColour (tone::muted);
    const auto labelFont = font (Weight::regular, 12.0f);
    g.drawText ("Lower pitch", juce::Rectangle<float> (m.getX() - 6.0f, centre.y - 20.0f, 100.0f, 18.0f), juce::Justification::centredLeft, false);
    g.drawText ("Higher pitch", juce::Rectangle<float> (m.getRight() - 94.0f, centre.y - 20.0f, 100.0f, 18.0f), juce::Justification::centredRight, false);
    g.drawText ("Smaller body", juce::Rectangle<float> (centre.x + 8.0f, m.getY() - 8.0f, 100.0f, 18.0f), juce::Justification::centredLeft, false);
    g.drawText ("Bigger body", juce::Rectangle<float> (centre.x + 8.0f, m.getBottom() - 10.0f, 100.0f, 18.0f), juce::Justification::centredLeft, false);

    g.setColour (tone::faint);
    g.setFont (font (Weight::regular, 11.0f));
    const auto tick = [&] (double p, double f, const char* text)
    {
        const auto at = pointFor (p, f);
        // Under the line going across, beside the line going up.
        g.drawText (juce::String::fromUTF8 (text), p != 0.0 ? juce::Rectangle<float> (at.x - 15.0f, at.y + 3.0f, 30.0f, 14.0f)
                                                            : juce::Rectangle<float> (at.x + 8.0f, at.y - 7.0f, 30.0f, 14.0f),
                    p != 0.0 ? juce::Justification::centred : juce::Justification::centredLeft, false);
    };
    tick (-12.0, 0.0, "\xe2\x88\x92" "12");
    tick (12.0, 0.0, "+12");
    tick (0.0, 6.0, "+6");
    tick (0.0, -6.0, "\xe2\x88\x92" "6");

    const auto voice = voiceAt();

    // Where each Character would land from here. They all add to Pitch and Formant Shift, so they move with them.
    for (int i = 0; i < smoothmorph::numCharacters; ++i)
    {
        if (i == character || i == 3)   // Robot does not move the voice to a place: it holds it on a note
            continue;

        const auto& c = smoothmorph::characters[i];
        const auto at = pointFor ((double) pitch + (double) amount * c.pitchSemitones, (double) formant + (double) amount * c.formantSemitones);
        if (at.getDistanceFrom (voice) < dotRadius + 6.0f || at.getDistanceFrom (centre) < 9.0f)
            continue;

        const bool shown = i == shownCharacter;
        g.setColour (shown ? tone::acid : tone::muted);
        g.drawEllipse (at.x - 4.5f, at.y - 4.5f, 9.0f, 9.0f, shown ? 1.8f : 1.2f);

        const juce::String name (c.name);
        const float width = juce::GlyphArrangement::getStringWidth (labelFont, name) + 4.0f;
        const bool leftSide = at.x < centre.x;
        g.setFont (labelFont);
        g.setColour (shown ? tone::acid : tone::muted);
        g.drawText (name, leftSide ? juce::Rectangle<float> (at.x - 8.0f - width, at.y - 9.0f, width, 18.0f)
                                   : juce::Rectangle<float> (at.x + 8.0f, at.y - 9.0f, width, 18.0f),
                    leftSide ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
    }

    // You, and the line from you to the voice.
    if (! bypassed && voice.getDistanceFrom (centre) > 1.0f)
    {
        g.setColour (tone::acid.withAlpha (0.8f));
        g.drawLine ({ centre, voice }, 1.6f);
    }

    g.setColour (tone::well);
    g.fillEllipse (centre.x - 7.0f, centre.y - 7.0f, 14.0f, 14.0f);
    g.setColour (tone::you);
    g.drawEllipse (centre.x - 6.0f, centre.y - 6.0f, 12.0f, 12.0f, 2.0f);
    g.setFont (font (Weight::medium, 12.0f));
    // Below and away from the line to the voice.
    const bool labelRight = voice.x < centre.x - 1.0f;
    g.drawText ("You", labelRight ? juce::Rectangle<float> (centre.x + 8.0f, centre.y + 8.0f, 32.0f, 16.0f)
                                  : juce::Rectangle<float> (centre.x - 40.0f, centre.y + 8.0f, 32.0f, 16.0f),
                labelRight ? juce::Justification::centredLeft : juce::Justification::centredRight, false);

    // The voice. It sits at You when bypassed, because that is where the sound is.
    const auto dotAt = bypassed ? centre : voice;
    const auto dotBox = juce::Rectangle<float> (dotRadius * 2.0f, dotRadius * 2.0f).withCentre (dotAt);
    g.setColour (bypassed ? tone::faint : tone::acid);
    if (bypassed)
        g.drawEllipse (dotBox.reduced (1.0f), 2.0f);
    else
        g.fillEllipse (dotBox);

    const juce::String tag = bypassed ? juce::String ("Bypassed") : (character > 0 ? juce::String (smoothmorph::characters[character].name) : juce::String());
    if (tag.isNotEmpty())
    {
        // On the far side from You, so the name stays clear of the line coming in, unless the edge of the pad is in the way.
        const auto tagFont = font (Weight::medium, 14.0f);
        const float width = juce::GlyphArrangement::getStringWidth (tagFont, tag) + 4.0f;
        const float gap = 6.0f;
        bool toRight = dotAt.x >= centre.x;
        if (toRight && dotBox.getRight() + gap + width > b.getRight() - 6.0f)   toRight = false;
        if (! toRight && dotBox.getX() - gap - width < b.getX() + 6.0f)          toRight = true;
        const auto box = juce::Rectangle<float> (toRight ? dotBox.getRight() + gap : dotBox.getX() - gap - width, dotAt.y - 10.0f, width, 20.0f);
        g.setColour (bypassed ? tone::muted : tone::acid);
        g.setFont (tagFont);
        g.drawText (tag, box, toRight ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
    }
}

// ---- the part that moves with the sound -------------------------------------------------------------------------

juce::Rectangle<int> VoicePad::liveBounds() const
{
    auto area = juce::Rectangle<float> (discRadius * 2.0f + 4.0f, discRadius * 2.0f + 4.0f).withCentre (discCentre);
    if (anyTrail)
        for (const auto& t : trail)
            area = area.getUnion (juce::Rectangle<float> (8.0f, 8.0f).withCentre (t));
    return area.getSmallestIntegerContainer().getIntersection (getLocalBounds());
}

bool VoicePad::advance (bool voiced, double liveShift, float level)
{
    const auto before = discCentre;
    const float radiusBefore = discRadius;

    const float wantedLevel = voiced && ! bypassed ? level : 0.0f;
    levelSmooth += (wantedLevel - levelSmooth) * (wantedLevel > levelSmooth ? 0.6f : 0.22f);
    if (levelSmooth < 0.004f)
        levelSmooth = 0.0f;

    if (voiced && ! bypassed)
        liveSmooth += (juce::jlimit (-pitchReach, pitchReach, liveShift) - liveSmooth) * 0.85;
    else if (bypassed)
        liveSmooth = 0.0;

    discCentre = pointFor (liveSmooth, bypassed ? 0.0 : voiceFormant());
    discRadius = levelSmooth > 0.0f ? dotRadius + 2.0f + levelSmooth * 34.0f : 0.0f;

    trail[(size_t) trailAt] = discCentre;
    trailAt = (trailAt + 1) % trailLength;

    // With no sound there is no disc and no trail, so nothing is left to repaint and the timer can slow down.
    bool spread = false;
    if (discRadius > 0.0f)
        for (const auto& t : trail)
            spread = spread || t.getDistanceFrom (discCentre) > 2.0f;

    const bool moved = before.getDistanceFrom (discCentre) > 0.25f || std::abs (radiusBefore - discRadius) > 0.25f || spread != anyTrail || spread;
    anyTrail = spread;

    if (moved)
    {
        const auto next = liveBounds();
        repaint (paintedLive.getUnion (next));
        paintedLive = next;
    }

    return moved || levelSmooth > 0.0f;
}

void VoicePad::paint (juce::Graphics& g)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const float scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

    if (! layerValid || std::abs (scale - layerScale) > 0.001f || layer.isNull())
    {
        const int w = juce::jmax (1, juce::roundToInt ((float) getWidth() * scale)), h = juce::jmax (1, juce::roundToInt ((float) getHeight() * scale));
        if (layer.isNull() || layer.getWidth() != w || layer.getHeight() != h)
            layer = juce::Image (juce::Image::RGB, w, h, false);
        juce::Graphics ig (layer);
        ig.addTransform (juce::AffineTransform::scale (scale));
        drawLayer (ig);
        layerScale = scale;
        layerValid = true;
    }

    g.drawImageTransformed (layer, juce::AffineTransform::scale (1.0f / scale));

    if (discRadius > 0.0f)
    {
        if (anyTrail)
        {
            // Back along where the voice has just been, oldest faintest.
            for (int i = 0; i < trailLength; ++i)
            {
                const auto& t = trail[(size_t) ((trailAt + i) % trailLength)];
                const float age = (float) (i + 1) / (float) trailLength;
                g.setColour (tone::acid.withAlpha (0.1f + 0.5f * age));
                g.fillEllipse (t.x - 2.5f, t.y - 2.5f, 5.0f, 5.0f);
            }
        }

        // Two flat discs, the sound spreading out from the voice. No outline, so nothing cuts through the name beside the dot.
        g.setColour (tone::acid.withAlpha (0.13f));
        g.fillEllipse (juce::Rectangle<float> (discRadius * 2.0f, discRadius * 2.0f).withCentre (discCentre));
        const float inner = dotRadius + (discRadius - dotRadius) * 0.55f;
        g.fillEllipse (juce::Rectangle<float> (inner * 2.0f, inner * 2.0f).withCentre (discCentre));
    }

    ++paints;
    paintSeconds += (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    const auto clip = g.getClipBounds();
    paintedArea += (double) clip.getWidth() * clip.getHeight();
}

// ---- the mouse --------------------------------------------------------------------------------------------------

void VoicePad::moveTo (juce::Point<float> at, bool snap)
{
    const auto m = map();
    double wantedPitch = ((double) at.x - m.getCentreX()) / (m.getWidth() * 0.5) * pitchReach;
    double wantedFormant = ((double) m.getCentreY() - at.y) / (m.getHeight() * 0.5) * formantReach;

    if (snap)
    {
        wantedPitch = std::round (wantedPitch);
        wantedFormant = std::round (wantedFormant);
    }

    setVoice (wantedPitch, wantedFormant);
}

void VoicePad::setVoice (double wantedPitch, double wantedFormant)
{
    // The dot is the voice, so a Character's part comes off to get the parameter.
    const auto& c = smoothmorph::characters[juce::jlimit (0, smoothmorph::numCharacters - 1, character)];
    pitchAttachment.setValueAsPartOfGesture (juce::jlimit (-24.0f, 24.0f, (float) (wantedPitch - (double) amount * c.pitchSemitones)));
    formantAttachment.setValueAsPartOfGesture (juce::jlimit (-12.0f, 12.0f, (float) (wantedFormant - (double) amount * c.formantSemitones)));
}

void VoicePad::mouseDown (const juce::MouseEvent& e)
{
    // Bypassed, the dot is drawn at You, where the sound is, and the voice it would move is not heard: the pad leaves it alone. To 1.2.5 a
    // click on the dot drawn at You moved the hidden voice there (Child: Pitch -7, Formant -4) (1.2.6).
    if (e.getNumberOfClicks() > 1 || e.mods.isPopupMenu() || bypassed)
        return;

    dragging = true;
    // On the dot, the voice is picked up where it is and moves by as far as the mouse does, so a click alone changes nothing. The dot is
    // drawn at the pad's edge when the voice is past it, and to 1.2.5 a click there set the voice to the edge (Monster, Pitch -20,
    // Formant -10: Pitch jumped to -16) (1.2.6). Anywhere else the voice goes where the click is.
    grabbed = e.position.getDistanceFrom (voiceAt()) <= dotRadius + 4.0f;
    gestureOpen = false;
    grabAt = e.position;
    grabPitch = voicePitch();
    grabFormant = voiceFormant();
    if (! grabbed)
    {
        openGesture();
        moveTo (e.position, e.mods.isShiftDown());
    }
}

void VoicePad::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    if (! grabbed)
    {
        moveTo (e.position, e.mods.isShiftDown());
        return;
    }
    if (e.position == grabAt && ! gestureOpen)
        return;
    openGesture();
    const auto m = map();
    const double perSemitone = m.getWidth() / (2.0 * pitchReach);
    double wantedPitch = grabPitch + (double) (e.position.x - grabAt.x) / perSemitone;
    double wantedFormant = grabFormant - (double) (e.position.y - grabAt.y) / perSemitone;
    if (e.mods.isShiftDown())
    {
        wantedPitch = std::round (wantedPitch);
        wantedFormant = std::round (wantedFormant);
    }
    setVoice (wantedPitch, wantedFormant);
}

void VoicePad::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        if (gestureOpen)
        {
            pitchAttachment.endGesture();
            formantAttachment.endGesture();
            gestureOpen = false;
        }
    }
}

void VoicePad::mouseDoubleClick (const juce::MouseEvent&)
{
    if (bypassed)
        return;
    pitchAttachment.setValueAsCompleteGesture (pitchParam.convertFrom0to1 (pitchParam.getDefaultValue()));
    formantAttachment.setValueAsCompleteGesture (formantParam.convertFrom0to1 (formantParam.getDefaultValue()));
}

void VoicePad::openGesture()
{
    if (gestureOpen)
        return;
    pitchAttachment.beginGesture();
    formantAttachment.beginGesture();
    gestureOpen = true;
}
