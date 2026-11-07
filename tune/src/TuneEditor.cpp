#include "TuneEditor.h"

using namespace ui;

namespace
{
    constexpr int margin = 20;
    constexpr int rowGap = 14;
    constexpr int fieldsHeight = 56;
    constexpr int notesHeight = 62;
    constexpr int slidersHeight = 44;
    constexpr int footerHeight = 46;
    constexpr double fastRate = 30.0, slowRate = 8.0;

    const juce::String restingHint = "Double-click anything to reset it. Click a value to type one.";

    juce::String withUnit (float v, int places, const char* unit)
    {
        return juce::String (v, places) + unit;
    }

    juce::String signedWithUnit (float v, const char* unit)
    {
        const int n = juce::roundToInt (v);
        return (n > 0 ? juce::String ("+") : (n < 0 ? juce::String::fromUTF8 ("\xe2\x88\x92") : juce::String())) + juce::String (std::abs (n)) + unit;
    }
}

// ---- one note on the strip --------------------------------------------------------------------------------------

class TuneEditor::NoteCell final : public juce::Component,
                                   public Hinted
{
public:
    NoteCell (TuneEditor& o, int pitchClass)
        : owner (o), index (pitchClass), accidental (juce::String (smoothtune::noteNames[pitchClass]).length() > 1)
    {
        setComponentID ("note" + juce::String (pitchClass));
        setTitle (juce::String ("Custom ") + smoothtune::noteNames[pitchClass]);
        hint = "Click to tune to this note or remove it. Right-click to bypass it.";
    }

    void setLook (int newMode, bool newTarget)
    {
        if (newMode != mode || newTarget != target)
        {
            mode = newMode;
            target = newTarget;
            repaint();
        }
    }

    int getShownMode() const noexcept  { return mode; }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const bool lit = isMouseOver (true);
        juce::Colour ink = tone::text;

        if (mode == 0)            // tuned to
        {
            g.setColour (target ? tone::lime : tone::text.withAlpha ((accidental ? 0.09f : 0.15f) + (lit ? 0.06f : 0.0f)));
            g.fillRoundedRectangle (b, 7.0f);
            ink = target ? tone::well : tone::text;
        }
        else if (mode == 2)       // a note, but left as sung
        {
            g.setColour (target ? tone::lime : tone::text.withAlpha (lit ? 0.75f : 0.5f));
            g.drawRoundedRectangle (b.reduced (0.75f), 6.25f, 1.5f);
            ink = target ? tone::lime : tone::text;
        }
        else                      // taken out
        {
            if (lit)
            {
                g.setColour (tone::text.withAlpha (0.06f));
                g.fillRoundedRectangle (b, 7.0f);
            }
            ink = lit ? tone::muted : tone::faint;
        }

        g.setColour (ink);
        g.setFont (font (mode == 1 ? Weight::regular : Weight::medium, 14.0f));
        g.drawText (smoothtune::noteNames[index], b, juce::Justification::centred, false);
    }

    void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            openMenu();
        else if (e.getNumberOfClicks() == 1)
            owner.clickNote (index);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            owner.setNoteMode (index, 0);
    }

private:
    void openMenu()
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        const char* names[] = { "Tune", "Remove", "Bypass" };

        for (int i = 0; i < 3; ++i)
            menu.addItem (i + 1, names[i], true, i == owner.noteMode (index) && owner.scale.getSelected() == smoothtune::scaleCustom);

        juce::Component::SafePointer<NoteCell> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withStandardItemHeight (26),
                            [safe] (int result)
                            {
                                if (safe != nullptr && result > 0)
                                    safe->owner.setNoteMode (safe->index, result - 1);
                            });
    }

    TuneEditor& owner;
    const int index;
    const bool accidental;
    int mode = 0;
    bool target = false;
};

// ---- the Advanced row -------------------------------------------------------------------------------------------

class TuneEditor::AdvancedHeader final : public juce::Component,
                                         public Hinted
{
public:
    explicit AdvancedHeader (TuneEditor& o) : owner (o)
    {
        setComponentID ("advanced");
        setTitle ("Advanced");
        hint = "Transpose, Reference A4, Tracking, Throat Length and Formant Preserve.";
    }

    void setSummary (const juce::String& text)
    {
        if (text != summary)
        {
            summary = text;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const bool lit = isMouseOver (true);
        const bool open = owner.isAdvancedOpen();
        auto b = getLocalBounds().toFloat();
        auto mark = b.removeFromLeft (16.0f);

        juce::Path chevron;
        const float cx = mark.getCentreX() - 2.0f, cy = mark.getCentreY();
        if (open)
        {
            chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
            chevron.lineTo (cx, cy + 2.0f);
            chevron.lineTo (cx + 4.0f, cy - 2.0f);
        }
        else
        {
            chevron.startNewSubPath (cx - 2.0f, cy - 4.0f);
            chevron.lineTo (cx + 2.0f, cy);
            chevron.lineTo (cx - 2.0f, cy + 4.0f);
        }
        g.setColour (lit ? tone::text : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setFont (font (Weight::medium, 13.0f));
        g.drawText ("Advanced", b.removeFromLeft (64.0f), juce::Justification::centredLeft, false);

        if (summary.isNotEmpty() && ! open)
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (summary, b.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }
    void mouseDown (const juce::MouseEvent&) override   { owner.setAdvancedOpen (! owner.isAdvancedOpen()); }

private:
    TuneEditor& owner;
    juce::String summary;
};

// ---- the editor -------------------------------------------------------------------------------------------------

TuneEditor::TuneEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p),
      display (p),
      presets (p),
      bypass (p.apvts, "bypass", "Bypass", "Turns the tuning off. The delay stays the same, so nothing shifts in time."),
      followKey (p.apvts, "followKey", "Follow Key", "Takes the key and scale from the Key plug-in when it has a good reading."),
      key (p.apvts, "key", "Key", "The root of the scale."),
      scale (p.apvts, "scale", "Scale", "Which notes the voice is pulled to. Custom uses the notes you pick below."),
      inputType (p.apvts, "inputType", "Input Type", "The range of the voice. Lower ranges add a little more delay."),
      retune (p.apvts, "retune", "Retune Speed", "How fast the voice is pulled to the note. Instant is the hard tuned sound.", tone::lime,
              [] (float v) { return v < 0.05f ? juce::String ("Instant") : (v < 10.0f ? withUnit (v, 1, " ms") : withUnit (v, 0, " ms")); }),
      humanize (p.apvts, "humanize", "Humanize", "Goes easier on held notes, while the start of each note stays quick.", tone::text,
                [] (float v) { return withUnit (v, 0, "%"); }),
      flex (p.apvts, "flex", "Flex Amount", "How far a note can drift before it gets corrected. The band shows it.", tone::text,
            [] (float v) { return withUnit (v, 0, " cents"); }),
      vibrato (p.apvts, "naturalVibrato", "Natural Vibrato", "Less or more of the sung vibrato. -100 removes it, +100 doubles it.", tone::text,
               [] (float v) { return signedWithUnit (v, "%"); }),
      transpose (p.apvts, "transpose", "Transpose", "Moves the voice in semitones, after tuning. The formants stay.", tone::text,
                 [] (float v) { return signedWithUnit (v, " st"); }),
      reference (p.apvts, "reference", "Reference A4", "The pitch of A above middle C, for a track not tuned to 440.", tone::text,
                 [] (float v) { return withUnit (v, 1, " Hz"); }),
      tracking (p.apvts, "tracking", "Tracking", "Which sounds count as pitched. Low leaves rough, breathy sound alone.", tone::text,
                [] (float v) { return withUnit (v, 0, "%"); }),
      throat (p.apvts, "throat", "Throat Length", "Above 100 sounds like a longer throat, lowering the formants.", tone::text,
              [] (float v) { return withUnit (v, 0, "%"); }),
      formant (p.apvts, "formantPreserve", "Formant Preserve", "On keeps the character of the voice. Off lets it follow the pitch.")
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (false);
    setOpaque (true);

    presets.nameNow = [&p]
    {
        if (! p.isCurrentProgramEdited())
            return p.getProgramName (p.getCurrentProgram());

        // Fresh out of the box the settings are their own, not a preset that has been changed.
        bool atDefaults = true;
        for (const char* id : { "retune", "humanize", "flex", "naturalVibrato" })
        {
            auto* prm = p.apvts.getParameter (id);
            atDefaults = atDefaults && std::abs (prm->getValue() - prm->getDefaultValue()) < 1.0e-4f;
        }
        return atDefaults ? juce::String ("Default") : p.getProgramName (p.getCurrentProgram()) + ", edited";
    };

    for (auto* s : { &transpose, &reference, &tracking, &throat })
        s->setCompact (true);

    addAndMakeVisible (display);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &presets, &bypass, &followKey, &key, &scale, &inputType, &retune, &humanize, &flex, &vibrato })
        addAndMakeVisible (c);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &transpose, &reference, &tracking, &throat, &formant })
        addChildComponent (c);

    for (int i = 0; i < 12; ++i)
    {
        notes.push_back (std::make_unique<NoteCell> (*this, i));
        addAndMakeVisible (*notes.back());
        noteAttachments.push_back (std::make_unique<juce::ParameterAttachment> (*p.apvts.getParameter ("note" + juce::String (i)),
            [this, i] (float v)
            {
                modes[(size_t) i] = juce::jlimit (0, 2, juce::roundToInt (v));
                updateNotes();
            }));
        noteAttachments.back()->sendInitialUpdate();
    }

    advancedHeader = std::make_unique<AdvancedHeader> (*this);
    addAndMakeVisible (*advancedHeader);

    hintText = restingHint;
    addMouseListener (this, true);

    setResizable (true, true);
    const bool open = isAdvancedOpen();
    using P = PluginProcessor;
    const int extra = open ? drawerHeight : 0;
    setResizeLimits (P::minWindowWidth, P::minWindowHeight + extra, P::maxWindowWidth, P::maxWindowHeight);
    setSize (juce::jlimit (P::minWindowWidth, P::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed)),
             juce::jlimit (P::minWindowHeight + extra, P::maxWindowHeight, p.windowHeight.load (std::memory_order_relaxed)));
    constructed = true;
    resized();

    updateFollow();
    updateNotes();
    updateAdvancedSummary();
    presets.refresh();
    tick (1.0 / fastRate);
    startTimerHz ((int) slowRate);
}

TuneEditor::~TuneEditor()
{
    stopTimer();
    removeMouseListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void TuneEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::base);

    // A note lighting up repaints this too, because the cells are not opaque. Text outside the part being painted costs for nothing.
    if (g.clipRegionIntersects (titleArea))
    {
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 22.0f));
        g.drawText ("Tune", titleArea, juce::Justification::centredLeft, false);
    }

    if (g.clipRegionIntersects (hintArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (hintText, hintArea, juce::Justification::centredRight, true);
    }

    if (! g.clipRegionIntersects (notesLabelArea))
        return;

    g.setColour (tone::muted);
    g.setFont (font (Weight::regular, 13.0f));
    g.drawText ("Notes", notesLabelArea, juce::Justification::centredLeft, false);

    // What the three looks on the notes mean, drawn the way the notes draw them.
    auto legend = notesLabelArea.toFloat();
    const auto legendFont = font (Weight::regular, 12.0f);
    const auto swatch = [&] (const char* name, int look)
    {
        auto item = legend.removeFromRight (juce::GlyphArrangement::getStringWidth (legendFont, name) + 19.0f);
        legend.removeFromRight (14.0f);
        const auto box = item.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 12.0f);
        if (look == 0)
        {
            g.setColour (tone::text.withAlpha (0.28f));
            g.fillRoundedRectangle (box, 3.0f);
        }
        else if (look == 2)
        {
            g.setColour (tone::text.withAlpha (0.6f));
            g.drawRoundedRectangle (box.reduced (0.6f), 2.5f, 1.2f);
        }
        else
        {
            g.setColour (tone::faint);
            g.setFont (font (Weight::regular, 10.0f));
            g.drawText ("C", box, juce::Justification::centred, false);
        }
        g.setColour (tone::muted);
        g.setFont (legendFont);
        g.drawText (name, item.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
    };
    swatch ("Bypass", 2);
    swatch ("Remove", 1);
    swatch ("Tune", 0);
}

void TuneEditor::resized()
{
    if (! constructed)
        return;

    processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
    processorRef.windowHeight.store (getHeight(), std::memory_order_relaxed);

    auto r = getLocalBounds().withTrimmedLeft (margin).withTrimmedRight (margin);

    // Top bar.
    auto bar = r.removeFromTop (56).withTrimmedTop (12).withTrimmedBottom (8);
    titleArea = bar.removeFromLeft (72);
    presets.setBounds (bar.removeFromLeft (248));
    bypass.setBounds (bar.removeFromRight (84));

    // Bottom up: the fixed rows. The display takes whatever is left.
    auto foot = r.removeFromBottom (footerHeight).withTrimmedBottom (10).withTrimmedTop (4);
    advancedHeader->setBounds (foot.removeFromLeft (foot.getWidth() * 2 / 5));
    hintArea = foot;

    const bool open = isAdvancedOpen();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &transpose, &reference, &tracking, &throat, &formant })
        c->setVisible (open);

    if (open)
    {
        auto drawer = r.removeFromBottom (drawerHeight - rowGap);
        r.removeFromBottom (rowGap);
        drawer = drawer.withTrimmedTop (2);
        const int gap = 20;
        const int pill = 128;
        formant.setBounds (drawer.removeFromRight (pill).withSizeKeepingCentre (pill, 34));
        drawer.removeFromRight (gap);
        const int w = (drawer.getWidth() - 3 * gap) / 4;
        for (auto* s : { &transpose, &reference, &tracking, &throat })
        {
            s->setBounds (drawer.removeFromLeft (w).withHeight (slidersHeight));
            drawer.removeFromLeft (gap);
        }
    }

    auto sliders = r.removeFromBottom (slidersHeight);
    r.removeFromBottom (rowGap + 2);
    {
        const int gap = 28;
        const int w = (sliders.getWidth() - 3 * gap) / 4;
        for (auto* s : { &retune, &humanize, &flex, &vibrato })
        {
            s->setBounds (sliders.removeFromLeft (w));
            sliders.removeFromLeft (gap);
        }
    }

    auto strip = r.removeFromBottom (notesHeight);
    r.removeFromBottom (rowGap - 2);
    notesLabelArea = strip.removeFromTop (20);
    strip.removeFromTop (2);
    {
        const int gap = 5;
        const int total = strip.getWidth();
        for (int i = 0; i < 12; ++i)
        {
            const int x0 = strip.getX() + (total + gap) * i / 12;
            const int x1 = strip.getX() + (total + gap) * (i + 1) / 12 - gap;
            notes[(size_t) i]->setBounds (x0, strip.getY(), x1 - x0, strip.getHeight());
        }
    }

    auto fields = r.removeFromBottom (fieldsHeight);
    r.removeFromBottom (rowGap);
    key.setBounds (fields.removeFromLeft (80));
    fields.removeFromLeft (10);
    scale.setBounds (fields.removeFromLeft (178));
    fields.removeFromLeft (10);
    followKey.setBounds (fields.removeFromLeft (102).withTrimmedTop (22));
    inputType.setBounds (fields.removeFromRight (juce::jmin (fields.getWidth() - 24, 400)));

    display.setBounds (r);
    repaint();
}

bool TuneEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void TuneEditor::setAdvancedOpen (bool shouldBeOpen)
{
    if (shouldBeOpen == isAdvancedOpen())
        return;

    processorRef.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);

    using P = PluginProcessor;
    const int extra = shouldBeOpen ? drawerHeight : 0;
    const int height = juce::jlimit (P::minWindowHeight + extra, P::maxWindowHeight, getHeight() + (shouldBeOpen ? drawerHeight : -drawerHeight));
    setResizeLimits (P::minWindowWidth, P::minWindowHeight + extra, P::maxWindowWidth, P::maxWindowHeight);
    setSize (getWidth(), height);
    resized();
    advancedHeader->repaint();
}

juce::String TuneEditor::readingText() const
{
    const auto r = processorRef.getReadings();
    auto text = juce::String ("Active Key ") + smoothtune::noteNames[juce::jlimit (0, 11, r.activeKey)] + " " + smoothtune::scaleNames[juce::jlimit (0, smoothtune::numScales - 1, r.activeScale)];
    if (! r.voiced)
        return text + "  |  no pitch";
    const juce::String note = r.targetNote < 0 ? juce::String ("no note") : juce::String (smoothtune::noteNames[r.targetNote % 12]) + juce::String (r.targetNote / 12 - 1);
    return text + "  |  " + juce::String (r.detectedHz, 1) + " Hz  |  Target " + note
           + "  |  Correction " + juce::String (juce::roundToInt (r.correctionCents)) + " cents";
}

// ---- notes ------------------------------------------------------------------------------------------------------

int TuneEditor::noteMode (int pitchClass) const noexcept
{
    return modes[(size_t) pitchClass];
}

void TuneEditor::setNoteMode (int pitchClass, int mode)
{
    // From any scale but Custom, the current scale's notes become the start of your own, so nothing jumps.
    if (scale.getSelected() != smoothtune::scaleCustom)
    {
        const auto r = processorRef.getReadings();
        const bool following = followKey.isOn() && processorRef.getPitchHistoryCount() > 0;
        const auto mask = smoothtune::scaleMask (following ? r.activeKey : key.getSelected(), following ? r.activeScale : scale.getSelected());

        for (int i = 0; i < 12; ++i)
            noteAttachments[(size_t) i]->setValueAsCompleteGesture ((mask >> i) & 1 ? 0.0f : 1.0f);

        scale.choose (smoothtune::scaleCustom);
    }

    noteAttachments[(size_t) pitchClass]->setValueAsCompleteGesture ((float) mode);
    updateNotes();
}

void TuneEditor::clickNote (int pitchClass)
{
    setNoteMode (pitchClass, notes[(size_t) pitchClass]->getShownMode() == 0 ? 1 : 0);
}

void TuneEditor::updateNotes()
{
    if (notes.size() < 12)
        return;

    const auto r = processorRef.getReadings();
    const int selectedScale = scale.getSelected();
    const bool following = followKey.isOn() && selectedScale != smoothtune::scaleCustom && processorRef.getPitchHistoryCount() > 0;
    const auto mask = smoothtune::scaleMask (following ? r.activeKey : key.getSelected(), following ? r.activeScale : selectedScale);

    for (int i = 0; i < 12; ++i)
    {
        const int look = selectedScale == smoothtune::scaleCustom ? modes[(size_t) i] : ((mask >> i) & 1 ? 0 : 1);
        notes[(size_t) i]->setLook (look, shownTarget == i);
    }
}

void TuneEditor::updateFollow()
{
    const auto r = processorRef.getReadings();

    if (followKey.isOn() && scale.getSelected() != smoothtune::scaleCustom && processorRef.getPitchHistoryCount() > 0)
    {
        key.showInstead (smoothtune::noteNames[juce::jlimit (0, 11, r.activeKey)]);
        scale.showInstead (smoothtune::scaleNames[juce::jlimit (0, smoothtune::numScales - 1, r.activeScale)]);
    }
    else
    {
        key.showInstead ({});
        scale.showInstead ({});
    }
}

void TuneEditor::updateAdvancedSummary()
{
    // Hidden settings still change the sound, so a closed Advanced says which ones have moved.
    juce::StringArray moved;
    const auto note = [&moved] (BarSlider& s, const char* name, double rest)
    {
        if (std::abs (s.getValue() - rest) > 1.0e-3)
            moved.add (juce::String (name) + " " + s.valueText());
    };
    note (transpose, "Transpose", 0.0);
    note (reference, "Reference", 440.0);
    note (tracking, "Tracking", 50.0);
    note (throat, "Throat", 100.0);
    if (! formant.isOn())
        moved.add ("Formant Preserve off");
    advancedHeader->setSummary (moved.joinIntoString (", "));
}

// ---- frames -----------------------------------------------------------------------------------------------------

bool TuneEditor::tick (double seconds)
{
    const bool moving = display.advance (seconds);

    const auto r = processorRef.getReadings();
    const int target = r.voiced && r.targetNote >= 0 ? r.targetNote % 12 : -1;
    if (target != shownTarget)
    {
        shownTarget = target;
        updateNotes();
    }

    updateFollow();
    updateNotes();

    if (--slowCounter <= 0)
    {
        slowCounter = fast ? 8 : 2;
        presets.refresh();
        updateAdvancedSummary();
    }

    return moving;
}

void TuneEditor::timerCallback()
{
    if (! isShowing())
        return;

    const double seconds = getTimerInterval() * 0.001;
    const bool moving = tick (seconds);

    // When nothing moves, nothing repaints, and the timer only looks in a few times a second to see whether a voice has started.
    if (moving != fast)
    {
        fast = moving;
        startTimerHz ((int) (fast ? fastRate : slowRate));
    }
}

// ---- the hint line ----------------------------------------------------------------------------------------------

void TuneEditor::setHint (const juce::String& text)
{
    if (text != hintText)
    {
        hintText = text;
        repaint (hintArea);
    }
}

void TuneEditor::showHintFor (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            setHint (hinted->hint);
            return;
        }
    }
    setHint (restingHint);
}

void TuneEditor::mouseEnter (const juce::MouseEvent& e)  { showHintFor (e.eventComponent); }
void TuneEditor::mouseMove (const juce::MouseEvent& e)   { showHintFor (e.eventComponent); }
void TuneEditor::mouseExit (const juce::MouseEvent&)     { setHint (restingHint); }

void TuneEditor::previewHint (const juce::String& paramId)
{
    if (auto* c = findChildWithID (paramId))
    {
        showHintFor (c);
        if (auto* s = dynamic_cast<BarSlider*> (c))
            s->setForcedHover (true);
    }
}
