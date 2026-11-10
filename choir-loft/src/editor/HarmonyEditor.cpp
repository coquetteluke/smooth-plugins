#include "HarmonyEditor.h"

using namespace ui;
using namespace smoothharmony;

namespace
{
    // The grid everything sits on.
    constexpr int margin = 24;
    constexpr int rowOneY = 18, rowOneHeight = 36;
    constexpr int rowTwoY = 66, rowTwoHeight = 32;
    constexpr int stageY = 110, stageTallest = 346;
    constexpr int gap = 22;
    constexpr int stripHeight = 202;
    constexpr int barHeight = 28, barGap = 3;
    constexpr int globalHeight = 32;
    constexpr int bottomHeight = 28;
    constexpr int advancedHeight = 172, advancedTitle = 22;

    // The pitch history moves every 5 ms, but a line only advances about 4 pixels between frames at this rate, and the eye reads that as smooth.
    // More frames only cost more CPU. Almost all of a frame's cost is the system presenting it, which depends on how often and not on how much was drawn.
    constexpr double maxFramesPerSecond = 24.0;
    // With nothing moving and no mouse over the window, it only has to notice a change from automation or from the host.
    constexpr double restingFramesPerSecond = 8.0;

    const juce::Colour neutralBar { 0xffd9c9d4 };   // controls that are not one singer's

    const juce::String restingHint = "Click singers to choose who you hear. Drag one to move it.";

    int columnWidth() { return (HarmonyEditor::designWidth - 2 * margin - 3 * gap) / 4; }
    int columnX (int column) { return margin + column * (columnWidth() + gap); }
}

// ---- preset name with next and previous arrows ----------------------------------------------------------------

class HarmonyEditor::PresetBox final : public juce::Component,
                                       public Hinted
{
public:
    explicit PresetBox (PluginProcessor& p) : processor (p)
    {
        hint = "Starting points. Click the name for the full list. They leave Key, Scale and Input Type alone.";
        setComponentID ("presets");
        setTitle ("Presets");
    }

    void refresh()
    {
        if (processor.getCurrentProgram() != shown)
        {
            shown = processor.getCurrentProgram();
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::text.withAlpha (zone != none ? 0.095f : 0.055f));
        g.fillRoundedRectangle (b, 7.0f);

        g.setColour (tone::text);
        g.setFont (font (Weight::regular, 14.0f));
        g.drawText (processor.getProgramName (processor.getCurrentProgram()), b.reduced (arrowWidth, 0.0f), juce::Justification::centred, true);

        const auto chevron = [&] (float cx, float direction, bool lit)
        {
            juce::Path p;
            p.startNewSubPath (cx - 2.5f * direction, b.getCentreY() - 5.0f);
            p.lineTo (cx + 2.5f * direction, b.getCentreY());
            p.lineTo (cx - 2.5f * direction, b.getCentreY() + 5.0f);
            g.setColour (lit ? tone::text : tone::muted);
            g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        chevron (b.getX() + arrowWidth * 0.5f, -1.0f, zone == previous);
        chevron (b.getRight() - arrowWidth * 0.5f, 1.0f, zone == next);
    }

    void mouseMove (const juce::MouseEvent& e) override  { setZone (zoneAt (e.position.x)); }
    void mouseExit (const juce::MouseEvent&) override    { setZone (none); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto hit = zoneAt (e.position.x);
        const int count = processor.getNumPrograms();

        if (hit == previous)
            choose ((processor.getCurrentProgram() + count - 1) % count);
        else if (hit == next)
            choose ((processor.getCurrentProgram() + 1) % count);
        else
            openMenu();
    }

    void choose (int index)
    {
        processor.setCurrentProgram (index);
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true));
        refresh();
    }

private:
    enum Zone { none, previous, name, next };
    static constexpr float arrowWidth = 30.0f;

    Zone zoneAt (float x) const
    {
        if (x < arrowWidth)                       return previous;
        if (x > (float) getWidth() - arrowWidth)  return next;
        return name;
    }

    void setZone (Zone z)
    {
        if (z != zone)
        {
            zone = z;
            repaint();
        }
    }

    void openMenu()
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());

        for (int i = 0; i < processor.getNumPrograms(); ++i)
            menu.addItem (i + 1, processor.getProgramName (i), true, i == processor.getCurrentProgram());

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (28),
                            [safe = juce::Component::SafePointer<PresetBox> (this)] (int result)
                            {
                                if (safe != nullptr && result > 0)
                                    safe->choose (result - 1);
                            });
    }

    PluginProcessor& processor;
    int shown = -1;
    Zone zone = none;
};

// ---- the Advanced switch ----------------------------------------------------------------------------------------

class HarmonyEditor::AdvancedHeader final : public juce::Component,
                                            public Hinted
{
public:
    explicit AdvancedHeader (HarmonyEditor& e) : editor (e)
    {
        hint = "Vibrato speed, glide, breath, consonants, throat, chords and how your voice is read.";
        setComponentID ("advanced");
        setTitle ("Advanced");
        setRepaintsOnMouseActivity (true);
    }

    void setAmount (float a)
    {
        if (a != amount)
        {
            amount = a;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const bool lit = isMouseOver();
        const auto b = getLocalBounds().toFloat();
        const juce::Point<float> c (b.getX() + 6.0f, b.getCentreY());

        // A chevron that turns as the section opens.
        juce::Path p;
        p.startNewSubPath (-2.5f, -5.0f);
        p.lineTo (2.5f, 0.0f);
        p.lineTo (-2.5f, 5.0f);
        g.setColour (lit ? tone::text : tone::muted);
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::rotation (amount * juce::MathConstants<float>::halfPi).translated (c));

        g.setFont (font (Weight::medium, 13.0f));
        g.drawText ("Advanced", b.withTrimmedLeft (20.0f), juce::Justification::centredLeft, false);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        editor.setAdvancedOpen (! editor.isAdvancedOpen(), true);
    }

private:
    HarmonyEditor& editor;
    float amount = 0.0f;
};

// ---- one voice --------------------------------------------------------------------------------------------------

HarmonyEditor::VoiceStrip::VoiceStrip (PluginProcessor& p, int index)
    : head (p.apvts, "voice" + juce::String (index + 1) + "On", "Voice " + juce::String (index + 1), "Click to hear this voice or not.", tone::voice[index]),
      interval (p.apvts, "voice" + juce::String (index + 1) + "Interval", index, tone::voice[index]),
      level (p.apvts, "voice" + juce::String (index + 1) + "Level", "Level", "How loud this voice is. All the way down turns it off.", tone::voice[index]),
      pan (p.apvts, "voice" + juce::String (index + 1) + "Pan", "Pan", "Where this voice sits, left to right. You can also drag it on the stage.", tone::voice[index]),
      drift (p.apvts, "voice" + juce::String (index + 1) + "Drift", "Drift", "How much this voice slowly wanders in pitch, timing and level. Higher sounds looser.", tone::voice[index]),
      tone (p.apvts, "voice" + juce::String (index + 1) + "Tone", "Tone", "Darker or brighter than the lead. Giving each voice its own helps them sound like different people.", tone::voice[index]),
      processor (p),
      voice (index),
      colour (tone::voice[index])
{
    tone.setSigned (true);
    setTitle ("Voice " + juce::String (index + 1));
    addAndMakeVisible (head);
    addAndMakeVisible (interval);
    addAndMakeVisible (level);
    addAndMakeVisible (pan);
    addAndMakeVisible (drift);
    addAndMakeVisible (tone);

    // A voice past the count comes in with a click on its name, and only after that does a click switch it.
    head.onClickFirst = [this]
    {
        if (onShown)
            return false;

        auto* more = processor.apvts.getParameter ("moreVoices");
        auto* count = processor.apvts.getParameter ("voices");
        count->beginChangeGesture();
        more->beginChangeGesture();
        count->setValueNotifyingHost (count->convertTo0to1 ((float) juce::jmin (voice + 1, 4)));
        more->setValueNotifyingHost (more->convertTo0to1 ((float) juce::jmax (0, voice + 1 - 4)));
        count->endChangeGesture();
        more->endChangeGesture();
        return true;
    };
}

void HarmonyEditor::VoiceStrip::resized()
{
    auto b = getLocalBounds();
    head.setBounds (b.removeFromTop (30));
    interval.setBounds (b.removeFromTop (46));
    b.removeFromTop (6);

    for (auto* slider : { (juce::Component*) &level, (juce::Component*) &pan, (juce::Component*) &drift, (juce::Component*) &tone })
    {
        slider->setBounds (b.removeFromTop (barHeight));
        b.removeFromTop (barGap);
    }
}

void HarmonyEditor::VoiceStrip::setSinging (const juce::String& note, bool isLive, bool isOn, bool isCompact)
{
    const bool heard = head.isHeard();
    const float size = isCompact ? 17.0f : 20.0f;

    if (! isOn)
        head.setRight ("Click to add", tone::muted, 13.0f);
    else if (! heard)
        head.setRight ("Off", tone::muted, 14.0f);
    else
        head.setRight (note, isLive ? colour : colour.withAlpha (0.8f), size);

    head.setCompact (isCompact);
    interval.setCompact (isCompact);

    const float alpha = ! isOn ? 0.42f : (heard ? 1.0f : 0.5f);

    if (getAlpha() != alpha)
        setAlpha (alpha);

    onShown = isOn;
    compactShown = isCompact;
}

// ---- the panel --------------------------------------------------------------------------------------------------

HarmonyEditor::Panel::Panel (HarmonyEditor& e, PluginProcessor& p)
    : owner (e),
      processor (p),
      stage (p),
      voicesCount (p.apvts, "How many voices sing with the lead. Above 4, the voices turn down together so it stays about as loud."),
      modePicker (p.apvts, "mode", { "In Key", "MIDI Chord", "Fixed", "Chord Tones", "MIDI Channel" }, 0, "Where the voices get their notes."),
      leadSwitch (p.apvts, "leadOn", "", "Click to hear the lead or not. With it off you hear only the harmonies.", tone::text),
      bypass (p.apvts, "bypass", "Bypass", "Hear your vocal with nothing done to it. The timing stays the same."),
      keyField (p.apvts, "key", "Key", "The key of your song. Voices stay on its notes in In Key mode."),
      scaleField (p.apvts, "scale", "Scale", "The scale of your song. Rows on the display show its notes."),
      chordRootField (p.apvts, "chordRoot", "Chord", "The chord the voices sing from in Chord Tones mode."),
      chordTypeField (p.apvts, "chordType", "Type", "The kind of chord the voices sing from in Chord Tones mode."),
      inputType (p.apvts, "inputType", "Voice range", "Your voice's range. Lower ranges take a little longer to read."),
      followKey (p.apvts, "followKey", "Follow Key", "Takes the key from Key on the same song, when it is sure of it."),
      chordHold (p.apvts, "chordHold", "Chord Hold", "Keeps a MIDI chord after the keys come up, until the next new note."),
      humanize (p.apvts, "humanize", "Humanize", "How much each voice differs from a machine, note by note. 0 is exact.", neutralBar),
      vibrato (p.apvts, "vibrato", "Vibrato", "How much the voices shake in pitch. They start it a moment into each note.", neutralBar),
      leadLevel (p.apvts, "leadLevel", "Lead", "How loud your own voice is. All the way down leaves only the harmonies.", neutralBar),
      harmonyLevel (p.apvts, "harmonyLevel", "Harmony", "How loud all the voices are together.", neutralBar),
      mix (p.apvts, "mix", "Mix", "0 is your vocal alone, 100 is the lead and voices.", neutralBar),
      vibratoRate (p.apvts, "vibratoRate", "Vibrato Rate", "How fast the vibrato is. Each voice runs a little apart from the others.", neutralBar),
      glide (p.apvts, "glide", "Glide", "How long the voices take to slide to a new note.", neutralBar),
      followLead (p.apvts, "followLead", "Follow Lead", "How much of your own pitch wander the voices copy. 0 sits right on the note.", neutralBar),
      breath (p.apvts, "breath", "Breath", "Adds each voice's own breathiness.", neutralBar),
      consonants (p.apvts, "consonants", "Consonants", "How much of your S and T sounds each voice carries.", neutralBar),
      scatter (p.apvts, "consonantScatter", "Scatter", "0 copies your consonants. Higher gives each voice its own, a little later and softer.", neutralBar),
      leadPan (p.apvts, "leadPan", "Lead Pan", "Where your own voice sits, left to right. You can also drag it on the stage.", neutralBar),
      tracking (p.apvts, "tracking", "Tracking", "Which sounds count as singing. Low is choosy, high lets rough or breathy sound in.", neutralBar),
      reference (p.apvts, "reference", "Reference A4", "The tuning of A. Leave it at 440 unless your song is tuned differently.", neutralBar)
{
    presets = std::make_unique<PresetBox> (p);
    advancedHeader = std::make_unique<AdvancedHeader> (e);

    addAndMakeVisible (stage);
    addAndMakeVisible (*presets);
    addAndMakeVisible (voicesCount);
    addAndMakeVisible (leadSwitch);
    addAndMakeVisible (modePicker);
    addAndMakeVisible (bypass);
    addAndMakeVisible (*advancedHeader);

    modePicker.setCellHints ({ "Voices stay in your key, counting the interval on its notes.",
                               "Hold chords on this track's MIDI, and the voices sing them.",
                               "Voices stay a set number of semitones from the lead, whatever the key.",
                               "Voices sing the nearest note of the chord you pick.",
                               "Voice 1 sings MIDI channel 1, voice 2 sings channel 2, and so on." });

    for (int v = 0; v < PluginProcessor::maxVoices; ++v)
    {
        strips[v] = std::make_unique<VoiceStrip> (p, v);
        addAndMakeVisible (*strips[v]);
        throat[v] = std::make_unique<BarSlider> (p.apvts, "voice" + juce::String (v + 1) + "Throat", "Voice " + juce::String (v + 1),
                                                 "Above 100 sounds like a longer throat. Spreading the voices apart helps them sound like different people.", tone::voice[v]);
        addChildComponent (*throat[v]);
    }

    for (auto* c : { (juce::Component*) &humanize, (juce::Component*) &vibrato, (juce::Component*) &leadLevel, (juce::Component*) &harmonyLevel, (juce::Component*) &mix })
        addAndMakeVisible (c);

    for (auto* c : { (juce::Component*) &vibratoRate, (juce::Component*) &glide, (juce::Component*) &followLead, (juce::Component*) &breath, (juce::Component*) &consonants,
                     (juce::Component*) &scatter, (juce::Component*) &leadPan, (juce::Component*) &tracking, (juce::Component*) &reference, (juce::Component*) &inputType })
        addChildComponent (c);

    // These change places with the Mode. See placeForMode.
    for (auto* c : { (juce::Component*) &keyField, (juce::Component*) &scaleField, (juce::Component*) &chordRootField, (juce::Component*) &chordTypeField,
                     (juce::Component*) &followKey, (juce::Component*) &chordHold })
        addChildComponent (c);

    // While Follow Key is on, the key comes from Key and a pick here would change nothing you can see or hear.
    // So a Key or Scale picked by hand turns Follow Key off: the last thing you chose is what the voices use.
    auto* followParam = p.apvts.getParameter ("followKey");
    keyField.onUserPick = scaleField.onUserPick = [followParam]
    {
        if (followParam->getValue() >= 0.5f)
        {
            followParam->beginChangeGesture();
            followParam->setValueNotifyingHost (0.0f);
            followParam->endChangeGesture();
        }
    };

    hintLine.setFont (font (Weight::regular, 13.0f));
    hintLine.setColour (juce::Label::textColourId, tone::muted);
    hintLine.setJustificationType (juce::Justification::centredRight);
    hintLine.setInterceptsMouseClicks (false, false);
    hintLine.setBorderSize ({});
    hintLine.setText (restingHint, juce::dontSendNotification);
    addAndMakeVisible (hintLine);

    stage.onHint = [this] (const juce::String& text) { showHint (text.isNotEmpty() ? text : restingHint); };

    addMouseListener (this, true);
}

HarmonyEditor::Panel::~Panel()
{
    removeMouseListener (this);
}

void HarmonyEditor::Panel::paint (juce::Graphics& g)
{
    g.fillAll (tone::base);

    g.setColour (tone::text);
    g.setFont (font (Weight::medium, 26.0f));
    g.drawText ("Choir Loft", juce::Rectangle<int> (margin, rowOneY, 130, rowOneHeight), juce::Justification::centredLeft, false);

    g.setColour (tone::muted);
    g.setFont (font (Weight::regular, 13.0f));
    g.drawText ("Voices", voicesCount.getBounds().translated (-54, 0).withWidth (48), juce::Justification::centredRight, false);

    if (advancedAmount > 0.01f)
    {
        const int top = stage.getBottom() + 16 + stripHeight + 12;
        const juce::StringArray titles { "Throat", "How they sing", "Consonants and lead", "Input and chords" };
        g.setFont (font (Weight::medium, 13.0f));
        g.setColour (tone::muted.withMultipliedAlpha (advancedAmount));

        for (int c = 0; c < 4; ++c)
            g.drawText (titles[c], juce::Rectangle<int> (columnX (c), top, columnWidth(), advancedTitle - 4), juce::Justification::centredLeft, false);
    }
}

void HarmonyEditor::Panel::placeForMode (int mode)
{
    // Three places by the display and three in Advanced. Whatever this Mode uses sits by the display. The rest stays one click away in Advanced.
    const bool chordTones = mode == modeChord, midi = mode == modeMidi || mode == modeChannel;

    auto* topA = chordTones ? &chordRootField : &keyField;
    auto* topB = chordTones ? &chordTypeField : &scaleField;
    auto* restA = chordTones ? &keyField : &chordRootField;
    auto* restB = chordTones ? &scaleField : &chordTypeField;
    auto* topC = midi ? &chordHold : &followKey;
    auto* restC = midi ? &followKey : &chordHold;

    const int right = designWidth - margin;
    const int followWidth = 96, scaleWidth = 190, keyWidth = 92;
    topC->setBounds (right - followWidth, rowTwoY, followWidth, rowTwoHeight);
    topB->setBounds (right - followWidth - 8 - scaleWidth, rowTwoY, scaleWidth, rowTwoHeight);
    topA->setBounds (right - followWidth - 8 - scaleWidth - 8 - keyWidth, rowTwoY, keyWidth, rowTwoHeight);

    // Key and Scale do nothing in the MIDI modes. They only colour the rows.
    const float useful = mode == modeScale || chordTones ? 1.0f : 0.55f;
    topA->setAlpha (useful);
    topB->setAlpha (useful);
    topC->setAlpha (mode == modeFixed || chordTones ? 0.55f : 1.0f);
    topA->setVisible (true);
    topB->setVisible (true);
    topC->setVisible (true);

    const bool open = advancedAmount > 0.01f;
    restA->setVisible (open);
    restB->setVisible (open);
    restC->setVisible (open);
    restA->setAlpha (advancedAmount);
    restB->setAlpha (advancedAmount);
    restC->setAlpha (advancedAmount);

    const int top = stage.getBottom() + 16 + stripHeight + 12 + advancedTitle;
    const int rowStep = barHeight + barGap;
    restC->setBounds (columnX (2), top + 3 * rowStep, columnWidth(), barHeight);
    restA->setBounds (columnX (3), top + 3 * rowStep, columnWidth(), barHeight);
    restB->setBounds (columnX (3), top + 4 * rowStep, columnWidth(), barHeight);
}

void HarmonyEditor::Panel::resized()
{
    const int right = designWidth - margin;
    const int openHeight = advancedHeight + 16;
    const int stageHeight = stageTallest - juce::roundToInt (advancedAmount * (float) openHeight);

    presets->setBounds (margin + 134, rowOneY, 270, rowOneHeight);
    bypass.setBounds (right - 84, rowOneY, 84, rowOneHeight);
    voicesCount.setBounds (right - 84 - 16 - 232, rowOneY, 232, rowOneHeight);

    modePicker.setBounds (margin, rowTwoY, 468, rowTwoHeight);

    stage.setBounds (margin, stageY, designWidth - 2 * margin, stageHeight);

    const int stripsY = stage.getBottom() + 16;

    // Four cards up to four voices, with the spare ones dimmed. Above that, one card for each singer, in narrower columns, so all of them stay in view.
    cardsShown = juce::jmax (4, processor.voiceCount());
    const int cardGap = cardsShown > 4 ? 14 : gap;
    const float cardWidth = (float) (designWidth - 2 * margin - (cardsShown - 1) * cardGap) / (float) cardsShown;

    for (int v = 0; v < PluginProcessor::maxVoices; ++v)
    {
        const int x = margin + juce::roundToInt ((float) v * (cardWidth + (float) cardGap));
        strips[v]->setBounds (x, stripsY, juce::roundToInt (cardWidth), stripHeight);
        strips[v]->setVisible (v < cardsShown);
    }

    // Advanced, between the voices and the row of overall controls
    const int advancedTop = stripsY + stripHeight + 12 + advancedTitle;
    const int rowStep = barHeight + barGap;
    const bool open = advancedAmount > 0.01f;

    const auto place = [&] (juce::Component& c, int column, int row)
    {
        c.setBounds (columnX (column), advancedTop + row * rowStep, columnWidth(), barHeight);
        c.setVisible (open);
        c.setAlpha (advancedAmount);
    };

    // Throat: one column of four, or two side by side when there are eight to show.
    for (int v = 0; v < PluginProcessor::maxVoices; ++v)
    {
        place (*throat[v], 0, v % 4);

        if (cardsShown > 4)
        {
            const int half = (columnWidth() - 12) / 2;
            throat[v]->setBounds (columnX (0) + (v >= 4 ? half + 12 : 0), throat[v]->getY(), half, barHeight);
        }
        else
        {
            throat[v]->setVisible (open && v < 4);
        }
    }

    place (vibratoRate, 1, 0);
    place (glide, 1, 1);
    place (followLead, 1, 2);
    place (breath, 1, 3);
    place (consonants, 2, 0);
    place (scatter, 2, 1);
    place (leadPan, 2, 2);
    place (inputType, 3, 0);
    place (tracking, 3, 1);
    place (reference, 3, 2);

    placeForMode (modeShown >= 0 ? modeShown : modePicker.getSelectedValue());

    // The overall controls and the hint keep their place at the bottom.
    const int globalY = designHeight - 18 - bottomHeight - 12 - globalHeight;
    int x = margin;
    const int fifth = (designWidth - 2 * margin - 4 * 12) / 5;

    for (auto* c : { (juce::Component*) &humanize, (juce::Component*) &vibrato, (juce::Component*) &leadLevel, (juce::Component*) &harmonyLevel, (juce::Component*) &mix })
    {
        c->setBounds (x, globalY, fifth, globalHeight);

        // The lead's own switch, the same dot a voice has, just ahead of its level.
        if (c == &leadLevel)
        {
            leadSwitch.setBounds (x, globalY, 16, globalHeight - 10);
            c->setBounds (x + 18, globalY, fifth - 18, globalHeight);
        }

        x += fifth + 12;
    }

    const int bottomY = globalY + globalHeight + 12;
    advancedHeader->setBounds (margin, bottomY, 110, bottomHeight);
    hintLine.setBounds (margin + 120, bottomY, designWidth - 2 * margin - 120, bottomHeight);
}

void HarmonyEditor::Panel::showHint (const juce::String& text)
{
    if (hintLine.getText() != text)
        hintLine.setText (text, juce::dontSendNotification);
}

void HarmonyEditor::Panel::hintFromComponent (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (c == &stage)
            return;   // the stage has its own hints

        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            showHint (hinted->hint);
            return;
        }
    }

    showHint (restingHint);
}

int HarmonyEditor::Panel::voiceUnder (juce::Component* c) const
{
    for (; c != nullptr; c = c->getParentComponent())
    {
        for (int v = 0; v < PluginProcessor::maxVoices; ++v)
            if (c == strips[v].get() || c == throat[v].get())
                return v;

        if (c == &leadPan || c == &leadLevel || c == &leadSwitch)
            return StageView::leadIndex;
    }

    return -1;
}

void HarmonyEditor::Panel::mouseEnter (const juce::MouseEvent& e)
{
    hintFromComponent (e.eventComponent);
    stage.setHighlight (voiceUnder (e.eventComponent));
}

void HarmonyEditor::Panel::mouseMove (const juce::MouseEvent& e)
{
    hintFromComponent (e.eventComponent);
}

void HarmonyEditor::Panel::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    showHint (restingHint);
    stage.setHighlight (-1);
}

// ---- the editor ------------------------------------------------------------------------------------------------

HarmonyEditor::HarmonyEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p),
      panel ((setLookAndFeel (&lookAndFeel), *this), p)
{
    setOpaque (true);
    addAndMakeVisible (panel);

    using Proc = PluginProcessor;
    const double ratio = (double) designWidth / (double) designHeight;
    setResizable (true, true);
    setResizeLimits (Proc::minWindowWidth, juce::roundToInt (Proc::minWindowWidth / ratio), Proc::maxWindowWidth, juce::roundToInt (Proc::maxWindowWidth / ratio));
    getConstrainer()->setFixedAspectRatio (ratio);

    panel.advancedAmount = p.advancedOpen.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
    panel.advancedHeader->setAmount (panel.advancedAmount);
    panel.setBounds (0, 0, designWidth, designHeight);
    panel.resized();

    const int savedWidth = juce::jlimit (Proc::minWindowWidth, Proc::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    constructed = true;

    tick (1.0 / 60.0);

    // At most one frame per screen refresh, and never over the cap on faster screens. When nothing moves, nothing repaints.
    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
    {
        const double elapsed = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / 60.0;
        lastFrameTime = timestampSeconds;
        sinceDrawn += elapsed;

        // Spend whole refreshes against a running budget, so that the average lands on the cap at any screen rate.
        frameBudget = juce::jmin (frameBudget + elapsed, 0.2);

        const bool busy = panel.stage.isMoving() || panel.advancedAmount != (isAdvancedOpen() ? 1.0f : 0.0f) || isMouseOverOrDragging (true);
        const double frameTime = 1.0 / (busy ? maxFramesPerSecond : restingFramesPerSecond);

        if (frameBudget + 0.001 < frameTime)
            return;

        frameBudget -= frameTime;
        const double step = sinceDrawn;
        sinceDrawn = 0.0;
        tick (step);
    });
}

HarmonyEditor::~HarmonyEditor()
{
    vblank.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void HarmonyEditor::resized()
{
    panel.setBounds (0, 0, designWidth, designHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

bool HarmonyEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void HarmonyEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processorRef.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);

    if (! animate)
    {
        panel.advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
        panel.resized();
        panel.advancedHeader->setAmount (panel.advancedAmount);
        panel.repaint();
    }
}

void HarmonyEditor::tick (double dtSeconds)
{
    panel.stage.advance (dtSeconds);
    panel.presets->refresh();

    const int mode = panel.modePicker.getSelectedValue();

    if (mode != panel.modeShown)
    {
        panel.modeShown = mode;
        panel.placeForMode (mode);

        for (auto& strip : panel.strips)
            strip->interval.setMode (mode);

        panel.repaint();
    }

    const int count = processorRef.voiceCount();

    if (juce::jmax (4, count) != panel.cardsShown)
    {
        panel.resized();
        panel.repaint();
    }

    for (int v = 0; v < panel.cardsShown; ++v)
        panel.strips[v]->setSinging (panel.stage.noteFor (v), panel.stage.isLive(), v < count, panel.cardsShown > 4);

    // The lead's dim follows its dot.
    panel.leadLevel.setAlpha (panel.stage.isHeard (StageView::leadIndex) ? 1.0f : 0.5f);

    // With Follow Key on and a key to follow, the fields say what is really in use.
    const bool following = processorRef.apvts.getRawParameterValue ("followKey")->load (std::memory_order_relaxed) >= 0.5f && processorRef.getPitchHistoryCount() > 0;
    const auto readings = processorRef.getReadings();
    panel.keyField.setOverride (following ? juce::String (noteNames[juce::jlimit (0, 11, readings.activeKey)]) : juce::String());
    panel.scaleField.setOverride (following ? juce::String (scaleNames[juce::jlimit (0, numScales - 1, readings.activeScale)]) : juce::String());

    // Advanced glides open and shut, in about a fifth of a second.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;

    if (panel.advancedAmount != target)
    {
        const float next = panel.advancedAmount + (target - panel.advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        panel.advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel.resized();
        panel.advancedHeader->setAmount (panel.advancedAmount);
        panel.repaint();
    }
}

void HarmonyEditor::previewHover (const juce::String& paramId)
{
    if (auto* c = panel.findChildWithID (paramId))
    {
        if (auto* slider = dynamic_cast<BarSlider*> (c))
            slider->setForcedHover (true);
        else if (auto* toggle = dynamic_cast<PillToggle*> (c))
            toggle->setForcedHover (true);
        else if (auto* choice = dynamic_cast<ChoiceField*> (c))
            choice->setForcedHover (true);

        if (auto* hinted = dynamic_cast<Hinted*> (c))
            panel.showHint (hinted->hint);
    }

    // Voice controls live inside a strip.
    for (int v = 0; v < PluginProcessor::maxVoices; ++v)
    {
        if (auto* c = panel.strips[v]->findChildWithID (paramId))
        {
            if (! panel.strips[v]->isVisible())
                continue;

            if (auto* slider = dynamic_cast<BarSlider*> (c))
                slider->setForcedHover (true);
            else if (auto* field = dynamic_cast<IntervalField*> (c))
                field->setForcedHover (true);

            if (auto* hinted = dynamic_cast<Hinted*> (c))
                panel.showHint (hinted->hint);

            panel.stage.setHighlight (v);
        }
    }
}
