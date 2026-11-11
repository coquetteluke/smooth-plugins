#include "KeyEditor.h"

#include "editor/Controls.h"
#include "editor/KeyViews.h"

using namespace keyui;

namespace
{
    constexpr int activeRate = 6;   // the analyser's own rate: nothing can change faster than this
    constexpr const char* flatNames[12] = { "", "Db", "", "Eb", "", "", "Gb", "", "Ab", "", "Bb", "" };

    // How much ink a key's likelihood gets, from 0 to 1. Likelihoods fall away fast, so this opens up the small ones to keep the
    // runners up visible. A key has to look at least a little likely to show at all.
    float inkFor (float candidate, bool heardNow)
    {
        const float amount = std::pow (juce::jmax (0.0f, candidate), 0.35f) * (heardNow ? 1.0f : 0.55f);
        return amount < 0.12f ? 0.0f : juce::jmin (1.0f, amount);
    }

    juce::String keyName (int key, bool minor)
    {
        return juce::String (smoothkey::keyNames[key]) + (minor ? " minor" : " major");
    }

    juce::String percentText (float share)
    {
        return juce::String (juce::roundToInt (share * 100.0f)) + "%";
    }

    // How clearly the key on show leads its runner up, in a word. The margin runs from 0, a tie, to 1, no contest.
    constexpr float closeBelow = 0.5f, veryCloseBelow = 0.2f;

    const char* leadWord (float margin)
    {
        return margin < veryCloseBelow ? "very close" : (margin < closeBelow ? "close" : "well behind");
    }

    juce::String centsText (float hz)
    {
        const int cents = juce::roundToInt (1200.0f * std::log2 (hz / 440.0f));
        return juce::String (std::abs (cents)) + (cents >= 0 ? " cents high" : " cents low");
    }

    // The Advanced line in the footer: its arrow turns as it opens, and it says when something inside has been changed.
    struct AdvancedHeader final : juce::Component, Hinted
    {
        explicit AdvancedHeader (KeyEditor& e) : editor (e)
        {
            setComponentID ("advanced");
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            hint = "Response, the manual key and scale, and Bypass.";
        }

        bool setState (float newAmount, int newChanged)
        {
            if (newAmount == amount && newChanged == changed)
                return false;

            amount = newAmount;
            changed = newChanged;
            repaint();
            return true;
        }

        void paint (juce::Graphics& g) override
        {
            ++paintCount;
            const auto b = getLocalBounds().toFloat();
            const bool lit = isMouseOver();
            const juce::Colour colour = lit ? tone::ink : tone::soft;
            const float wordsWidth = textWidth ("Advanced", medium, 15.0f);
            const float arrowX = b.getRight() - wordsWidth - 16.0f;

            juce::Path p;
            p.startNewSubPath (-3.0f, -5.0f);
            p.lineTo (2.5f, 0.0f);
            p.lineTo (-3.0f, 5.0f);
            p.applyTransform (juce::AffineTransform::rotation (amount * juce::MathConstants<float>::halfPi).translated (arrowX, b.getCentreY()));
            g.setColour (colour);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
            drawTextIn (g, "Advanced", medium, 15.0f, b, Align::right);

            if (changed > 0 && amount < 0.5f)
            {
                g.setColour (tone::soft);
                drawTextIn (g, juce::String (changed) + " changed", book, 14.0f, b.withWidth (arrowX - 12.0f), Align::right);
            }
        }

        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }
        void mouseDown (const juce::MouseEvent&) override   { editor.setAdvancedOpen (! editor.isAdvancedOpen(), true); }

        KeyEditor& editor;
        float amount = -1.0f;
        int changed = -1;
    };
}

// ---- everything on the page ---------------------------------------------------------------------------------------

struct KeyEditor::Panel final : juce::Component
{
    Panel (KeyEditor& e, PluginProcessor& p)
        : owner (e),
          processor (p),
          mode (p.apvts, "mode", { "Listen", "Manual" }, { 0, 1 }, "Listen finds the key. Manual lets you set it yourself."),
          listening (p.apvts, "listening", "Listening", "Turn off to freeze the reading where it is."),
          reset (p.apvts, "Forget everything heard so far and start listening again.", [&p] { p.requestReset(); }),
          lock (p.getLockParameter(), "Lock", "Hold the key on show. Other plug-ins follow it until you turn Lock off.", true),
          source (p.apvts, "source", { "Full Mix", "Single Voice" }, { 0, 1 }, "Full Mix uses the bass and chords. Single Voice is for one vocal or instrument."),
          swap (p.apvts, "swapRelative", "Swap", "Show the relative key instead, for when it says C major and you hear A minor.", true),
          tempoScale (p.apvts, "tempoScale", { "Half", "Normal", "Double" }, { 1, 0, 2 }, "Halve or double the tempo shown, if it is counting at the wrong speed."),
          response (p.apvts, "response", { "Fast", "Stable" }, { 0, 1 }, "Stable waits until it is sure. Fast reads sooner and may move more."),
          manualKey (p.apvts, "The key to use in Manual. Click to type one, such as F# or Bb.", [&e] (int key, int scale) { e.setManualKey (key, scale); }),
          manualScale (p.apvts, "manualScale", { "Major", "Minor" }, { 0, 1 }, "The scale to use in Manual."),
          bypass (p.apvts, "bypass", "Bypass", "Stops listening. The audio passes through unchanged either way.", true),
          advancedHeader (e)
    {
        setOpaque (true);

        const auto addAll = [this] (std::initializer_list<juce::Component*> list)
        {
            for (auto* c : list)
                addAndMakeVisible (*c);
        };

        addAll ({ &hero, &circle, &centreBig, &centreSmall, &keyboard, &title, &status, &tuningLabel, &tuningValue, &tuningNote, &tempoLabel, &tempoValue,
                  &hintLine, &noteLine, &mode, &listening, &reset, &lock, &source, &swap, &tempoScale, &advancedHeader, &second });
        addChildComponent (response);
        addChildComponent (manualKey);
        addChildComponent (manualScale);
        addChildComponent (bypass);
        addChildComponent (responseLabel);
        addChildComponent (manualKeyLabel);
        addChildComponent (manualScaleLabel);

        noteLine.setComponentID ("note");
        status.setComponentID ("status");
        centreBig.setComponentID ("centreBig");
        centreSmall.setComponentID ("centreSmall");
        title.setText ("Key");
        tuningLabel.setText ("Tuning");
        tempoLabel.setText ("Tempo");
        responseLabel.setText ("Response");
        manualKeyLabel.setText ("Manual key");
        manualScaleLabel.setText ("Manual scale");

        for (auto* r : { &title, &tuningLabel, &tuningNote, &tempoLabel, &hintLine, &noteLine, &responseLabel, &manualKeyLabel, &manualScaleLabel, &centreSmall })
            r->setInterceptsMouseClicks (false, false);

        tuningValue.hint = "Where the music sits against A at 440 Hz. Red means it is 1 Hz or more away.";
        tempoValue.hint = "The tempo it hears, after Half or Double.";
        centreBig.hint = "How sure it is of the key on show.";
        status.hint = "What it is doing right now.";

        hero.listenHint = "The key it hears. Other plug-ins can read this too.";
        hero.manualHint = "Click to type a key, such as F# minor or Bb.";
        circle.listenHint = "Darker keys look more likely. Major outside, minor inside. Red is the key on show.";
        circle.manualHint = "Click a key to set it.";
        keyboard.listenHint = "The notes of the key, with the home note in red. Dots are the notes sounding now.";
        keyboard.manualHint = "Click a note to make it the key.";

        hero.onTyped = [&e] (const juce::String& text)
        {
            int key = 0, scale = -1;
            if (parseKey (text, key, scale))
                e.setManualKey (key, scale);
        };
        const auto resetManual = [&e, &p]
        {
            e.setManualKey (juce::roundToInt (defaultOf (*p.apvts.getParameter ("manualKey"))), juce::roundToInt (defaultOf (*p.apvts.getParameter ("manualScale"))));
        };
        hero.onReset = resetManual;
        circle.onReset = resetManual;
        circle.onPick = [&e] (int key, bool minor) { e.setManualKey (key, minor ? 1 : 0); };
        // The key the block named when it was last drawn, so a click takes the key that was read, not one that arrived since.
        second.onAction = [this]
        {
            if (offeredKey >= 0)
                owner.useKey (offeredKey, offeredMinor);
        };
        keyboard.onPick = [&e] (int note) { e.setManualKey (note, -1); };

        // Hovering anything in the window updates the hint line.
        addMouseListener (this, true);
    }

    ~Panel() override
    {
        removeMouseListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::paper);
    }

    void layout (float advanced)
    {
        constexpr int W = designWidth, H = designHeight, margin = 36;
        constexpr int rowY = 24, rowH = 36, rightColumn = 404, bandH = 128, advancedH = 84;

        title.setBounds (margin, rowY, 72, rowH);
        int x = margin + 66;
        mode.setBounds (x, rowY, 168, rowH);                  x += 168 + 10;
        listening.setBounds (x, rowY, 96, rowH);              x += 96 + 10;
        reset.setBounds (x, rowY, 70, rowH);                  x += 70 + 10;
        lock.setBounds (x, rowY, 70, rowH);                   x += 70 + 28;
        source.setBounds (x, rowY, 214, rowH);
        status.setBounds (W - margin - 250, rowY, 250, rowH);

        const int footerTop = H - 40;
        const int bodyBottom = footerTop - 8 - juce::roundToInt ((float) advancedH * advanced);
        const int bandTop = bodyBottom - bandH;
        const int rightX = W - margin - rightColumn;
        const int bodyTop = rowY + rowH + 20;
        const int bodyH = bandTop - 14 - bodyTop;

        // The reading, and under it the line for the runner up.
        hero.setBounds (margin, bodyTop, rightX - 28 - margin, bodyH - secondH - secondGap);
        swap.setBounds (hero.swapBounds().translated (hero.getX(), hero.getY()));
        second.setBounds (margin, hero.getBottom() + secondGap, hero.getWidth(), secondH);

        const int diameter = juce::jmin (rightColumn, bodyH);
        circle.setBounds (rightX + (rightColumn - diameter) / 2, bodyTop + juce::jmin (6, bodyH - diameter), diameter, diameter);

        const auto c = circle.getBounds().toFloat().getCentre();
        const float hole = circle.holeRadius();
        // Both are opaque, so they must stay inside the hole or they cut into the ring.
        const float boxW = hole * 1.62f;
        centreBig.setBounds (juce::Rectangle<float> (c.x - boxW * 0.5f, c.y - hole * 0.54f, boxW, hole * 0.70f).getSmallestIntegerContainer());
        centreSmall.setBounds (juce::Rectangle<float> (c.x - boxW * 0.42f, c.y + hole * 0.16f, boxW * 0.84f, hole * 0.36f).getSmallestIntegerContainer());

        keyboard.setBounds (margin, bandTop, rightX - 28 - margin, bandH);

        const int tempoX = rightX + 198, valueH = 42;
        tuningLabel.setBounds (rightX, bandTop - 2, 190, 22);
        tuningValue.setBounds (rightX, bandTop + 22, 190, valueH);
        tuningNote.setBounds (rightX, bandTop + 22 + valueH + 8, 190, 34);
        tempoLabel.setBounds (tempoX, bandTop - 2, rightColumn - 198, 22);
        tempoValue.setBounds (tempoX, bandTop + 22, rightColumn - 198, valueH);
        tempoScale.setBounds (tempoX, bandTop + 22 + valueH + 8, rightColumn - 198, 34);

        const bool showAdvanced = advanced > 0.02f;
        const int labelY = bodyBottom + 12, controlY = labelY + 24, controlH = 36;
        x = margin;

        const auto place = [&] (juce::Component* label, juce::Component& control, int width)
        {
            if (label != nullptr)
            {
                label->setBounds (x, labelY, width, 22);
                label->setVisible (showAdvanced);
                label->setAlpha (advanced);
            }

            control.setBounds (x, controlY, width, controlH);
            control.setVisible (showAdvanced);
            control.setAlpha (advanced);
            x += width + 40;
        };

        place (&manualKeyLabel, manualKey, 96);
        place (&manualScaleLabel, manualScale, 168);
        place (&responseLabel, response, 168);
        place (nullptr, bypass, 96);

        hintLine.setBounds (margin, footerTop, hintRoom(), 28);
        noteLine.setBounds (W - margin - 170 - noteWidth, footerTop, noteWidth, 28);
        advancedHeader.setBounds (W - margin - 170, footerTop, 170, 28);
    }

    // The footer shares its line with the note about key.json, and the note always wins.
    int hintRoom() const
    {
        return designWidth - 2 * 36 - 170 - (noteLine.getText().isNotEmpty() ? noteWidth + 12 : 12);
    }

    void showHint (const juce::String& text)
    {
        const auto wanted = text.isEmpty() ? restingHint : text;
        hintLine.setText (textWidth (wanted, book, 15.0f) <= (float) hintRoom() ? wanted : restingHint);
    }

    void hintFromComponent (juce::Component* c)
    {
        for (; c != nullptr && c != this; c = c->getParentComponent())
        {
            if (auto* hinted = dynamic_cast<Hinted*> (c))
            {
                if (hinted->hint.isNotEmpty())
                {
                    showHint (hinted->hint);
                    return;
                }
            }
        }

        showHint ({});
    }

    void mouseEnter (const juce::MouseEvent& e) override  { hintFromComponent (e.eventComponent); }
    void mouseExit (const juce::MouseEvent&) override     { showHint ({}); }

    int countAdvancedChanges() const
    {
        int count = 0;

        for (auto* id : { "response", "manualKey", "manualScale", "bypass" })
        {
            auto* param = processor.apvts.getParameter (id);
            if (std::abs (param->getValue() - param->getDefaultValue()) > 1.0e-4f)
                ++count;
        }

        return count;
    }

    static constexpr int noteWidth = 400, secondH = 34, secondGap = 8;
    int offeredKey = -1;
    bool offeredMinor = false;

    KeyEditor& owner;
    PluginProcessor& processor;
    const juce::String restingHint { "Double-click any control to put it back." };

    HeroView hero;
    CircleView circle;
    KeyboardView keyboard;
    SecondLine second;
    Readout centreBig { bold, 38.0f, tone::ink, Align::centre }, centreSmall { book, 15.0f, tone::soft, Align::centre };
    Readout title { heavy, 30.0f, tone::ink }, status { medium, 17.0f, tone::ink, Align::right };
    Readout tuningLabel { book, 15.0f, tone::soft }, tuningValue { semi, 34.0f, tone::ink }, tuningNote { book, 15.0f, tone::soft };
    Readout tempoLabel { book, 15.0f, tone::soft }, tempoValue { semi, 34.0f, tone::ink };
    Readout hintLine { book, 15.0f, tone::soft }, noteLine { medium, 14.0f, tone::red, Align::right };
    Readout responseLabel { book, 15.0f, tone::soft }, manualKeyLabel { book, 15.0f, tone::soft }, manualScaleLabel { book, 15.0f, tone::soft };

    Segments mode;
    Pill listening;
    ResetButton reset;
    Pill lock;
    Segments source;
    Pill swap;
    Segments tempoScale, response;
    KeyField manualKey;
    Segments manualScale;
    Pill bypass;
    AdvancedHeader advancedHeader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
};

// ---- the editor ---------------------------------------------------------------------------------------------------

KeyEditor::KeyEditor (PluginProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&lookAndFeel);
    panel = std::make_unique<Panel> (*this, p);
    addAndMakeVisible (*panel);
    setOpaque (true);   // saves clearing the window under every repaint

    using Proc = PluginProcessor;
    const double ratio = (double) designWidth / (double) designHeight;
    setResizable (true, true);
    setResizeLimits (Proc::minWindowWidth, juce::roundToInt (Proc::minWindowWidth / ratio), Proc::maxWindowWidth, juce::roundToInt (Proc::maxWindowWidth / ratio));
    getConstrainer()->setFixedAspectRatio (ratio);

    advancedAmount = p.advancedOpen.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
    panel->setBounds (0, 0, designWidth, designHeight);
    panel->layout (advancedAmount);
    panel->showHint ({});

    const int savedWidth = juce::jlimit (Proc::minWindowWidth, Proc::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    constructed = true;

    refresh();
    timerRate = activeRate;
    startTimerHz (timerRate);
}

KeyEditor::~KeyEditor()
{
    stopTimer();
    panel.reset();
    setLookAndFeel (nullptr);
}

juce::Component& KeyEditor::getPanel()
{
    return *panel;
}

juce::Rectangle<int> KeyEditor::getKeyLetterBounds()
{
    return panel->hero.keyLetterBounds().translated (panel->hero.getX(), panel->hero.getY());
}

void KeyEditor::paint (juce::Graphics& g)
{
    // The panel covers all but what rounding leaves. JUCE does not skip this paint for a child with a transform, so filling the
    // whole window here would run on every repaint.
    g.setColour (tone::paper);
    const int panelBottom = juce::roundToInt ((float) designHeight * (float) getWidth() / (float) designWidth);
    g.fillRect (getWidth() - 2, 0, 2, getHeight());
    g.fillRect (0, panelBottom - 2, getWidth(), juce::jmax (2, getHeight() - panelBottom + 2));
}

void KeyEditor::resized()
{
    if (panel == nullptr)
        return;

    panel->setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processor.windowWidth.store (juce::jlimit (PluginProcessor::minWindowWidth, PluginProcessor::maxWindowWidth, getWidth()), std::memory_order_relaxed);
}

bool KeyEditor::isAdvancedOpen() const noexcept
{
    return processor.advancedOpen.load (std::memory_order_relaxed);
}

void KeyEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processor.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);

    if (animate)
    {
        lastTick = juce::Time::getMillisecondCounterHiRes();
        timerRate = 60;
        startTimerHz (timerRate);
        return;
    }

    advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
    panel->layout (advancedAmount);
    panel->advancedHeader.setState (advancedAmount, panel->countAdvancedChanges());
    panel->repaint();
}

void KeyEditor::setManualKey (int key, int scale)
{
    auto& keyParam = *processor.apvts.getParameter ("manualKey");
    auto& scaleParam = *processor.apvts.getParameter ("manualScale");

    if (juce::roundToInt (keyParam.convertFrom0to1 (keyParam.getValue())) != key)
        setAsGesture (keyParam, (float) key);

    if (scale >= 0 && juce::roundToInt (scaleParam.convertFrom0to1 (scaleParam.getValue())) != scale)
        setAsGesture (scaleParam, (float) scale);
}

void KeyEditor::useKey (int key, bool minor)
{
    setManualKey (key, minor ? 1 : 0);
    auto& modeParam = *processor.apvts.getParameter ("mode");

    if (juce::roundToInt (modeParam.convertFrom0to1 (modeParam.getValue())) != 1)
        setAsGesture (modeParam, 1.0f);
}

juce::String KeyEditor::getSecondLineText() const  { return panel->second.allText(); }
juce::String KeyEditor::getUseText() const         { return panel->second.get().action; }
juce::String KeyEditor::getCentreText() const      { return (panel->centreBig.getText() + " " + panel->centreSmall.getText()).trim(); }

void KeyEditor::previewHover (const juce::String& componentId)
{
    if (auto* c = panel->findChildWithID (componentId))
        panel->hintFromComponent (c);
}

void KeyEditor::timerCallback()
{
    refresh();

    // Advanced glides open and shut, in about a fifth of a second.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;

    if (advancedAmount != target)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float seconds = (float) juce::jmin (0.05, (now - lastTick) * 0.001);
        lastTick = now;
        const float next = advancedAmount + (target - advancedAmount) * (1.0f - std::exp (-seconds / 0.05f));
        advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel->layout (advancedAmount);
        panel->repaint();
        quietTicks = 0;
        return;
    }

    // Readings arrive about six times a second, and often do not change. A stopped session polls more slowly and draws nothing.
    const int wanted = quietTicks > 12 ? 3 : activeRate;

    if (wanted != timerRate)
    {
        timerRate = wanted;
        startTimerHz (timerRate);
    }
}

void KeyEditor::refresh()
{
    const auto d = processor.getDisplay();
    auto& p = *panel;
    bool moved = false;

    const bool bypassed = processor.apvts.getRawParameterValue ("bypass")->load (std::memory_order_relaxed) >= 0.5f;

    // A key is held, or Lock is on and still waiting for one.
    const bool waiting = d.lockOn && ! d.locked;
    // Picking a key by hand is for Manual, and not while a key is held: the held one would stay on show whatever was clicked.
    const bool picking = d.manual && ! d.locked;

    if (d.locked) statusText = "Locked";
    else if (d.manual) statusText = "Set by hand";
    else if (bypassed) statusText = "Bypassed";
    else if (! d.active) statusText = "Paused";
    else if (waiting) statusText = "Lock waiting for a sure key";
    else if (! d.signal) statusText = d.key >= 0 ? "No audio, last key heard" : "Waiting for audio";
    else if (d.unsure) statusText = "Hearing audio, not sure";
    else statusText = d.key >= 0 ? "Hearing audio" : "Hearing audio, no key yet";

    // A held key is as certain as one set by hand, so it never fades.
    const bool dimmed = d.key < 0 || (! d.manual && ! d.locked && (! d.signal || ! d.active));

    HeroView::Content hero;
    hero.hasKey = d.key >= 0;
    // In a close call the big letters go grey, as they do for a key that is no longer sounding: from across the room a
    // black key reads as an answer, and this is not one yet.
    hero.dimmed = dimmed || d.unsure;
    hero.manual = picking;

    if (d.key >= 0)
    {
        keyText = smoothkey::keyNames[d.key];
        hero.scale = d.minor ? "minor" : "major";
        hero.otherName = juce::String (flatNames[d.key]).isNotEmpty() ? "Also written " + juce::String (flatNames[d.key]) : juce::String();
        const int rel = d.minor ? (d.key + 3) % 12 : (d.key + 9) % 12;
        hero.relative = juce::String (smoothkey::keyNames[rel]) + (d.minor ? " major" : " minor");
    }
    else
    {
        keyText = "--";
        hero.scale = d.signal && d.active ? "no key yet" : "waiting for audio";
    }

    hero.key = d.key >= 0 ? keyText : juce::String ("?");
    moved = p.hero.set (hero) || moved;

    static const juce::String lockedHint { "Locked on this key. Turn Lock off to change it." };
    p.hero.hint = d.locked ? lockedHint : (picking ? p.hero.manualHint : p.hero.listenHint);

    // Under the reading: the runner up and how close it is, both keys when it cannot tell them apart, or the lock.
    SecondLine::Content line;
    const bool hearing = d.active && d.signal && d.heardKey >= 0;
    p.offeredKey = -1;
    p.second.hint = {};

    if (d.locked)
    {
        line.tag = "Locked";
        p.second.hint = "This key is held. Under it is what the music sounds like right now.";

        if (hearing)
        {
            line.label = "Hears";
            line.text = keyName (d.heardKey, d.heardMinor);
            line.detail = percentText (d.heardConfidence);
        }
        else
        {
            line.label = "Holding this key";
        }
    }
    else if (! d.manual && d.key >= 0 && d.altKey >= 0)
    {
        if (d.unsure)
        {
            line.tag = "Not sure";
            line.tagRed = true;
            line.text = keyName (d.key, d.minor) + " or " + keyName (d.altKey, d.altMinor);
            p.second.hint = "Two keys fit about equally. Lock keeps the one on show, or use the other.";
        }
        else
        {
            line.label = "Runner up";
            line.text = keyName (d.altKey, d.altMinor);
            line.detail = percentText (d.altConfidence) + ", " + leadWord (d.margin);
            p.second.hint = "The next most likely key, and how close it is to the one on show.";
        }

        line.action = "Use " + keyName (d.altKey, d.altMinor);
        p.offeredKey = d.altKey;
        p.offeredMinor = d.altMinor;
    }
    else if (waiting && ! d.manual)
    {
        line.label = "Lock is on. It will hold the first sure key.";
    }

    moved = p.second.set (line) || moved;

    // The circle shows what it hears, so it stays empty in Manual, where it is not listening.
    // Eased over about half a second, and a key only redraws when it has really moved, so the circle breathes and does not flicker.
    for (size_t k = 0; k < 24; ++k)
    {
        const float target = d.manual ? 0.0f : inkFor (d.candidates[k], d.signal && d.active);
        candidateInk[k] += (target - candidateInk[k]) * 0.3f;
        const float inSteps = candidateInk[k] * (float) CircleView::levels;

        if (target == 0.0f && candidateInk[k] < 0.05f)
            candidateLevels[k] = 0;
        else if (std::abs (inSteps - (float) candidateLevels[k]) > 0.7f)
            candidateLevels[k] = (juce::uint8) juce::roundToInt (inSteps);
    }

    const int shownKey = d.key >= 0 ? d.key + (d.minor ? 12 : 0) : -1;
    moved = p.circle.set (candidateLevels, shownKey, dimmed, picking) || moved;

    moved = p.keyboard.setScale (d.key, d.minor, dimmed, picking) || moved;

    if (d.locked)
    {
        p.circle.hint = lockedHint;
        p.keyboard.hint = lockedHint;
    }

    std::array<float, 12> notes {};
    if (d.active)
        notes = d.liveNotes;
    moved = p.keyboard.setNotes (notes) || moved;

    // How sure, in the middle of the circle. A held key is not a measure of anything, so it says Locked and no number.
    if (d.locked)
    {
        moved = p.centreBig.setText ({}) || moved;
        moved = p.centreSmall.set ("Locked", tone::ink) || moved;
    }
    else if (d.manual)
    {
        moved = p.centreBig.setText ({}) || moved;
        moved = p.centreSmall.set ("Click a key", tone::soft) || moved;
    }
    else
    {
        moved = p.centreBig.set (d.key >= 0 ? percentText (d.confidence) : juce::String(), dimmed ? tone::faint : tone::ink) || moved;
        moved = p.centreSmall.set (d.key >= 0 ? (d.unsure ? "not sure" : "sure") : "No key yet", d.unsure && d.key >= 0 ? tone::red : tone::soft) || moved;
    }

    moved = p.status.set (statusText, dimmed && ! d.manual && ! d.locked ? tone::soft : tone::ink) || moved;
    p.status.setDot (d.active && d.signal);

    noteText = processor.hasFileFailed() ? "Could not save key.json, so other plug-ins will not see this key" : "";

    if (p.noteLine.setText (noteText))
    {
        moved = true;
        p.noteLine.setVisible (noteText.isNotEmpty());
        p.hintLine.setBounds (p.hintLine.getBounds().withWidth (p.hintRoom()));
        p.showHint ({});
    }

    juce::String tuning, tuningNote, tempo;
    bool referenceOff = false, tuningInk = false;

    if (d.manual)
    {
        tuning = "A 440 Hz";
        tempo = "No beat";
        tuningNote = "Not listening";
        tuningInk = true;
    }
    else
    {
        // Tuning is measured from the music, so under a held key it goes by what is heard.
        const bool measured = d.locked ? d.heardKey >= 0 : d.key >= 0;
        const bool live = d.locked ? hearing : ! dimmed;
        tuning = measured ? "A " + juce::String (d.referenceHz, 1) + " Hz" : "No reading";
        referenceOff = measured && std::abs (d.referenceHz - 440.0f) >= 1.0f;
        tempo = d.shownBpm >= 1.0f ? juce::String (d.shownBpm, 1) + " BPM" : "No beat";
        tuningNote = referenceOff ? centsText (d.referenceHz) : (measured ? "In tune with A 440" : juce::String());
        tuningInk = measured && live;
    }

    moved = p.tuningValue.set (tuning, referenceOff ? tone::red : (tuningInk ? tone::ink : tone::faint)) || moved;
    moved = p.tuningNote.set (tuningNote, referenceOff ? tone::red : tone::soft) || moved;
    moved = p.tempoValue.set (tempo, d.shownBpm >= 1.0f && ! d.manual && ! dimmed ? tone::ink : tone::faint) || moved;

    // These do nothing in Manual, and say so. They still work, and automation still moves them.
    p.listening.setDimmed (d.manual);
    p.swap.setDimmed (d.manual || d.locked);
    p.source.setDimmed (d.manual);
    p.response.setDimmed (d.manual);
    p.tempoScale.setDimmed (d.manual);

    p.advancedHeader.setState (advancedAmount, p.countAdvancedChanges());

    quietTicks = moved ? 0 : quietTicks + 1;
}
