#include "MorphEditor.h"

using namespace ui;

namespace
{
    constexpr int margin = 20;
    constexpr int columnGap = 12;
    constexpr int rowGap = 12;
    constexpr int valuesHeight = 52;
    constexpr int chipsHeight = 58;
    constexpr int slidersHeight = 44;
    constexpr int footerHeight = 34;
    constexpr double fastRate = 30.0, slowRate = 8.0;

    const juce::String restingHint = "Double-click resets. Click a value to type.";

    juce::String withUnit (float v, int places, const char* unit)
    {
        return juce::String (v, places) + unit;
    }

    juce::String noteName (int midi)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }
}

// ---- the Advanced row -------------------------------------------------------------------------------------------

class MorphEditor::AdvancedHeader final : public juce::Component,
                                          public Hinted
{
public:
    explicit AdvancedHeader (MorphEditor& o) : owner (o)
    {
        setComponentID ("advanced");
        setTitle ("Advanced");
        hint = "Range Match, Peak Guard, Centre, Mix and Output.";
    }

    void setSummary (const juce::String& text)
    {
        if (text != summary)
        {
            summary = text;
            repaint();
        }
    }

    juce::String getSummary() const  { return summary; }

    float summaryOverflow() const
    {
        if (summary.isEmpty())
            return -1.0e6f;
        const float room = (float) getWidth() - 16.0f - 64.0f - 8.0f;
        return juce::GlyphArrangement::getStringWidth (font (Weight::regular, 13.0f), summary) - room;
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
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

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
    MorphEditor& owner;
    juce::String summary;
};

// ---- the editor -------------------------------------------------------------------------------------------------

MorphEditor::MorphEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p),
      pad (p),
      mirror (p),
      presets (p),
      bypass (p.apvts, "bypass", "Bypass", "Hear your own voice. The delay stays the same."),
      pitchValue (p.apvts, "pitch", "Pitch", "How high the voice sings. The dot's left and right."),
      formantValue (p.apvts, "formant", "Formant", "How big the singer sounds. The dot's up and down."),
      chips (p.apvts, "character", "Character", "A whole voice at once. It adds to where the dot is."),
      amount (p.apvts, "characterAmount", "Amount", "How much of the Character you hear.", tone::acid,
              [] (float v) { return withUnit (v, 0, "%"); }),
      vibratoDepth (p.apvts, "vibratoDepth", "Vibrato", "How far the added vibrato swings. 0 adds none.", tone::acid,
                    [] (float v) { return withUnit (v, 0, " cents"); }),
      vibratoRate (p.apvts, "vibratoRate", "Rate", "How fast the added vibrato swings.", tone::text,
                   [] (float v) { return withUnit (v, 1, " Hz"); }),
      whisper (p.apvts, "whisper", "Whisper", "Swaps the buzz of the voice for breath.", tone::acid,
               [] (float v) { return withUnit (v, 0, "%"); }),
      robotNote (p.apvts, "robotNote", "Robot Note", "The one note Robot sits on. Pitch moves it.", tone::text,
                 [] (float v) { return noteName (juce::roundToInt (v)); }),
      rangeMatch (p.apvts, "rangeMatch", "Range Match", "Learns the voice's range and moves it to Range Centre."),
      peakGuard (p.apvts, "peakGuard", "Peak Guard", "Holds peaks at or under your own voice's highest."),
      rangeCentre (p.apvts, "rangeCentre", "Range Centre", "Where Range Match puts the voice.", tone::text,
                   [] (float v) { return noteName (juce::roundToInt (v)); }),
      mix (p.apvts, "mix", "Mix", "Blends the alter ego with your own voice.", tone::text,
           [] (float v) { return withUnit (v, 0, "%"); }),
      output (p.apvts, "output", "Output", "Level coming out of the plug-in.", tone::text,
              [] (float v) { return signedText (v, 1, " dB"); })
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (false);
    setOpaque (true);

    processorRef.getTap().windowOpened();

    presets.nameNow = [&p]
    {
        return p.isCurrentProgramEdited() ? p.getProgramName (p.getCurrentProgram()) + ", edited" : p.getProgramName (p.getCurrentProgram());
    };

    chips.onHover = [this] (int index) { pad.showCharacter (index); };
    amount.setLabelRow (20);

    addAndMakeVisible (pad);
    addAndMakeVisible (mirror);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &presets, &bypass, &pitchValue, &formantValue, &chips, &amount, &vibratoDepth, &vibratoRate, &whisper, &robotNote })
        addAndMakeVisible (c);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &rangeMatch, &peakGuard, &rangeCentre, &mix, &output })
        addChildComponent (c);

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

    presets.refresh();
    tick();
    startTimerHz ((int) slowRate);
}

MorphEditor::~MorphEditor()
{
    stopTimer();
    processorRef.getTap().windowClosed();
    removeMouseListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void MorphEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::base);

    if (g.clipRegionIntersects (titleArea))
    {
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 24.0f));
        g.drawText ("Alter Ego", titleArea, juce::Justification::centredLeft, false);
    }

    if (g.clipRegionIntersects (statusArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawFittedText (status, statusArea, juce::Justification::centredRight, 2, 1.0f);
    }

    if (g.clipRegionIntersects (hintArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (hintText, hintArea, juce::Justification::centredRight, true);
    }
}

void MorphEditor::resized()
{
    if (! constructed)
        return;

    processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
    processorRef.windowHeight.store (getHeight(), std::memory_order_relaxed);

    auto area = getLocalBounds().reduced (margin, 0);

    // The mirror runs the whole height, floor to ceiling, like one.
    const int mirrorWidth = juce::jlimit (260, 440, juce::roundToInt ((float) getWidth() * 0.286f));
    mirror.setBounds (area.removeFromRight (mirrorWidth).withTrimmedTop (14).withTrimmedBottom (14));
    area.removeFromRight (columnGap);

    auto bar = area.removeFromTop (58).withTrimmedTop (14).withTrimmedBottom (8);
    titleArea = bar.removeFromLeft (118);
    presets.setBounds (bar.removeFromLeft (juce::jmin (248, bar.getWidth() - 100)));
    bypass.setBounds (bar.removeFromRight (86));

    auto foot = area.removeFromBottom (footerHeight + 4).withTrimmedBottom (6);
    advancedHeader->setBounds (foot.removeFromLeft (foot.getWidth() * 2 / 5));
    hintArea = foot;

    const bool open = isAdvancedOpen();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &rangeMatch, &peakGuard, &rangeCentre, &mix, &output })
        c->setVisible (open);

    if (open)
    {
        auto drawer = area.removeFromBottom (drawerHeight - rowGap);
        area.removeFromBottom (rowGap);
        const int gap = 24;
        const int pill = 112, guardPill = 104;
        rangeMatch.setBounds (drawer.removeFromLeft (pill).withSizeKeepingCentre (pill, 34));
        drawer.removeFromLeft (10);
        peakGuard.setBounds (drawer.removeFromLeft (guardPill).withSizeKeepingCentre (guardPill, 34));
        drawer.removeFromLeft (gap);
        const int w = (drawer.getWidth() - 2 * gap) / 3;
        for (auto* s : { &rangeCentre, &mix, &output })
        {
            s->setBounds (drawer.removeFromLeft (w).withHeight (slidersHeight));
            drawer.removeFromLeft (gap);
        }
    }

    auto sliders = area.removeFromBottom (slidersHeight);
    area.removeFromBottom (rowGap);
    {
        const int gap = 24;
        const int w = (sliders.getWidth() - 3 * gap) / 4;
        for (auto* s : { &vibratoDepth, &vibratoRate, &whisper, &robotNote })
        {
            s->setBounds (sliders.removeFromLeft (w));
            sliders.removeFromLeft (gap);
        }
    }

    auto choice = area.removeFromBottom (chipsHeight);
    area.removeFromBottom (rowGap);
    amount.setBounds (choice.removeFromRight (juce::jmax (150, (choice.getWidth() - 3 * 24) / 4)));
    choice.removeFromRight (24);
    chips.setBounds (choice);

    auto values = area.removeFromBottom (valuesHeight);
    area.removeFromBottom (8);
    pitchValue.setBounds (values.removeFromLeft (140));
    formantValue.setBounds (values.removeFromLeft (140));
    statusArea = values.withTrimmedLeft (12);

    pad.setBounds (area);
    repaint();
}

bool MorphEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void MorphEditor::setAdvancedOpen (bool shouldBeOpen)
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

juce::String MorphEditor::presetText() const   { return presets.nameNow != nullptr ? presets.nameNow() : juce::String(); }
float MorphEditor::advancedSummaryOverflow() const { return advancedHeader->summaryOverflow(); }

juce::String MorphEditor::readingText() const
{
    const auto said = [] (double hz) { return hz > 0.0 ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String ("no pitch"); };
    return "You " + said (mirror.getYouHz()) + "  |  Alter Ego " + said (mirror.getEgoHz());
}

void MorphEditor::updateStatus()
{
    const auto value = [this] (const char* id) { return processorRef.apvts.getRawParameterValue (id)->load (std::memory_order_relaxed); };

    const int character = juce::jlimit (0, smoothmorph::numCharacters - 1, juce::roundToInt (value ("character")));
    const auto& c = smoothmorph::characters[character];
    const float share = value ("characterAmount") * 0.01f;

    juce::String text;

    if (value ("bypass") >= 0.5f)
    {
        text = "Bypassed. This is your own voice, at the same delay.";
    }
    else
    {
        const auto shortest = [] (float v) { return signedText (v, std::abs (v - std::round (v)) < 0.05f ? 0 : 1, " st"); };

        if (character == 3)
            text = "Robot holds every note on " + noteName (juce::roundToInt (value ("robotNote") + value ("pitch"))) + ".";
        else if (character == 0)
            text = std::abs (value ("pitch")) < 0.005f && std::abs (value ("formant")) < 0.005f && value ("vibratoDepth") <= 0.0f && value ("whisper") <= 0.0f
                       ? "Your voice, untouched." : "No Character added.";
        else
            text = juce::String (c.name) + " adds " + shortest ((float) c.pitchSemitones * share) + " pitch, "
                   + shortest ((float) c.formantSemitones * share) + " formant" + (character == 4 ? "\nand a growl." : ".");

        const float offset = processorRef.getEngine().rangeOffset();
        if (value ("rangeMatch") >= 0.5f && character != 3)
            text += offset != 0.0f ? "\nRange Match adds " + shortest (offset) + "." : "\nRange Match is listening.";
    }

    if (text != status)
    {
        status = text;
        repaint (statusArea);
    }

    // A control with nothing to do right now goes quiet, and still works.
    amount.setResting (character == 0);
    robotNote.setResting (character != 3);
    vibratoRate.setResting (value ("vibratoDepth") <= 0.0f);
    rangeCentre.setResting (value ("rangeMatch") < 0.5f);
}

void MorphEditor::updateAdvancedSummary()
{
    // Hidden settings still change the sound, so a closed Advanced says which ones have moved.
    juce::StringArray moved;
    if (rangeMatch.isOn())
        moved.add ("Range Match to " + rangeCentre.valueText());
    if (peakGuard.isOn())
        moved.add ("Peak Guard on");
    if (std::abs (mix.getValue() - 100.0) > 0.05)
        moved.add ("Mix " + mix.valueText());
    if (std::abs (output.getValue()) > 0.005)
        moved.add ("Output " + output.valueText());
    advancedHeader->setSummary (moved.joinIntoString (", "));
}

// ---- frames -----------------------------------------------------------------------------------------------------

bool MorphEditor::tick()
{
    bool moving = mirror.advance();

    moving = pad.advance (mirror.isVoiced(), mirror.getLiveShift(), mirror.getOutputLevel()) || moving;

    if (--slowCounter <= 0)
    {
        slowCounter = fast ? 6 : 1;
        presets.refresh();
        updateStatus();
        updateAdvancedSummary();
    }

    return moving;
}

void MorphEditor::timerCallback()
{
    if (! isShowing())
        return;

    const bool moving = tick();

    // When nothing moves, nothing repaints, and the timer only looks in a few times a second to see whether a voice has started.
    if (moving != fast)
    {
        fast = moving;
        startTimerHz ((int) (fast ? fastRate : slowRate));
    }
}

// ---- the hint line ----------------------------------------------------------------------------------------------

void MorphEditor::setHint (const juce::String& text)
{
    if (text != hintText)
    {
        hintText = text;
        repaint (hintArea);
    }
}

void MorphEditor::showHintFor (juce::Component* c)
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

void MorphEditor::mouseEnter (const juce::MouseEvent& e)  { showHintFor (e.eventComponent); }
void MorphEditor::mouseMove (const juce::MouseEvent& e)   { showHintFor (e.eventComponent); }
void MorphEditor::mouseExit (const juce::MouseEvent&)     { setHint (restingHint); }

void MorphEditor::previewHint (const juce::String& paramId)
{
    if (auto* c = findChildWithID (paramId))
    {
        showHintFor (c);
        if (auto* s = dynamic_cast<DotSlider*> (c))
            s->setForcedHover (true);
    }
}

juce::StringArray MorphEditor::allHints (MorphEditor& e)
{
    juce::StringArray all { restingHint };
    for (int i = 0; i < e.getNumChildComponents(); ++i)
        if (auto* hinted = dynamic_cast<Hinted*> (e.getChildComponent (i)))
            all.add (hinted->hint);
    return all;
}
