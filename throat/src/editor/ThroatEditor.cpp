#include "ThroatEditor.h"

using namespace ui;

namespace
{
    // The grid everything sits on.
    constexpr int margin = 28;
    constexpr int viewTop = 72, viewWidth = 744, viewHeight = 530;
    constexpr int sideLeft = 796, sideWidth = 296;
    constexpr int tableTop = 614, tableRow = 21, tableLabelWidth = 60;
    constexpr int footerTop = 692, footerHeight = 22;

    // How many display frames a second, at most. See docs: 30 is an even cadence on 60 and 120 Hz screens, and frame rate is what
    // sets a window's CPU.
    constexpr double maxFramesPerSecond = 30.0;

    const juce::String restingHint = "Drag the five points on the tube. Double-click anything to reset it, click a value to type one.";

    constexpr const char* widthIds[] = { "width1", "width2", "width3", "width4", "width5" };
    constexpr const char* placeIds[] = { nullptr, "place2", "place3", "place4", "place5" };
    constexpr const char* columnNames[] = { "Cords", "Throat", "Back of mouth", "Roof of mouth", "Lips" };

    juce::String howMuch (float ratio, const char* more, const char* less)
    {
        const int percent = juce::roundToInt (std::abs (ratio - 1.0f) * 100.0f);

        if (percent == 0)
            return "The singer's own";

        return juce::String (percent) + "% " + (ratio > 1.0f ? more : less);
    }
}

// ---- preset name with next and previous arrows ----------------------------------------------------------------

class ThroatEditor::PresetBox final : public juce::Component,
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
        g.setColour (tone::text.withAlpha (zone != none ? 0.11f : 0.07f));
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

// ---- a plain button for a one-off action ------------------------------------------------------------------------

class ThroatEditor::SmallButton final : public juce::Component,
                                        public Hinted
{
public:
    SmallButton (const juce::String& textToShow, const juce::String& hintText) : text (textToShow)
    {
        hint = hintText;
        setTitle (textToShow);
        setRepaintsOnMouseActivity (true);
    }

    void paint (juce::Graphics& g) override
    {
        const bool lit = isMouseOverOrDragging();
        g.setColour (tone::text.withAlpha (lit ? 0.12f : 0.07f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 7.0f);
        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (text, getLocalBounds(), juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick != nullptr && getLocalBounds().contains (e.getPosition()))
            onClick();
    }

    std::function<void()> onClick;

private:
    const juce::String text;
};

// ---- the header that opens Advanced ------------------------------------------------------------------------------

class ThroatEditor::AdvancedHeader final : public juce::Component,
                                           public Hinted
{
public:
    explicit AdvancedHeader (ThroatEditor& e) : editor (e)
    {
        hint = "How the voice was sung, breath tone, pitch range and precision. Everything in here keeps working when it is closed.";
        setComponentID ("advanced");
        setTitle ("Advanced");
        setRepaintsOnMouseActivity (true);
    }

    void setState (float amountToShow, int changedCount)
    {
        if (amount != amountToShow || changed != changedCount)
        {
            amount = amountToShow;
            changed = changedCount;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const bool lit = isMouseOverOrDragging();

        const juce::Point<float> c (5.0f, b.getCentreY());
        juce::Path p;
        p.startNewSubPath (-2.5f, -5.0f);
        p.lineTo (2.5f, 0.0f);
        p.lineTo (-2.5f, 5.0f);
        p.applyTransform (juce::AffineTransform::rotation (juce::jmax (0.0f, amount) * juce::MathConstants<float>::halfPi).translated (c));
        g.setColour (lit ? tone::text : tone::muted);
        g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setFont (font (Weight::medium, 14.0f));
        g.drawText ("Advanced", b.withTrimmedLeft (18.0f), juce::Justification::centredLeft, false);

        if (changed > 0 && amount < 0.5f)
        {
            g.setColour (tone::faint);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (juce::String (changed) + " changed", b, juce::Justification::centredRight, false);
        }
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        editor.setAdvancedOpen (! editor.isAdvancedOpen(), true);
    }

private:
    ThroatEditor& editor;
    float amount = -1.0f;
    int changed = 0;
};

// ---- the whole panel -------------------------------------------------------------------------------------------

using Kind = ParamSlider::Kind;
using Unit = ParamSlider::Unit;

ThroatEditor::Panel::Panel (ThroatEditor& e, PluginProcessor& p)
    : owner (e),
      processor (p),
      view (p),
      bypass (p.apvts, "bypass", "Bypass", "Hear the untouched voice. It stays in time with everything else.", ParamToggle::Kind::button),
      length (p.apvts, "length", "Length", "Longer sounds bigger and darker, shorter sounds smaller and brighter. Between 0.80 and 1.20 stays believable.", Kind::big, Unit::ratio),
      width (p.apvts, "width", "Width", "The whole tube wider or narrower. Wider is a little darker and more focused. Between 0.80 and 1.20 stays believable.", Kind::big, Unit::ratio),
      breath (p.apvts, "breath", "Breath", "Adds air, shaped by the same throat as the voice. 100% is a full whisper.", Kind::wide, Unit::percent),
      mix (p.apvts, "mix", "Mix", "How much of the new throat you hear. 100% is all of it.", Kind::wide, Unit::percent),
      output (p.apvts, "output", "Output", "Final level, after everything else.", Kind::wide, Unit::signedDb),
      sourceVoice (p.apvts, "sourceVoice", "Sung as", "How hard the voice coming in was sung."),
      modelVoice (p.apvts, "modelVoice", "Sounds", "How hard it should sound. Set it the same as Sung as to leave the voice alone."),
      range (p.apvts, "range", "Range", "Limits the pitch search. Pick one if the pitch reading wanders."),
      precision (p.apvts, "precision", "Precision", "How narrow the model's formants can be. Try Extreme for big changes or if you hear whistling."),
      pulse (p.apvts, "pulse", "Pulse", "How long the vocal cords stay open. Wider is darker and softer, narrower is brighter.", Kind::row, Unit::signedPercent),
      breathHp (p.apvts, "breathHp", "Air tone", "The breath sits above this. Low is raspy, high is whispery.", Kind::row, Unit::hz),
      levelMatch (p.apvts, "levelMatch", "Level match", "Keeps the level steady as the throat changes.", ParamToggle::Kind::row),
      peakGuard (p.apvts, "peakGuard", "Peak guard", "Holds the output's peaks at or under the input's highest, so a take that did not clip going in cannot clip coming out.", ParamToggle::Kind::row)
{
    setOpaque (true);

    presets = std::make_unique<PresetBox> (p);
    advancedHeader = std::make_unique<AdvancedHeader> (e);
    resetButton = std::make_unique<SmallButton> ("Reset", "Puts Length, Width and all five points back to the singer's own.");
    resetButton->onClick = [&e] { e.resetShape(); };

    length.describe = [] (float v) { return howMuch (v, "longer", "shorter"); };
    width.describe = [] (float v) { return howMuch (v, "wider", "narrower"); };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &view, presets.get(), advancedHeader.get(), resetButton.get(), &bypass,
                                                                        &length, &width, &breath, &mix, &output,
                                                                        &sourceVoice, &modelVoice, &range, &precision, &pulse, &breathHp, &levelMatch, &peakGuard })
        addAndMakeVisible (c);

    for (int i = 0; i < TractView::numPoints; ++i)
    {
        const juce::String name (columnNames[i]);
        pointWidth[(size_t) i] = std::make_unique<ParamSlider> (p.apvts, widthIds[i], name, "Width at this point. 1.00 is the singer's own. Drag up or down, or click to type.", Kind::cell, Unit::ratio);
        addAndMakeVisible (*pointWidth[(size_t) i]);

        if (placeIds[i] != nullptr)
        {
            pointPlace[(size_t) i] = std::make_unique<ParamSlider> (p.apvts, placeIds[i], name, "Where this point sits along the tube, from the cords at 0% to the lips at 100%. Drag up or down, or click to type.", Kind::cell, Unit::percent);
            addAndMakeVisible (*pointPlace[(size_t) i]);
        }
    }

    view.onPointHovered = [this] (int point)
    {
        lightPoint (point);
        showHint (point >= 0 ? TractView::pointHint (point) : juce::String());
    };

    hintLine.setInterceptsMouseClicks (false, false);
    hintLine.setJustificationType (juce::Justification::centredLeft);
    hintLine.setMinimumHorizontalScale (1.0f);
    hintLine.setFont (font (Weight::regular, 13.0f));
    hintLine.setColour (juce::Label::textColourId, tone::muted);
    hintLine.setBorderSize ({});
    addAndMakeVisible (hintLine);
    showHint ({});

    latencyShown = latencyText();

    // Hovering anything in the window updates the hint line.
    addMouseListener (this, true);
}

ThroatEditor::Panel::~Panel()
{
    removeMouseListener (this);
}

juce::String ThroatEditor::Panel::latencyText() const
{
    const double rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    return "Latency " + juce::String (1000.0 * processor.getLatencySamples() / rate, 1) + " ms";
}

void ThroatEditor::Panel::showHint (const juce::String& text)
{
    hintLine.setText (text.isEmpty() ? restingHint : text, juce::dontSendNotification);
}

void ThroatEditor::Panel::lightPoint (int point)
{
    if (point == pointLit)
        return;

    pointLit = point;

    for (int i = 0; i < TractView::numPoints; ++i)
    {
        pointWidth[(size_t) i]->setForcedHover (i == point);
        if (pointPlace[(size_t) i] != nullptr)
            pointPlace[(size_t) i]->setForcedHover (i == point);
    }

    repaint (margin, tableTop, viewWidth, tableRow);
}

void ThroatEditor::Panel::hintFromComponent (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            showHint (hinted->hint);

            int point = -1;
            for (int i = 0; i < TractView::numPoints; ++i)
                if (c == pointWidth[(size_t) i].get() || c == pointPlace[(size_t) i].get())
                    point = i;

            view.setHighlight (point);
            lightPoint (point);
            return;
        }

        if (c == &view)
            return;   // The tract says what it wants through onPointHovered.
    }

    showHint ({});
    view.setHighlight (-1);
    lightPoint (-1);
}

void ThroatEditor::Panel::mouseEnter (const juce::MouseEvent& e)
{
    hintFromComponent (e.eventComponent);
}

void ThroatEditor::Panel::mouseExit (const juce::MouseEvent& e)
{
    if (e.eventComponent != &view)
    {
        showHint ({});
        view.setHighlight (-1);
        lightPoint (-1);
    }
}

int ThroatEditor::Panel::countAdvancedChanges() const
{
    int changed = 0;

    for (const char* id : { "sourceVoice", "modelVoice", "range", "precision", "pulse", "breathHp", "levelMatch", "peakGuard" })
    {
        const auto* p = processor.apvts.getParameter (id);
        changed += std::abs (p->getValue() - p->getDefaultValue()) > 1.0e-4f ? 1 : 0;
    }

    return changed;
}

void ThroatEditor::Panel::layoutSide()
{
    const float a = advancedAmount;
    const auto mixed = [a] (float closed, float open) { return juce::roundToInt (closed + (open - closed) * a); };

    int y = viewTop;
    const auto next = [&y] (juce::Component& c, int height, int gapAfter)
    {
        c.setBounds (sideLeft, y, sideWidth, height);
        y += height + gapAfter;
    };

    next (length, mixed (128, 90), mixed (10, 2));
    next (width, mixed (128, 90), mixed (24, 10));
    next (breath, mixed (56, 44), mixed (6, 0));
    next (mix, mixed (56, 44), mixed (6, 0));
    next (output, mixed (56, 44), mixed (22, 8));
    next (*advancedHeader, 28, 4);

    const bool show = a > 0.01f;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &sourceVoice, &modelVoice, &pulse, &breathHp, &range, &precision, &levelMatch })
    {
        c->setVisible (show);
        c->setAlpha (a);
        next (*c, 34, 2);
    }

    // The two switches share the last row, so the column keeps its height.
    const int half = (sideWidth - 20) / 2;
    peakGuard.setVisible (show);
    peakGuard.setAlpha (a);
    peakGuard.setBounds (levelMatch.getBounds().withLeft (sideLeft + sideWidth - half));
    levelMatch.setBounds (levelMatch.getBounds().withWidth (half));
}

void ThroatEditor::Panel::resized()
{
    view.setBounds (margin, viewTop, viewWidth, viewHeight);
    presets->setBounds (146, 24, 220, 34);

    bypass.setBounds (sideLeft + sideWidth - 86, 24, 86, 34);
    resetButton->setBounds (sideLeft + sideWidth - 86 - 8 - 86, 24, 86, 34);

    const float column = (float) (viewWidth - tableLabelWidth) / (float) TractView::numPoints;
    for (int i = 0; i < TractView::numPoints; ++i)
    {
        const int x = margin + tableLabelWidth + juce::roundToInt (column * (float) i);
        const int w = juce::roundToInt (column) - 8;
        pointWidth[(size_t) i]->setBounds (x, tableTop + tableRow, w, tableRow);
        if (pointPlace[(size_t) i] != nullptr)
            pointPlace[(size_t) i]->setBounds (x, tableTop + tableRow * 2, w, tableRow);
    }

    hintLine.setBounds (margin, footerTop, viewWidth, footerHeight);
    layoutSide();
}

void ThroatEditor::Panel::paint (juce::Graphics& g)
{
    // Flat, and only drawn when the layout changes. Nothing here runs per frame.
    g.fillAll (tone::base);

    g.setColour (tone::text);
    g.setFont (font (Weight::medium, 30.0f));
    g.drawText ("Throat", juce::Rectangle<int> (margin, 18, 110, 46), juce::Justification::centredLeft, false);

    // The table of points, lined up under the tract.
    const float column = (float) (viewWidth - tableLabelWidth) / (float) TractView::numPoints;
    g.setFont (font (Weight::regular, 13.0f));
    g.setColour (tone::faint);
    g.drawText ("Width", juce::Rectangle<int> (margin, tableTop + tableRow, tableLabelWidth, tableRow), juce::Justification::centredLeft, false);
    g.drawText ("Along", juce::Rectangle<int> (margin, tableTop + tableRow * 2, tableLabelWidth, tableRow), juce::Justification::centredLeft, false);

    for (int i = 0; i < TractView::numPoints; ++i)
    {
        const int x = margin + tableLabelWidth + juce::roundToInt (column * (float) i);
        g.setColour (i == pointLit ? tone::text : tone::muted);
        g.drawText (columnNames[i], juce::Rectangle<int> (x, tableTop, juce::roundToInt (column), tableRow), juce::Justification::centredLeft, false);
    }

    // The cords cannot move. Their place is a plain fact, not a control.
    g.setFont (font (Weight::medium, 15.0f));
    g.setColour (tone::faint);
    g.drawText ("0%", juce::Rectangle<int> (margin + tableLabelWidth, tableTop + tableRow * 2, 60, tableRow), juce::Justification::centredLeft, false);

    g.setFont (font (Weight::regular, 13.0f));
    g.drawText (latencyShown, juce::Rectangle<int> (sideLeft, footerTop, sideWidth, footerHeight), juce::Justification::centredRight, false);
}

// ---- the editor ------------------------------------------------------------------------------------------------

ThroatEditor::ThroatEditor (PluginProcessor& p)
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

    // At most one frame per screen refresh, and never over the cap on faster screens. When nothing moves, nothing repaints.
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
        tick (step);
    });
}

ThroatEditor::~ThroatEditor()
{
    vblank.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void ThroatEditor::paint (juce::Graphics& g)
{
    // The panel covers everything but the sliver rounding can leave at some window sizes. JUCE does not skip this paint for a
    // child with a transform, so filling the whole window here would run on every frame.
    g.setColour (tone::base);
    g.fillRect (getWidth() - 2, 0, 2, getHeight());
    g.fillRect (0, getHeight() - 2, getWidth(), 2);
}

void ThroatEditor::resized()
{
    panel.setBounds (0, 0, designWidth, designHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

bool ThroatEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void ThroatEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processorRef.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);

    if (! animate)
    {
        panel.advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
        panel.layoutSide();
        panel.advancedHeader->setState (panel.advancedAmount, panel.countAdvancedChanges());
        panel.repaint();
    }
}

void ThroatEditor::resetShape()
{
    for (const char* id : { "length", "width", "width1", "width2", "width3", "width4", "width5", "place2", "place3", "place4", "place5" })
    {
        auto* p = processorRef.apvts.getParameter (id);

        if (p->getValue() != p->getDefaultValue())
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
    }
}

void ThroatEditor::tick (double dtSeconds)
{
    panel.view.advance (dtSeconds);
    panel.presets->refresh();

    // Advanced glides open and shut, in about a fifth of a second.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;

    if (panel.advancedAmount != target)
    {
        const float next = panel.advancedAmount + (target - panel.advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        panel.advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel.layoutSide();
        panel.repaint (sideLeft, 0, designWidth - sideLeft, footerTop);
        panel.advancedHeader->setState (panel.advancedAmount, panel.countAdvancedChanges());
    }

    // Things that hardly ever change get looked at about twice a second.
    if (--slowCountdown <= 0)
    {
        slowCountdown = 15;
        panel.advancedHeader->setState (panel.advancedAmount, panel.countAdvancedChanges());
        const auto latency = panel.latencyText();

        if (latency != panel.latencyShown)
        {
            panel.latencyShown = latency;
            panel.repaint (sideLeft, footerTop, sideWidth, footerHeight);
        }
    }
}

void ThroatEditor::previewHover (const juce::String& paramId)
{
    for (int i = 0; i < TractView::numPoints; ++i)
    {
        if (paramId == widthIds[i] || (placeIds[i] != nullptr && paramId == placeIds[i]))
        {
            panel.view.setHighlight (i);
            panel.lightPoint (i);
            panel.showHint (TractView::pointHint (i));
            return;
        }
    }

    if (auto* c = panel.findChildWithID (paramId))
    {
        if (auto* slider = dynamic_cast<ParamSlider*> (c))
            slider->setForcedHover (true);

        if (auto* hinted = dynamic_cast<Hinted*> (c))
            panel.showHint (hinted->hint);
    }
}
