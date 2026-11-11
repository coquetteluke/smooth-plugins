#include "DuoEditor.h"

using namespace ui;

namespace
{
    // The grid everything sits on.
    constexpr int margin = 24;
    constexpr int headerY = 18, headerHeight = 36;
    constexpr int stageY = 68;
    constexpr int stageWidth = 766;
    constexpr int gap = 16;
    constexpr int widthRowHeight = 58;
    constexpr int bottomHeight = 30;
    constexpr int advancedHeight = 118, advancedRow = 46;
    constexpr int tightnessWidth = 84;
    constexpr int faderWidth = 58, faderGap = 6;
    constexpr int readoutHeight = 24, captionHeight = 34;

    // 30 is an even cadence on 60 and 120 Hz screens, and Luke checked it looks good.
    constexpr double maxFramesPerSecond = 30.0;
    // With nothing moving and no mouse over the window, it only has to notice a change from automation or sound starting.
    constexpr int restingMilliseconds = 125;

    const juce::String restingHint = "Drag a double sideways for Width, back or forward for Tightness. Double-click anything to reset it.";

    juce::String noteName (int midi)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }
}

// ---- preset name with next and previous arrows ----------------------------------------------------------------

class DuoEditor::PresetBox final : public juce::Component,
                                   public Hinted
{
public:
    explicit PresetBox (PluginProcessor& p) : processor (p)
    {
        hint = "Starting points. Click the name for the full list.";
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
        g.setColour (tone::text.withAlpha (zone != none ? 0.13f : 0.075f));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);

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

        chevron (b.getX() + arrowWidth * 0.5f + 3.0f, -1.0f, zone == previous);
        chevron (b.getRight() - arrowWidth * 0.5f - 3.0f, 1.0f, zone == next);
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

// ---- Advanced, opening and shutting -----------------------------------------------------------------------------

class DuoEditor::AdvancedHeader final : public juce::Component,
                                        public Hinted
{
public:
    explicit AdvancedHeader (DuoEditor& e) : owner (e)
    {
        hint = "Timing, Pitch, Vibrato, Timbre, Vocal Range, pans, Extra Delay, and the Precise Vibrato, Mono Safe and Match Level switches.";
        setComponentID ("advanced");
        setTitle ("Advanced");
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
        const auto b = getLocalBounds().toFloat();
        juce::Path p;
        p.startNewSubPath (-2.5f, -5.0f);
        p.lineTo (2.5f, 0.0f);
        p.lineTo (-2.5f, 5.0f);
        g.setColour (hovered ? tone::text : tone::muted);
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::rotation (amount * juce::MathConstants<float>::halfPi).translated (6.0f, b.getCentreY()));
        g.setFont (font (Weight::medium, 14.0f));
        g.drawText ("Advanced", b.withTrimmedLeft (20.0f), juce::Justification::centredLeft, false);
    }

    void mouseEnter (const juce::MouseEvent&) override  { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override   { owner.setAdvancedOpen (! owner.isAdvancedOpen(), true); }

private:
    DuoEditor& owner;
    float amount = 0.0f;
    bool hovered = false;
};

// ---- the panel --------------------------------------------------------------------------------------------------

DuoEditor::Panel::Panel (DuoEditor& e, PluginProcessor& p)
    : owner (e),
      processor (p),
      stage (p),
      presets (std::make_unique<PresetBox> (p)),
      advancedHeader (std::make_unique<AdvancedHeader> (e)),
      doublesCount (p.apvts, "doubles", "How many copies of your voice sing with you."),
      bypass (p.apvts, "bypass", "Bypass", "Hear only your own voice, untouched."),
      width (p.apvts, "How far apart the doubles stand. 0 is all in the middle, 100 is hard left and right."),
      tightness (p.apvts, "How closely the doubles follow you. Tight stays right with you. Loose sits later and wanders more."),
      leadLevel (p.apvts, "lead", "Lead", "How loud your own voice is. All the way down is off.", tone::text),
      level1 (p.apvts, "double1", "Double 1", "How loud double 1 is. All the way down is off.", tone::sky),
      level2 (p.apvts, "double2", "Double 2", "How loud double 2 is. All the way down is off.", tone::sky),
      level3 (p.apvts, "double3", "Double 3", "How loud double 3 is. All the way down is off.", tone::sky),
      timing (p.apvts, "timingamount", "Timing", "How much the doubles' slow timing wander moves. 100 is normal.", ValueSlider::Format::percent),
      pitch (p.apvts, "pitchamount", "Pitch", "How much the doubles' quicker pitch wander moves. 100 is normal.", ValueSlider::Format::percent),
      vibrato (p.apvts, "vibrato", "Vibrato", "Changes your vibrato in the doubles: one straighter, one wider. Does nothing without vibrato.", ValueSlider::Format::percent),
      timbre (p.apvts, "timbre", "Timbre", "Gives each double a different size of throat. Watch the singers change size.", ValueSlider::Format::percent),
      leadPan (p.apvts, "leadpan", "Lead Pan", "Where you stand, left to right. You can also drag yourself on the stage.", ValueSlider::Format::pan),
      doublesPan (p.apvts, "doublespan", "Doubles Pan", "Moves all the doubles left or right together, on top of Width.", ValueSlider::Format::pan),
      extraDelay (p.apvts, "extradelay", "Extra Delay", "Adds a fixed delay to every double. Turn it up for a slapback.", ValueSlider::Format::milliseconds),
      range (p.apvts, "range", "Vocal Range", "Where to listen for your pitch. Used only by Vibrato and Timbre."),
      preciseVibrato (p.apvts, "precisevibrato", "Precise Vibrato", "Vibrato follows your real vibrato closely. Off is how it worked before 0.4, kept for older sessions and three presets."),
      monoSafe (p.apvts, "monosafe", "Mono Safe", "The doubles cancel when left and right are added: in mono, and for a tuner or harmony plug-in after Duo, only your own voice is left. Width and Doubles Pan rest."),
      matchLevel (p.apvts, "matchlevel", "Match Level", "Turns you and the doubles down together, so Duo comes out as loud as the vocal that went in. The balance between you and the doubles stays.")
{
    setOpaque (true);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &stage, presets.get(), advancedHeader.get(), &doublesCount, &bypass, &width, &tightness, &leadLevel, &level1, &level2, &level3,
                                                                        &timing, &pitch, &vibrato, &timbre, &leadPan, &doublesPan, &extraDelay, &range, &preciseVibrato, &monoSafe, &matchLevel })
        addAndMakeVisible (c);

    for (auto& r : readouts)
        addAndMakeVisible (r);

    hintLine.setFont (font (Weight::regular, 13.0f));
    hintLine.setColour (juce::Label::textColourId, tone::muted);
    hintLine.setJustificationType (juce::Justification::centredRight);
    hintLine.setInterceptsMouseClicks (false, false);
    hintLine.setText (restingHint, juce::dontSendNotification);
    addAndMakeVisible (hintLine);

    addMouseListener (this, true);
}

DuoEditor::Panel::~Panel()
{
    removeMouseListener (this);
}

void DuoEditor::Panel::paint (juce::Graphics& g)
{
    g.fillAll (tone::base);

    g.setColour (tone::text);
    g.setFont (font (Weight::medium, 31.0f));
    g.drawText ("Duo", juce::Rectangle<float> ((float) margin, (float) headerY, 70.0f, (float) headerHeight), juce::Justification::centredLeft, false);

    // Under the readouts, what they are.
    const auto r = readouts[0].getBounds();
    g.setColour (tone::faint);
    g.setFont (font (Weight::regular, 12.5f));
    g.drawFittedText ("The note you are singing, and how far behind you each double is", juce::Rectangle<int> (level3.getRight() - 4 * faderWidth - 3 * faderGap, r.getBottom() + 2, 4 * faderWidth + 3 * faderGap, captionHeight),
                      juce::Justification::centred, 2);
}

void DuoEditor::Panel::resized()
{
    constexpr int W = designWidth, H = designHeight;

    presets->setBounds (margin + 72, headerY, 236, headerHeight);
    bypass.setBounds (W - margin - 78, headerY + 2, 78, headerHeight - 4);
    doublesCount.setBounds (bypass.getX() - 20 - 190, headerY, 190, headerHeight);

    const int bottomY = H - bottomHeight - 10;
    advancedHeader->setBounds (margin, bottomY, 120, bottomHeight);
    hintLine.setBounds (margin + 130, bottomY, W - 2 * margin - 130, bottomHeight);

    // Advanced takes its room from the stage, a little at a time as it opens.
    const int advancedRoom = juce::roundToInt ((float) advancedHeight * advancedAmount);
    const int contentBottom = bottomY - 4 - advancedRoom;
    const int stageHeight = contentBottom - widthRowHeight - stageY;

    stage.setBounds (margin, stageY, stageWidth, stageHeight);

    // Width lines up exactly with the stage: each handle sits under the double it moves.
    width.setBounds (margin, stage.getBottom() + 2, stageWidth, widthRowHeight - 2);
    width.setSpan (stage.xForPan (-1.0f), stage.xForPan (1.0f));

    tightness.setBounds (stage.getRight() + 6, stageY, tightnessWidth, stageHeight + 10);

    // The levels, in a column of their own on the right.
    const int mixerWidth = 4 * faderWidth + 3 * faderGap;
    const int mixerX = W - margin - mixerWidth;
    const int faderHeight = contentBottom - stageY - readoutHeight - captionHeight - 4;
    const std::array<juce::Component*, 4> faders { &leadLevel, &level1, &level2, &level3 };
    const std::array<int, 4> readoutOrder { StageView::leadIndex, 0, 1, 2 };

    for (int i = 0; i < 4; ++i)
    {
        const int x = mixerX + i * (faderWidth + faderGap);
        faders[(size_t) i]->setBounds (x, stageY, faderWidth, faderHeight);
        readouts[(size_t) readoutOrder[(size_t) i]].setBounds (x, stageY + faderHeight, faderWidth, readoutHeight);
    }

    // Advanced fills from the bottom up, two rows of four, and a column at the right for the three switches added in 0.4.
    const bool showAdvanced = advancedAmount > 0.0f;
    const std::array<juce::Component*, 8> advancedControls { &timing, &pitch, &vibrato, &timbre, &range, &leadPan, &doublesPan, &extraDelay };
    const std::array<juce::Component*, 3> switches { &preciseVibrato, &monoSafe, &matchLevel };
    constexpr int switchWidth = 150, switchHeight = 26, switchGap = 7, columnGap = 24;
    const int columnWidth = (W - 2 * margin - 4 * columnGap - switchWidth) / 4;
    const int firstRowY = contentBottom + 8;

    for (int i = 0; i < 8; ++i)
    {
        auto* c = advancedControls[(size_t) i];
        c->setVisible (showAdvanced);
        c->setAlpha (advancedAmount * ((c == &leadPan || c == &doublesPan) ? width.getAlpha() : 1.0f));
        const int column = i % 4, row = i / 4;
        const int height = c == &range ? 32 : advancedRow;
        c->setBounds (margin + column * (columnWidth + columnGap), firstRowY + row * (advancedRow + 6) + (c == &range ? 8 : 0), columnWidth, height);
    }

    for (int i = 0; i < 3; ++i)
    {
        auto* c = switches[(size_t) i];
        c->setVisible (showAdvanced);
        c->setAlpha (advancedAmount);
        c->setBounds (W - margin - switchWidth, firstRowY + 2 + i * (switchHeight + switchGap), switchWidth, switchHeight);
    }
}

void DuoEditor::Panel::showHint (const juce::String& text)
{
    const auto next = text.isEmpty() ? restingHint : text;

    if (hintLine.getText() != next)
        hintLine.setText (next, juce::dontSendNotification);
}

void DuoEditor::Panel::hintFromComponent (juce::Component* c)
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

void DuoEditor::Panel::mouseEnter (const juce::MouseEvent& e)  { hintFromComponent (e.eventComponent); }
void DuoEditor::Panel::mouseMove (const juce::MouseEvent& e)   { hintFromComponent (e.eventComponent); }
void DuoEditor::Panel::mouseExit (const juce::MouseEvent&)     { showHint ({}); }

// ---- the window ------------------------------------------------------------------------------------------------

DuoEditor::DuoEditor (PluginProcessor& p)
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

    panel.stage.onActivity = [this] { restingFor = 0.0; if (isTimerRunning()) timerCallback(); };

    tick (1.0 / 60.0);

    // At rest a slow timer looks for sound or a changed setting. The screen's refresh only drives the window while something moves.
    startTimer (restingMilliseconds);
}

DuoEditor::~DuoEditor()
{
    stopTimer();
    vblank.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void DuoEditor::resized()
{
    panel.setBounds (0, 0, designWidth, designHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

bool DuoEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void DuoEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processorRef.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);
    restingFor = 0.0;

    if (! animate)
    {
        panel.advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
        panel.resized();
        panel.advancedHeader->setAmount (panel.advancedAmount);
        panel.repaint();
    }
    else if (isTimerRunning())
    {
        timerCallback();
    }
}

bool DuoEditor::isBusy()
{
    return panel.stage.isMoving() || panel.advancedAmount != (isAdvancedOpen() ? 1.0f : 0.0f) || isMouseOverOrDragging (true);
}

void DuoEditor::timerCallback()
{
    // Switching between the two clocks only ever happens here, never from inside the screen's own callback.
    vblank.reset();
    tick ((double) restingMilliseconds * 0.001);

    if (! isBusy())
        return;

    stopTimer();
    lastFrameTime = 0.0;
    frameBudget = 0.0;
    sinceDrawn = 0.0;
    restingFor = 0.0;

    // At most one frame per screen refresh, and never over the cap on faster screens.
    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
    {
        if (isTimerRunning())
            return;

        const double elapsed = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / 60.0;
        lastFrameTime = timestampSeconds;
        sinceDrawn += elapsed;

        // Spend whole refreshes against a running budget, so that the average lands on the cap at any screen rate.
        frameBudget = juce::jmin (frameBudget + elapsed, 0.2);
        const double frameTime = 1.0 / maxFramesPerSecond;

        if (frameBudget + 0.001 < frameTime)
            return;

        frameBudget -= frameTime;
        const double step = sinceDrawn;
        sinceDrawn = 0.0;
        tick (step);

        restingFor = isBusy() ? 0.0 : restingFor + step;

        if (restingFor > 0.5)
            startTimer (restingMilliseconds);
    });
}

void DuoEditor::updateReadouts (double dtSeconds)
{
    sinceReadout += dtSeconds;

    if (sinceReadout < 0.2)
        return;

    sinceReadout = 0.0;
    const bool live = panel.stage.isLive();
    const int count = panel.doublesCount.getCount();

    for (int v = 0; v < DuoEngine::maxDoubles; ++v)
    {
        const bool singing = v < count;
        const float ms = processorRef.getDelayMs (v);
        // Live, the delay it is singing at. At rest, where its mark is.
        const int shown = juce::roundToInt (live ? (double) ms : panel.stage.restMs (v));
        panel.readouts[(size_t) v].setText (singing ? juce::String (shown) + " ms" : juce::String(), live && singing);
    }

    // The note, held for a moment so a passing consonant does not flicker it.
    const float hz = processorRef.getPitchHz();
    const int note = live && hz > 0.0f ? juce::roundToInt (69.0f + 12.0f * std::log2 (hz / 440.0f)) : -1;
    noteHeld = note == noteCandidate ? noteHeld + 1 : 0;
    noteCandidate = note;

    if (noteHeld >= 1 || note == noteShown)
        noteShown = note;

    panel.readouts[StageView::leadIndex].setText (noteShown >= 0 ? noteName (noteShown) : juce::String ("-"), noteShown >= 0);
}

void DuoEditor::tick (double dtSeconds)
{
    panel.stage.advance (dtSeconds);
    panel.presets->refresh();
    updateReadouts (dtSeconds);

    const int count = panel.doublesCount.getCount();
    panel.level1.setSinging (count >= 1);
    panel.level2.setSinging (count >= 2);
    panel.level3.setSinging (count >= 3);

    // A mono track has no left or right, so those controls step back. Mono Safe leaves them nothing to move either.
    const bool monoSafeOn = processorRef.apvts.getRawParameterValue ("monosafe")->load() >= 0.5f;
    const float stereoAlpha = processorRef.getMainBusNumOutputChannels() > 1 && ! monoSafeOn ? 1.0f : 0.45f;
    if (panel.width.getAlpha() != stereoAlpha)
    {
        panel.width.setAlpha (stereoAlpha);
        panel.resized();
    }

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

void DuoEditor::previewHover (const juce::String& paramId)
{
    if (paramId.startsWith ("singer"))
    {
        panel.stage.setHighlight (paramId.getTrailingIntValue());
        panel.showHint (panel.stage.hint);
        return;
    }

    if (auto* c = panel.findChildWithID (paramId))
    {
        if (auto* control = dynamic_cast<ParamControl*> (c))
            control->setForcedHover (true);

        if (auto* hinted = dynamic_cast<Hinted*> (c))
            panel.showHint (hinted->hint);
    }
}
