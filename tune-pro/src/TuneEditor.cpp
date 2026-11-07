#include "TuneEditor.h"

using namespace smoothtune;
using namespace tuneui;
using Kind = ParamSlider::Kind;

namespace
{
    const char* const defaultHint = "Drag a control to change it. Click a value to type one. Double-click resets.";

    juce::String noteName (int midi) { return midi < 0 ? juce::String ("no note") : juce::String (noteNames[midi % 12]) + juce::String (midi / 12 - 1); }

    constexpr int margin = 24, keyboardHeight = 88, heroGap = 3, mainHeight = 82, advancedHeight = 156, footerHeight = 32, footerBottom = 16;
}

// ------------------------------------------------------------------------------------------------- the live reading

// One line of text kept in a cached image, drawn again only when the words change.
class TextPlate final : public juce::Component
{
public:
    TextPlate (float sizeToUse, Weight weightToUse, juce::Colour colourToUse, juce::Justification justificationToUse)
        : size (sizeToUse), weight (weightToUse), colour (colourToUse), justification (justificationToUse)
    {
        setOpaque (true);
        setBufferedToImage (true);
        setInterceptsMouseClicks (false, false);
    }

    void setText (const juce::String& newText)
    {
        if (newText != text)
        {
            text = newText;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::ink);
        g.setColour (colour);
        g.setFont (font (weight, size));
        g.drawText (text, getLocalBounds(), justification, false);
    }

private:
    const float size;
    const Weight weight;
    const juce::Colour colour;
    const juce::Justification justification;
    juce::String text;
};

// The note being hit, large, and how far the voice was moved to get there, as a bar running out from the middle.
class TuneEditor::Reading final : public juce::Component,
                                  public Hinted
{
public:
    explicit Reading (PluginProcessor& p) : processor (p)
    {
        hint = "The note being hit, and how far your voice was moved to reach it.";
        setTitle ("Correction");
        setOpaque (true);
        addAndMakeVisible (words);
        addAndMakeVisible (note);
        words.setText ("No pitch");
        bypassParam = processor.apvts.getRawParameterValue ("bypass");
    }

    void resized() override
    {
        note.setBounds (getLocalBounds().removeFromRight (84));
        auto column = getLocalBounds().withTrimmedRight (104);
        words.setBounds (column.removeFromTop (getHeight() / 2 + 2).withTrimmedTop (4));
    }

    void advance (double dt)
    {
        const auto r = processor.getReadings();
        const bool bypassed = bypassParam->load (std::memory_order_relaxed) > 0.5f;
        const float target = r.voiced && ! bypassed ? juce::jlimit (-100.0f, 100.0f, r.correctionCents) : 0.0f;
        shownCents += (target - shownCents) * (1.0f - std::exp (-(float) dt / 0.035f));

        const int pixels = juce::roundToInt (shownCents * 1.3f);
        if (pixels != pixelsDrawn)
        {
            pixelsDrawn = pixels;
            repaint (barArea().expanded (1.0f, 4.0f).getSmallestIntegerContainer());
        }

        const int noteNow = r.voiced && ! bypassed ? r.targetNote : -1;
        if (noteNow != noteShown)
        {
            noteShown = noteNow;
            note.setText (noteNow >= 0 ? noteName (noteNow) : juce::String());
        }

        clock += dt;
        if (clock >= 0.2)
        {
            clock = 0.0;
            const int cents = juce::roundToInt (r.correctionCents);
            words.setText (bypassed ? juce::String ("Bypassed") : ! r.voiced ? juce::String ("No pitch") : std::abs (cents) < 1 ? juce::String ("On pitch")
                           : juce::String (cents > 0 ? "Moved up " : "Moved down ") + juce::String (std::abs (cents)) + " cents");
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::ink);

        const auto bar = barArea();
        g.setColour (tone::white.withAlpha (0.13f));
        g.fillRect (bar);

        const float mid = bar.getCentreX(), reach = bar.getWidth() * 0.5f * (float) pixelsDrawn / 130.0f;
        g.setColour (tone::coral);
        g.fillRect (juce::Rectangle<float> (juce::jmin (mid, mid + reach), bar.getY(), std::abs (reach), bar.getHeight()));

        g.setColour (tone::white);
        g.fillRect (juce::Rectangle<float> (mid - 1.0f, bar.getY() - 3.0f, 2.0f, bar.getHeight() + 6.0f));
    }

private:
    juce::Rectangle<float> barArea() const
    {
        const auto column = getLocalBounds().toFloat().withTrimmedRight (104.0f);
        return column.withY (column.getY() + column.getHeight() * 0.5f + 5.0f).withHeight (6.0f);
    }

    PluginProcessor& processor;
    std::atomic<float>* bypassParam = nullptr;
    TextPlate words { 13.0f, Weight::regular, tone::muted, juce::Justification::centredLeft };
    TextPlate note { 34.0f, Weight::medium, tone::white, juce::Justification::centredRight };
    float shownCents = 0.0f;
    int pixelsDrawn = 0, noteShown = -1;
    double clock = 1.0;
};

// ----------------------------------------------------------------------------------------------------- Advanced button

class TuneEditor::AdvancedButton final : public juce::Button,
                                         public Hinted
{
public:
    AdvancedButton() : juce::Button ("Advanced")
    {
        hint = "Voice, tuning, added vibrato and MIDI.";
        setClickingTogglesState (false);
    }

    void setState (float openAmount, int changesInside)
    {
        if (openAmount != amount || changesInside != changes)
        {
            amount = openAmount;
            changes = changesInside;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isOver, bool isDown) override
    {
        auto b = getLocalBounds().toFloat();
        const bool lit = isOver || isDown;

        juce::Path chevron;
        const float cx = b.getRight() - 8.0f, cy = b.getCentreY(), flip = 1.0f - 2.0f * amount;
        chevron.startNewSubPath (cx - 4.5f, cy - 2.5f * flip);
        chevron.lineTo (cx, cy + 2.5f * flip);
        chevron.lineTo (cx + 4.5f, cy - 2.5f * flip);
        g.setColour (lit ? tone::white : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        b.removeFromRight (22.0f);

        g.setColour (lit ? tone::white : tone::muted);
        g.setFont (font (Weight::medium, 13.5f));
        g.drawText (changes > 0 && amount < 0.5f ? "Advanced, " + juce::String (changes) + " changed" : juce::String ("Advanced"), b, juce::Justification::centredRight, false);
    }

private:
    float amount = 0.0f;
    int changes = 0;
};

// ------------------------------------------------------------------------------------------------------------- panel

TuneEditor::Panel::Panel (TuneEditor& e, PluginProcessor& p)
    : owner (e), processor (p),
      advancedButton (std::make_unique<AdvancedButton>()),
      presets (p),
      key (p.apvts, "key", {}, "The key your song is in."),
      scale (p.apvts, "scale", {}, "Which notes the voice is pulled to. Chromatic means all of them."),
      followKey (p.apvts, "followKey", "Follow Key", "Takes the key from the Key plug-in when it has a confident reading.", tone::white, tone::ink),
      retune (p.apvts, "retune", "Retune Speed", "How fast the voice is pulled to the note. Fast is the hard tuned effect.", Kind::lead),
      flex (p.apvts, "flex", "Flex Tune", "How far a note may drift before it gets corrected.", Kind::main),
      humanize (p.apvts, "humanize", "Humanize", "Keeps movement in held notes. The start of a note stays quick.", Kind::main),
      naturalVibrato (p.apvts, "naturalVibrato", "Natural Vibrato", "The vibrato already in the voice. -100 flattens it, +100 doubles it.", Kind::main),
      mix (p.apvts, "mix", "Mix", "The tuned voice against the original.", Kind::main),
      bypass (p.apvts, "bypass", "Bypass", "Turns the tuning off and keeps the same delay.", tone::white, tone::ink),
      engine (p.apvts, "engine", "Low Latency to sing through it live. High Quality for the final mix."),
      inputType (p.apvts, "inputType", "Input Type", "The voice's range. A lower range adds more delay."),
      midiOctave (p.apvts, "midiOctave", "MIDI Octave", "All Octaves: a held note counts in every octave. As Played: that note only."),
      throat (p.apvts, "throat", "Throat", "The size of the voice, without moving the pitch. 100% is unchanged.", Kind::compact),
      transpose (p.apvts, "transpose", "Transpose", "Moves the whole voice up or down, after tuning.", Kind::compact),
      tracking (p.apvts, "tracking", "Tracking", "Which sounds count as pitched. Raise it to tune rough, breathy sound too.", Kind::compact),
      reference (p.apvts, "reference", "Reference A4", "Where A sits. 440 Hz unless the song was tuned differently.", Kind::compact),
      vibratoAmount (p.apvts, "vibratoAmount", "Amount", "How wide the added vibrato is. Zero adds none.", Kind::compact),
      vibratoRate (p.apvts, "vibratoRate", "Rate", "How fast the added vibrato moves.", Kind::compact),
      vibratoDelay (p.apvts, "vibratoDelay", "Delay", "How long a note holds before the added vibrato starts.", Kind::compact),
      formantPreserve (p.apvts, "formantPreserve", "Formant Preserve", "On keeps the character of the voice. Off lets it follow the pitch.", tone::white, tone::ink),
      classic (p.apvts, "classic", "Classic Mode", "The plain fast snap. Ignores Retune Speed, Flex, Humanize and vibrato.", tone::white, tone::ink),
      midiTargets (p.apvts, "midiTargets", "MIDI Targets", "Held MIDI notes become the targets. With none held, nothing is tuned.", tone::white, tone::ink)
{
    setOpaque (true);
    setBufferedToImage (true);
    setInterceptsMouseClicks (false, true);

    addAndMakeVisible (*advancedButton);
    advancedButton->onClick = [this] { owner.setAdvancedOpen (! owner.isAdvancedOpen(), true); };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &presets, &key, &scale, &followKey, &retune, &flex, &humanize, &naturalVibrato, &mix, &bypass, &engine, &latency, &hintLine, &advanced })
        addAndMakeVisible (c);

    advanced.setInterceptsMouseClicks (false, true);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &inputType, &throat, &transpose, &formantPreserve, &tracking, &reference, &classic, &vibratoAmount, &vibratoRate, &vibratoDelay, &midiTargets, &midiOctave })
        advanced.addAndMakeVisible (c);

    latency.setFont (font (Weight::regular, 13.0f));
    latency.setColour (juce::Label::textColourId, tone::muted);
    latency.setInterceptsMouseClicks (false, false);
    hintLine.setFont (font (Weight::regular, 13.0f));
    hintLine.setColour (juce::Label::textColourId, tone::muted);
    hintLine.setInterceptsMouseClicks (false, false);
    hintLine.setText (defaultHint, juce::dontSendNotification);
}

TuneEditor::Panel::~Panel() = default;

void TuneEditor::Panel::paint (juce::Graphics& g)
{
    g.fillAll (tone::ink);
    g.setColour (tone::white);
    g.setFont (font (Weight::medium, 24.0f));
    g.drawText ("Tune Pro", juce::Rectangle<int> (margin, 0, 120, headerHeight), juce::Justification::centredLeft, false);
}

void TuneEditor::Panel::AdvancedArea::paint (juce::Graphics& g)
{
    g.setColour (tone::white);
    g.setFont (font (Weight::medium, 13.5f));
    g.drawText ("Voice", voiceTitle, juce::Justification::centredLeft, false);
    g.drawText ("Tuning", tuningTitle, juce::Justification::centredLeft, false);
    g.drawText ("Added vibrato", vibratoTitle, juce::Justification::centredLeft, false);
    g.drawText ("MIDI", midiTitle, juce::Justification::centredLeft, false);
}

void TuneEditor::Panel::layout (float amount)
{
    // Header
    presets.setBounds (150, 19, 196, 32);
    key.setBounds (362, 19, 64, 32);
    scale.setBounds (432, 19, 158, 32);
    followKey.setBounds (596, 19, 92, 32);

    // From the bottom up. Advanced opens upward and the display gives up the room.
    const int footerY = designHeight - footerBottom - footerHeight;
    bypass.setBounds (margin, footerY, 76, footerHeight);
    engine.setBounds (margin + 88, footerY, 222, footerHeight);
    latency.setBounds (margin + 316, footerY, 92, footerHeight);
    hintLine.setBounds (margin + 416, footerY, designWidth - 2 * margin - 416 - 170, footerHeight);
    advancedButton->setBounds (designWidth - margin - 164, footerY, 164, footerHeight);

    const int openHeight = juce::roundToInt ((float) advancedHeight * amount);
    const int advancedY = footerY - 16 - openHeight;
    const int mainBottom = advancedY - juce::roundToInt (16.0f * amount);
    advanced.setBounds (margin, advancedY, designWidth - 2 * margin, openHeight);
    advanced.setVisible (openHeight > 0);

    const int mainY = mainBottom - mainHeight;
    const int gap = 36, leadWidth = 330;
    const int cell = (designWidth - 2 * margin - leadWidth - 4 * gap) / 4;
    int x = margin;
    retune.setBounds (x, mainY, leadWidth, mainHeight);
    x += leadWidth + gap;
    for (auto* s : { &flex, &humanize, &naturalVibrato, &mix })
    {
        s->setBounds (x, mainY + mainHeight - 64, cell, 62);
        x += cell + gap;
    }

    keyboardBottom = mainY - 18;
    keyboardTop = keyboardBottom - keyboardHeight;

    // Advanced lays itself out at full height and is simply clipped while it opens.
    const int w = 130, g2 = 20, groupGap = 34, titleH = 22, rowH = 52, rowGap = 12;
    const int row1 = titleH + 6, row2 = row1 + rowH + rowGap;
    const auto group = [&] (int gx, juce::Rectangle<int>& title, juce::Component* a, juce::Component* b, juce::Component* c, juce::Component* d)
    {
        title = { gx, 0, w * 2, titleH };
        if (a != nullptr) a->setBounds (gx, row1, w, rowH);
        if (b != nullptr) b->setBounds (gx + w + g2, row1, w, rowH);
        if (c != nullptr) c->setBounds (gx, row2, w, rowH);
        if (d != nullptr) d->setBounds (gx + w + g2, row2, w, rowH);
    };

    int gx = 0;
    group (gx, advanced.voiceTitle, &throat, &transpose, &inputType, nullptr);
    formantPreserve.setBounds (gx + w + g2, row2 + rowH - 34, w, 34);
    gx += 2 * w + g2 + groupGap;
    group (gx, advanced.tuningTitle, &tracking, &reference, nullptr, nullptr);
    classic.setBounds (gx, row2 + rowH - 34, w, 34);
    gx += 2 * w + g2 + groupGap;
    group (gx, advanced.vibratoTitle, &vibratoAmount, &vibratoRate, &vibratoDelay, nullptr);
    gx += 2 * w + g2 + groupGap;
    advanced.midiTitle = { gx, 0, w, titleH };
    midiTargets.setBounds (gx, row1 + rowH - 34, w, 34);
    midiOctave.setBounds (gx, row2, w, rowH);
}

int TuneEditor::Panel::countChanges() const
{
    int changes = 0;
    for (auto* s : { &throat, &transpose, &tracking, &reference, &vibratoAmount, &vibratoRate, &vibratoDelay })
        changes += s->isChanged() ? 1 : 0;
    for (auto* t : { &formantPreserve, &classic, &midiTargets })
        changes += t->isChanged() ? 1 : 0;
    for (auto* m : { &inputType, &midiOctave })
        changes += m->isChanged() ? 1 : 0;
    return changes;
}

// ------------------------------------------------------------------------------------------------------------- editor

TuneEditor::TuneEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), pitch (p), keyboard (p),
      panel ((setLookAndFeel (&lookAndFeel), *this), p)
{
    setOpaque (true);
    reading = std::make_unique<Reading> (p);
    addAndMakeVisible (panel);
    addAndMakeVisible (pitch);
    addAndMakeVisible (keyboard);
    addAndMakeVisible (*reading);
    keyboard.onHint = [this] (const juce::String& text) { showHint (text); };
    addMouseListener (this, true);

    const double ratio = (double) designWidth / (double) designHeight;
    setResizable (true, true);
    setResizeLimits (PluginProcessor::minWindowWidth, juce::roundToInt (PluginProcessor::minWindowWidth / ratio),
                     PluginProcessor::maxWindowWidth, juce::roundToInt (PluginProcessor::maxWindowWidth / ratio));
    getConstrainer()->setFixedAspectRatio (ratio);

    advancedAmount = isAdvancedOpen() ? 1.0f : 0.0f;
    const int savedWidth = juce::jlimit (PluginProcessor::minWindowWidth, PluginProcessor::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    constructed = true;
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    resized();   // the limits above may already have set this size, before the panel was ready

    tick (1.0 / maxFramesPerSecond);
    startTimerHz (5);
}

TuneEditor::~TuneEditor()
{
    stopTimer();
    vblank.reset();
    removeMouseListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    reading.reset();
    setLookAndFeel (nullptr);
}

void TuneEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::ink);
}

void TuneEditor::placeInstrument()
{
    const float s = (float) getWidth() / (float) designWidth;
    const int heroTop = juce::roundToInt ((float) headerHeight * s);
    const int keysTop = juce::roundToInt ((float) panel.keyboardTop * s);
    const int keysBottom = juce::roundToInt ((float) panel.keyboardBottom * s);

    pitch.setTypeScale (s);
    keyboard.setTypeScale (s);
    pitch.setBounds (0, heroTop, getWidth(), keysTop - heroGap - heroTop);
    keyboard.setBounds (0, keysTop, getWidth(), keysBottom - keysTop);
}

void TuneEditor::resized()
{
    if (! constructed)
        return;

    panel.setBounds (0, 0, designWidth, designHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));
    panel.layout (advancedAmount);
    placeInstrument();
    reading->setBounds (designWidth - margin - 372, 14, 372, 42);
    reading->setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));
    processor.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

void TuneEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processor.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);

    if (! animate)
    {
        advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
        panel.layout (advancedAmount);
        placeInstrument();
        panel.advancedButton->setState (advancedAmount, panel.countChanges());
        panel.repaint();
    }
    else
    {
        stillFor = 0.0;
        timerCallback();
    }
}

void TuneEditor::showHint (const juce::String& text)
{
    panel.hintLine.setText (text.isNotEmpty() ? text : juce::String (defaultHint), juce::dontSendNotification);
}

void TuneEditor::hintFromComponent (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (c == &keyboard)
            return;     // it words its own hint, note by note
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            showHint (hinted->hint);
            return;
        }
    }
    showHint ({});
}

void TuneEditor::mouseEnter (const juce::MouseEvent& e)
{
    hintFromComponent (e.eventComponent);
}

void TuneEditor::mouseExit (const juce::MouseEvent&)
{
    showHint ({});
}

void TuneEditor::previewHover (const juce::String& paramId)
{
    if (auto* c = findControl (paramId))
    {
        if (auto* slider = dynamic_cast<ParamSlider*> (c))
            slider->setForcedHover (true);
        if (auto* toggle = dynamic_cast<ParamToggle*> (c))
            toggle->setForcedHover (true);
        if (auto* hinted = dynamic_cast<Hinted*> (c))
            showHint (hinted->hint);
    }
}

juce::Component* TuneEditor::findControl (const juce::String& id)
{
    if (auto* c = panel.findChildWithID (id))
        return c;
    return panel.advanced.findChildWithID (id);
}

int TuneEditor::getNumControls() const noexcept
{
    int found = 0;
    const auto count = [&found] (const juce::Component& holder)
    {
        for (auto* c : holder.getChildren())
            if (dynamic_cast<const ParamSlider*> (c) != nullptr || dynamic_cast<const ParamToggle*> (c) != nullptr
                || dynamic_cast<const ParamSegments*> (c) != nullptr || dynamic_cast<const ParamMenu*> (c) != nullptr)
                ++found;
    };
    count (panel);
    count (panel.advanced);
    return found;
}

void TuneEditor::tick (double dtSeconds)
{
    bool moved = pitch.advance (dtSeconds);
    keyboard.advance();
    reading->advance (dtSeconds);
    panel.presets.refresh();

    // Advanced glides open and shut, in about a fifth of a second.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;
    if (advancedAmount != target)
    {
        const float next = advancedAmount + (target - advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel.layout (advancedAmount);
        placeInstrument();
        panel.advancedButton->setState (advancedAmount, panel.countChanges());
        moved = true;
    }

    // Things that change rarely.
    if (--slowCountdown <= 0)
    {
        slowCountdown = 15;
        panel.advancedButton->setState (advancedAmount, panel.countChanges());
        const double rate = processor.getSampleRate();
        const auto text = rate > 0.0 ? "Adds " + juce::String (processor.getLatencySamples() * 1000.0 / rate, 1) + " ms" : juce::String();
        panel.latency.setText (text, juce::dontSendNotification);
    }

    stillFor = moved ? 0.0 : stillFor + dtSeconds;
}

void TuneEditor::timerCallback()
{
    // Nothing has been moving, so look only a few times a second. Move again as soon as there is something to show.
    tick (0.2);

    if (stillFor < 1.0 && vblank == nullptr)
    {
        stopTimer();
        lastFrameTime = 0.0;
        frameBudget = 0.0;
        sinceDrawn = 0.0;

        // At most one frame per cap on any screen rate, spending whole refreshes against a running budget.
        vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
        {
            const double elapsed = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / maxFramesPerSecond;
            lastFrameTime = timestampSeconds;
            sinceDrawn += elapsed;
            frameBudget = juce::jmin (frameBudget + elapsed, 0.05);

            if (frameBudget + 0.001 < 1.0 / maxFramesPerSecond)
                return;

            frameBudget -= 1.0 / maxFramesPerSecond;
            const double step = sinceDrawn;
            sinceDrawn = 0.0;
            tick (step);

            if (stillFor >= 1.0)
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TuneEditor> (this)]
                {
                    if (safe != nullptr && safe->stillFor >= 1.0)
                    {
                        safe->vblank.reset();
                        safe->startTimerHz (5);
                    }
                });
            }
        });
    }
}

juce::String TuneEditor::readingText() const
{
    const auto r = processor.getReadings();
    auto left = juce::String ("Active Key ") + noteNames[juce::jlimit (0, 11, r.activeKey)] + " " + scaleNames[juce::jlimit (0, numScales - 1, r.activeScale)];
    if (! r.voiced)
        return left + "  |  no pitch";
    return left + "  |  In " + juce::String (r.detectedHz, 1) + " Hz  |  Out " + juce::String (r.outputHz, 1) + " Hz  |  Target " + noteName (r.targetNote)
           + "  |  Correction " + juce::String (juce::roundToInt (r.correctionCents)) + " cents";
}
