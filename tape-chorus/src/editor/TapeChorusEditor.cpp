#include "TapeChorusEditor.h"

using namespace ui;

namespace
{
    constexpr double maxFramesPerSecond = 30.0;
    constexpr int margin = 32, columns = 9;
    constexpr int heroTop = 60, footerTop = 598, footerHeight = 30;
    constexpr int mainHeight = 92, closedMainTop = 490, openMainTop = 316;
    constexpr int columnGap = 16;

    const juce::String restingHint = "Drag anything to change it, double-click to reset, click a number to type.";

    ParamStrip::Style big (ParamStrip::Unit unit, int decimals = 1)
    {
        ParamStrip::Style s;
        s.size = ParamStrip::Size::large;
        s.unit = unit;
        s.decimals = decimals;
        return s;
    }

    ParamStrip::Style small (ParamStrip::Unit unit, int decimals = 1)
    {
        ParamStrip::Style s;
        s.size = ParamStrip::Size::small;
        s.unit = unit;
        s.decimals = decimals;
        return s;
    }

    template <typename Fn>
    ParamStrip::Style with (ParamStrip::Style s, Fn&& change)
    {
        change (s);
        return s;
    }
}

class TapeChorusEditor::PresetBox final : public juce::Component,
                                          public Hinted
{
public:
    explicit PresetBox (PluginProcessor& p) : processor (p)
    {
        hint = "Starting points. Click the name for the full list, or step through with the arrows.";
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
        g.setColour (tone::mint.withAlpha (zone != none ? 0.13f : 0.08f));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);

        g.setColour (tone::mint);
        g.setFont (font (Weight::medium, 13.5f));
        g.drawText (processor.getProgramName (processor.getCurrentProgram()), b.reduced (arrowWidth, 0.0f), juce::Justification::centred, true);

        const auto chevron = [&] (float cx, float direction, bool lit)
        {
            juce::Path p;
            p.startNewSubPath (cx - 2.5f * direction, b.getCentreY() - 5.0f);
            p.lineTo (cx + 2.5f * direction, b.getCentreY());
            p.lineTo (cx - 2.5f * direction, b.getCentreY() + 5.0f);
            g.setColour (lit ? tone::mint : tone::muted);
            g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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

    void choose (int index)
    {
        processor.setCurrentProgram (index);
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true));
        refresh();
    }

    void openMenu()
    {
        juce::PopupMenu menu;
        for (int i = 0; i < processor.getNumPrograms(); ++i)
            menu.addItem (i + 1, processor.getProgramName (i), true, i == processor.getCurrentProgram());

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()),
                            [safe = juce::Component::SafePointer<PresetBox> (this)] (int chosen)
                            {
                                if (safe != nullptr && chosen > 0)
                                    safe->choose (chosen - 1);
                            });
    }

    PluginProcessor& processor;
    Zone zone = none;
    int shown = -1;
};

class TapeChorusEditor::AdvancedToggle final : public juce::Component,
                                               public Hinted
{
public:
    explicit AdvancedToggle (TapeChorusEditor& e) : editor (e)
    {
        hint = "Tape wobble, wear and filters. Everything stays set while this is closed.";
        setComponentID ("advanced");
        setTitle ("Advanced");
        setRepaintsOnMouseActivity (true);
    }

    void setState (float open, int changed)
    {
        if (std::abs (open - amount) > 0.001f || changed != numChanged)
        {
            amount = open;
            numChanged = changed;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const bool lit = isMouseOver();
        const float cy = (float) getHeight() * 0.5f;

        juce::Path chevron;
        chevron.startNewSubPath (-2.5f, -5.0f);
        chevron.lineTo (2.5f, 0.0f);
        chevron.lineTo (-2.5f, 5.0f);
        g.setColour (lit ? tone::mint : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::rotation (-amount * juce::MathConstants<float>::halfPi).translated (5.0f, cy));

        g.setColour (lit ? tone::mint : tone::muted);
        g.setFont (font (Weight::medium, 13.5f));
        g.drawText ("Advanced", juce::Rectangle<float> (18.0f, 0.0f, 70.0f, (float) getHeight()), juce::Justification::centredLeft, false);

        if (numChanged > 0 && amount < 0.5f)
        {
            g.setColour (tone::faint);
            g.setFont (font (Weight::medium, 12.0f));
            g.drawText (juce::String (numChanged) + " changed", juce::Rectangle<float> (92.0f, 0.0f, 80.0f, (float) getHeight()), juce::Justification::centredLeft, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (contains (e.getPosition()))
            editor.setAdvancedOpen (! editor.isAdvancedOpen(), true);
    }

private:
    TapeChorusEditor& editor;
    float amount = 0.0f;
    int numChanged = 0;
};

TapeChorusEditor::Panel::Panel (TapeChorusEditor& e, PluginProcessor& p)
    : owner (e),
      processor (p),
      view (p),
      bypass (p.apvts, "bypass", "Bypass", "Turns the chorus off, with a short fade.", ParamSwitch::Kind::pill),
      inputGain (p.apvts, "inputGain", "Input", "Level going in. Pushes the tape harder and comes out louder.", with (big (ParamStrip::Unit::db), [] (auto& s) { s.fromCentre = true; })),
      voices (p.apvts, "voices", "Voices", "How many separate copies move at once.", with (big (ParamStrip::Unit::count), [] (auto& s) { s.segments = 8; })),
      rate (p.apvts, "rate", "Rate", "How fast the voices sweep.", big (ParamStrip::Unit::hz)),
      depth (p.apvts, "depth", "Depth", "How far each voice swings. More swing, more pitch movement.", big (ParamStrip::Unit::ms, 2)),
      delay (p.apvts, "delay", "Delay", "How far behind the dry signal the voices sit.", big (ParamStrip::Unit::ms)),
      feedback (p.apvts, "feedback", "Feedback", "Sends the voices back onto the tape, so they repeat and swirl.", big (ParamStrip::Unit::percent)),
      spread (p.apvts, "spread", "Spread", "Moves left and right apart. At 0 both sides are the same.", big (ParamStrip::Unit::percent)),
      mix (p.apvts, "mix", "Mix", "How much chorus against the dry signal.", big (ParamStrip::Unit::percent)),
      outputGain (p.apvts, "outputGain", "Output", "Level coming out, after everything.", with (big (ParamStrip::Unit::db), [] (auto& s) { s.fromCentre = true; s.alignRight = true; })),
      inputLevel ("inputLevel", "Input level. The left reel winds out with it.", false),
      pitchSwing ("pitchSwing", "How far Rate and Depth bend the pitch, in cents. 100 cents is a semitone.", false),
      outputLevel ("outputLevel", "Output level. The right reel winds out with it.", true),
      wow (p.apvts, "wow", "Wow", "Slow, uneven tape drift.", small (ParamStrip::Unit::percent)),
      wowRate (p.apvts, "wowRate", "Wow speed", "How fast Wow drifts.", small (ParamStrip::Unit::hz)),
      flutter (p.apvts, "flutter", "Flutter", "Fast, uneven tape shake.", small (ParamStrip::Unit::percent)),
      flutterRate (p.apvts, "flutterRate", "Flutter speed", "How fast Flutter shakes.", small (ParamStrip::Unit::hz)),
      random (p.apvts, "random", "Random", "Wander with no pattern, different for every voice. Takes away the regular chorus sound.", small (ParamStrip::Unit::percent)),
      randomRate (p.apvts, "randomRate", "Random speed", "How fast Random wanders.", small (ParamStrip::Unit::hz)),
      saturation (p.apvts, "saturation", "Saturation", "Soft tape clipping on the voices. The dry signal stays clean.", small (ParamStrip::Unit::percent)),
      degrade (p.apvts, "degrade", "Degrade", "Random level dips, like worn tape. Deep ones show as gaps in the lines.", small (ParamStrip::Unit::percent)),
      degradeRate (p.apvts, "degradeRate", "Degrade speed", "How fast the dips come and go.", small (ParamStrip::Unit::hz)),
      noise (p.apvts, "noise", "Noise", "Tape hiss on the voices.", small (ParamStrip::Unit::percent)),
      tone (p.apvts, "tone", "Tone", "Top end of the voices. 0 is flat.", with (small (ParamStrip::Unit::db), [] (auto& s) { s.fromCentre = true; })),
      lowCut (p.apvts, "lowCut", "Low Cut", "Keeps low end out of the voices. The dry signal keeps it.", small (ParamStrip::Unit::lowCut)),
      highCut (p.apvts, "highCut", "High Cut", "Darkens the voices, and every repeat a little more.", small (ParamStrip::Unit::highCut)),
      unsync (p.apvts, "unsync", "Unsync", "On: left and right wander on their own, which widens the sound.", ParamSwitch::Kind::column),
      noiseGate (p.apvts, "noiseGate", "Noise Gate", "On: the hiss fades out when the input goes quiet.", ParamSwitch::Kind::column)
{
    setOpaque (true);
    presets = std::make_unique<PresetBox> (p);
    advancedToggle = std::make_unique<AdvancedToggle> (e);

    view.onHintChanged = [this] (const juce::String& text) { showHint (text); };
    addAndMakeVisible (view);
    addAndMakeVisible (*presets);
    addAndMakeVisible (*advancedToggle);
    addAndMakeVisible (bypass);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &inputGain, &voices, &rate, &depth, &delay, &feedback, &spread, &mix, &outputGain, &inputLevel, &pitchSwing, &outputLevel })
        addAndMakeVisible (c);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &wow, &wowRate, &flutter, &flutterRate, &random, &randomRate, &unsync, &saturation, &degrade, &degradeRate, &noise, &noiseGate, &tone, &lowCut, &highCut })
        addChildComponent (c);

    hintLine.setFont (font (Weight::medium, 12.5f));
    hintLine.setColour (juce::Label::textColourId, tone::muted);
    hintLine.setBorderSize ({});
    hintLine.setInterceptsMouseClicks (false, false);
    hintLine.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (hintLine);
    showHint ({});

    addMouseListener (this, true);   // hovering anything updates the hint line
}

TapeChorusEditor::Panel::~Panel()
{
    removeMouseListener (this);
}

void TapeChorusEditor::Panel::showHint (const juce::String& text)
{
    hintLine.setText (text.isEmpty() ? restingHint : text, juce::dontSendNotification);
}

void TapeChorusEditor::Panel::hintFromComponent (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            showHint (hinted->hint);
            return;
        }
    }

    showHint ({});
}

void TapeChorusEditor::Panel::mouseEnter (const juce::MouseEvent& e)  { hintFromComponent (e.eventComponent); }
void TapeChorusEditor::Panel::mouseExit (const juce::MouseEvent&)     { showHint ({}); }

int TapeChorusEditor::Panel::countAdvancedChanges() const
{
    int changed = 0;

    for (const char* id : { "wow", "wowRate", "flutter", "flutterRate", "random", "randomRate", "unsync", "saturation", "degrade", "degradeRate", "noise", "noiseGate", "tone", "lowCut", "highCut" })
    {
        const auto* prm = processor.apvts.getParameter (id);
        changed += std::abs (prm->getValue() - prm->getDefaultValue()) > 1.0e-4f ? 1 : 0;
    }

    return changed;
}

void TapeChorusEditor::Panel::layoutColumns()
{
    const float a = advancedAmount;
    const int mainTop = juce::roundToInt ((float) closedMainTop + (float) (openMainTop - closedMainTop) * a);

    view.setBounds (0, heroTop, designWidth, mainTop - 10 - heroTop);

    const float pitch = (float) (designWidth - margin * 2 + columnGap) / (float) columns;
    const auto columnX = [pitch] (int i) { return margin + juce::roundToInt (pitch * (float) i); };
    const int columnW = juce::roundToInt (pitch) - columnGap;

    ParamStrip* mains[] = { &inputGain, &voices, &rate, &depth, &delay, &feedback, &spread, &mix, &outputGain };
    for (int i = 0; i < columns; ++i)
        mains[i]->setBounds (columnX (i), mainTop, i == columns - 1 ? designWidth - margin - columnX (i) : columnW, 72);

    inputLevel.setBounds (columnX (0), mainTop + 73, columnW, 17);
    pitchSwing.setBounds (columnX (3), mainTop + 73, columnW, 17);
    outputLevel.setBounds (columnX (8), mainTop + 73, designWidth - margin - columnX (8), 17);

    // The drawer slides up with the row above it, on the same nine columns.
    const bool show = a > 0.01f;
    headerY = mainTop + mainHeight + 16;
    const int rowOne = headerY + 26, rowTwo = rowOne + 66;

    const auto place = [&] (juce::Component& c, int column, int y)
    {
        c.setVisible (show);
        c.setAlpha (a);
        c.setBounds (columnX (column), y, columnW, 54);
    };

    place (wow, 0, rowOne);        place (wowRate, 0, rowTwo);
    place (flutter, 1, rowOne);    place (flutterRate, 1, rowTwo);
    place (random, 2, rowOne);     place (randomRate, 2, rowTwo);
    place (unsync, 3, rowOne);
    place (saturation, 4, rowOne);
    place (degrade, 5, rowOne);    place (degradeRate, 5, rowTwo);
    place (noise, 6, rowOne);      place (noiseGate, 6, rowTwo);
    place (tone, 7, rowOne);
    place (lowCut, 8, rowOne);     place (highCut, 8, rowTwo);
}

void TapeChorusEditor::Panel::resized()
{
    presets->setBounds (184, 19, 230, 30);
    bypass.setBounds (designWidth - margin - 84, 19, 84, 30);
    advancedToggle->setBounds (margin, footerTop, 180, footerHeight);
    hintLine.setBounds (margin + 190, footerTop, designWidth - margin * 2 - 190, footerHeight);
    layoutColumns();
}

void TapeChorusEditor::Panel::paint (juce::Graphics& g)
{
    // Flat, and only drawn when the layout changes. Nothing here runs per frame.
    g.fillAll (tone::base);

    g.setColour (tone::mint);
    g.setFont (font (Weight::medium, 23.0f));
    g.drawText ("Tape Chorus", juce::Rectangle<int> (margin, 14, 140, 40), juce::Justification::centredLeft, false);

    if (advancedAmount > 0.01f)
    {
        const float pitch = (float) (designWidth - margin * 2 + columnGap) / (float) columns;
        const auto columnX = [pitch] (int i) { return margin + juce::roundToInt (pitch * (float) i); };
        g.setColour (tone::faint.withAlpha (advancedAmount));
        g.setFont (font (Weight::medium, 12.0f));
        g.drawText ("Tape wobble", columnX (0), headerY, 200, 18, juce::Justification::centredLeft, false);
        g.drawText ("Tape wear", columnX (4), headerY, 200, 18, juce::Justification::centredLeft, false);
        g.drawText ("Filters", columnX (7), headerY, 200, 18, juce::Justification::centredLeft, false);
    }
}

void TapeChorusEditor::Panel::refreshReadings()
{
    const auto levelText = [] (float db)
    {
        if (db <= -60.0f)
            return juce::String ("Level off");

        const auto rounded = juce::roundToInt (db);
        return "Level " + (rounded < 0 ? minusSign() : juce::String()) + juce::String (std::abs (rounded)) + " dB";
    };

    const float in = processor.getInputLevel()->get(), out = processor.getOutputLevel()->get();
    inputLevel.setText (levelText (in), in > 0.0f);
    outputLevel.setText (levelText (out), out > 0.0f);

    // With no audio running the reading has nothing to go on, so work out what Rate and Depth will do. The same sum as the engine's.
    float cents = processor.getPitchSwing()->get();
    if (! view.hasAudioRun())
    {
        const auto slope = juce::MathConstants<float>::twoPi * processor.apvts.getRawParameterValue ("rate")->load() * processor.apvts.getRawParameterValue ("depth")->load() * 0.001f;
        cents = 1200.0f * std::log2 (1.0f + slope);
    }
    pitchSwing.setText (juce::String::fromUTF8 ("\xc2\xb1") + juce::String (cents, cents < 99.5f ? 1 : 0) + " cents", false);
}

TapeChorusEditor::TapeChorusEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p),
      panel ((setLookAndFeel (&lookAndFeel), *this), p)
{
    addAndMakeVisible (panel);
    setOpaque (true);   // saves clearing the window under every frame

    using Proc = PluginProcessor;
    const double ratio = (double) designWidth / (double) designHeight;
    setResizable (true, true);
    setResizeLimits (Proc::minWindowWidth, juce::roundToInt (Proc::minWindowWidth / ratio), Proc::maxWindowWidth, juce::roundToInt (Proc::maxWindowWidth / ratio));
    getConstrainer()->setFixedAspectRatio (ratio);

    panel.advancedAmount = p.advancedOpen.load (std::memory_order_relaxed) ? 1.0f : 0.0f;

    const int savedWidth = juce::jlimit (Proc::minWindowWidth, Proc::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    constructed = true;

    tick (1.0 / 30.0);
    attachScreen();
}

TapeChorusEditor::~TapeChorusEditor()
{
    stopTimer();
    vblank.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void TapeChorusEditor::attachScreen()
{
    stopTimer();
    stillFrames = 0;
    lastFrameTime = 0.0;
    frameBudget = 0.0;
    sinceDrawn = 0.0;

    // At most one frame per screen refresh, and never over the cap on faster screens.
    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
    {
        const double elapsed = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / 60.0;
        lastFrameTime = timestampSeconds;
        sinceDrawn += elapsed;

        // Spend whole refreshes against a running budget, so that the average lands on the cap at any screen rate.
        frameBudget = juce::jmin (frameBudget + elapsed, 0.05);

        if (frameBudget + 0.001 < 1.0 / maxFramesPerSecond)
            return;

        frameBudget -= 1.0 / maxFramesPerSecond;
        const double step = sinceDrawn;
        sinceDrawn = 0.0;

        stillFrames = tick (step) ? 0 : stillFrames + 1;

        // Nothing has moved for a second. Let go of the screen's refresh and just look in now and then.
        if (stillFrames > 30)
        {
            vblank.reset();
            startTimerHz (8);
        }
    });
}

void TapeChorusEditor::timerCallback()
{
    if (tick (1.0 / 8.0))
        attachScreen();
}

void TapeChorusEditor::paint (juce::Graphics& g)
{
    // The panel covers everything but the sliver rounding can leave at some window sizes. JUCE does not skip this paint for a
    // child with a transform, so filling the whole window here would run on every frame.
    g.setColour (tone::base);
    g.fillRect (getWidth() - 2, 0, 2, getHeight());
    g.fillRect (0, getHeight() - 2, getWidth(), 2);
}

void TapeChorusEditor::resized()
{
    panel.setBounds (0, 0, designWidth, designHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

bool TapeChorusEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void TapeChorusEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processorRef.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);

    if (! animate)
    {
        panel.advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
        panel.layoutColumns();
        panel.advancedToggle->setState (panel.advancedAmount, panel.countAdvancedChanges());
        panel.repaint();
    }
    else if (vblank == nullptr)
    {
        attachScreen();
    }
}

bool TapeChorusEditor::tick (double dtSeconds)
{
    bool moved = panel.view.tick (dtSeconds);

    const float target = isAdvancedOpen() ? 1.0f : 0.0f;
    if (panel.advancedAmount != target)
    {
        const float next = panel.advancedAmount + (target - panel.advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        panel.advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel.layoutColumns();
        panel.repaint();
        panel.advancedToggle->setState (panel.advancedAmount, panel.countAdvancedChanges());
        moved = true;
    }

    // Things that hardly ever change, and numbers people cannot read any faster, get looked at about five times a second.
    if (--slowCountdown <= 0)
    {
        slowCountdown = 6;
        panel.refreshReadings();
        panel.presets->refresh();
        panel.advancedToggle->setState (panel.advancedAmount, panel.countAdvancedChanges());
    }

    return moved;
}

void TapeChorusEditor::previewHover (const juce::String& paramId)
{
    if (auto* c = panel.findChildWithID (paramId))
    {
        if (auto* strip = dynamic_cast<ParamStrip*> (c))
            strip->setForcedHover (true);

        if (auto* hinted = dynamic_cast<Hinted*> (c))
            panel.showHint (hinted->hint);
    }
}
