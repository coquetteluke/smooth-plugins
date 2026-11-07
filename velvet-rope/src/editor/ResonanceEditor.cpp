#include "ResonanceEditor.h"

using namespace ui;

namespace
{
    // The grid everything sits on.
    constexpr int margin = 20;
    constexpr int graphRight = 812;       // the plot's right edge. The dB scale sits between here and the side column
    constexpr int sideLeft = 860;         // the side column starts here
    constexpr int graphTop = 64;
    constexpr int footerHeight = 60;
    constexpr int stripHeight = 58;
    constexpr int axisHeight = 22;        // the frequency labels under the plot
    constexpr int labelColumn = 84;

    // How many display frames a second, at most. The engine publishes about 47 graph frames a second, so drawing more than
    // that only adds in-between frames. This cap is about a quarter off the window's CPU at 60 frames, and the graph's
    // glide hides the odd skipped refresh.
    constexpr double maxFramesPerSecond = 45.0;

    const juce::String restingHint = "Drag the dots on the graph. Double-click anything to reset it.";
}

// ---- preset name with next and previous arrows ----------------------------------------------------------------

class ResonanceEditor::PresetBox final : public juce::Component,
                                         public Hinted
{
public:
    explicit PresetBox (SmoothResonanceProcessor& p) : processor (p)
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
        g.setColour (tone::text.withAlpha (zone != none ? 0.09f : 0.06f));
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

    SmoothResonanceProcessor& processor;
    int shown = -1;
    Zone zone = none;
};

// ---- how much is coming out right now --------------------------------------------------------------------------

class ResonanceEditor::PeakReading final : public juce::Component
{
public:
    explicit PeakReading (SmoothResonanceProcessor& p) : processor (p)
    {
        setInterceptsMouseClicks (false, false);
    }

    void advance (double dt)
    {
        // What you hear after Mix, so this agrees with the graph.
        const float raw = processor.maxReduction->getReading();
        const float mix = juce::jlimit (0.0f, 1.0f, getReal (*processor.parameters.getParameter ("mix")) * 0.01f);
        const float now = raw <= 0.0f ? 0.0f : -20.0f * std::log10 (juce::jmax (1.0e-4f, 1.0f - mix + mix * std::pow (10.0f, -raw * 0.05f)));
        const float hz = processor.maxReductionFreq->getReading();
        const auto seconds = (float) dt;
        const bool isBypassed = processor.parameters.getParameter ("bypass")->getValue() > 0.5f;

        // Up straight away, down slowly enough to read.
        if (now >= shownDb)
            shownDb = now;
        else
            shownDb += (now - shownDb) * (1.0f - std::exp (-seconds / 0.35f));

        // A jump to another peak snaps there. Gliding would pass through frequencies where nothing is happening.
        if (now > 0.5f && hz > 0.0f)
        {
            if (std::abs (std::log2 (hz / shownHz)) > 0.25f)
                shownHz = hz;
            else
                shownHz += (hz - shownHz) * (1.0f - std::exp (-seconds / 0.08f));
        }

        refreshCountdown -= seconds;

        if (refreshCountdown <= 0.0f)
        {
            refreshCountdown = 0.1f;

            auto top = juce::String (juce::jmax (0.0f, shownDb), 1) + " dB";
            auto where = shownDb < 0.3f ? juce::String ("Nothing right now") : "around " + hertzText (shownHz);

            if (top != amountText || where != whereText || isBypassed != bypassed)
            {
                amountText = top;
                whereText = where;
                bypassed = isBypassed;
                repaint();
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const bool active = shownDb >= 0.3f;

        g.setColour (active && ! bypassed ? tone::gold : tone::faint);
        g.setFont (font (Weight::regular, 30.0f));
        g.drawText (amountText, b.removeFromRight (118.0f), juce::Justification::centredRight, false);

        b.removeFromRight (12.0f);
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (bypassed ? "Would take out" : "Taking out", b.withTrimmedBottom (b.getHeight() * 0.5f), juce::Justification::bottomRight, false);
        g.drawText (whereText, b.withTrimmedTop (b.getHeight() * 0.5f), juce::Justification::topRight, false);
    }

private:
    SmoothResonanceProcessor& processor;
    float shownDb = 0.0f, shownHz = 1000.0f, refreshCountdown = 0.0f;
    bool bypassed = false;
    juce::String amountText { "0.0 dB" }, whereText { "Nothing right now" };
};

// ---- which dot the strip below the graph is showing ------------------------------------------------------------

class ResonanceEditor::BandPicker final : public juce::Component,
                                          public Hinted
{
public:
    explicit BandPicker (ResonanceGraph& g) : graph (g)
    {
        hint = "Pick a dot. The values next to this belong to it.";
        setTitle ("Selected dot");
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText ("Dot", juce::Rectangle<float> (0.0f, 6.0f, 60.0f, 18.0f), juce::Justification::centredLeft, false);

        for (int b = 0; b < 3; ++b)
        {
            const auto c = cellFor (b);
            const bool on = graph.getSelectedBand() == b;
            g.setColour (on ? tone::text : tone::text.withAlpha (hovered == b ? 0.12f : 0.06f));
            g.fillEllipse (c);
            g.setColour (on ? tone::well : (hovered == b ? tone::text : tone::muted));
            g.setFont (font (Weight::medium, 12.0f));
            g.drawText (juce::String (b + 1), c, juce::Justification::centred, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (cellAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override   { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = cellAt (e.position);

        if (hit >= 0)
        {
            graph.setSelectedBand (hit);
            repaint();
        }
    }

private:
    juce::Rectangle<float> cellFor (int b) const
    {
        return { (float) b * 28.0f, 26.0f, 22.0f, 22.0f };
    }

    int cellAt (juce::Point<float> p) const
    {
        for (int b = 0; b < 3; ++b)
            if (cellFor (b).expanded (2.0f).contains (p))
                return b;

        return -1;
    }

    void setHovered (int b)
    {
        if (b != hovered)
        {
            hovered = b;
            repaint();
        }
    }

    ResonanceGraph& graph;
    int hovered = -1;
};

// ---- the line that opens the rest of the controls --------------------------------------------------------------

class ResonanceEditor::AdvancedHeader final : public juce::Component,
                                              public Hinted
{
public:
    explicit AdvancedHeader (ResonanceEditor& e) : editor (e)
    {
        hint = "Stereo, tilts, trim and side chain. Everything in here keeps working when it is closed.";
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

        // The chevron turns as the section opens.
        const juce::Point<float> c (5.0f, b.getCentreY());
        juce::Path p;
        p.startNewSubPath (-2.5f, -5.0f);
        p.lineTo (2.5f, 0.0f);
        p.lineTo (-2.5f, 5.0f);
        p.applyTransform (juce::AffineTransform::rotation (amount * juce::MathConstants<float>::halfPi).translated (c));
        g.setColour (lit ? tone::text : tone::muted);
        g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setFont (font (Weight::medium, 13.0f));
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
    ResonanceEditor& editor;
    float amount = -1.0f;
    int changed = -1;
};

// ---- the whole panel -------------------------------------------------------------------------------------------

using Kind = ParamSlider::Kind;
using Unit = ParamSlider::Unit;

ResonanceEditor::Panel::Panel (ResonanceEditor& e, SmoothResonanceProcessor& p)
    : owner (e),
      processor (p),
      graph (p),
      depth (p.parameters, "depth", "Depth", "The most any one spot can be turned down.", Kind::bigDial, Unit::db, tone::gold),
      sharpness (p.parameters, "sharpness", "Sharpness", "Low treats wide areas. High treats narrow spots.", Kind::dial, Unit::percent),
      selectivity (p.parameters, "selectivity", "Selectivity", "Low catches small bumps too. High catches only the worst.", Kind::dial, Unit::percent),
      attack (p.parameters, "attack", "Attack", "How fast it grabs a harsh peak.", Kind::dial, Unit::ms),
      release (p.parameters, "release", "Release", "How fast it lets go.", Kind::dial, Unit::ms),
      mode (p.parameters, "mode", "Mode", "Soft compares each spot to its neighbours. Hard uses a fixed level."),
      hardThreshold (p.parameters, "hardThreshold", "Threshold", "In Hard mode, the level where it starts working.", Kind::row, Unit::db),
      stereoMode (p.parameters, "stereoMode", "Stereo", "Stereo treats left and right. Mid/Side treats center and sides."),
      link (p.parameters, "link", "Link", "At 100% both sides move together. Lower treats them apart.", Kind::row, Unit::percent),
      stereoFocus (p.parameters, "stereoFocus", "Focus", "Moves the work toward one side. Left of center is left or mid.", Kind::row, Unit::signedPercent),
      sharpnessTilt (p.parameters, "sharpnessTilt", "Sharp tilt", "Right of center: sharper in the highs, wider in the lows.", Kind::row, Unit::signedPercent),
      attackTilt (p.parameters, "attackTilt", "Attack tilt", "Right of center: slower in the highs, faster in the lows.", Kind::row, Unit::signedPercent),
      releaseTilt (p.parameters, "releaseTilt", "Release tilt", "Right of center: slower in the highs, faster in the lows.", Kind::row, Unit::signedPercent),
      wetTrim (p.parameters, "wetTrim", "Wet trim", "Level of the treated sound before Mix.", Kind::row, Unit::signedDb),
      sideChain (p.parameters, "sideChain", "Use", "Listens to another track to decide what to turn down.", tone::text),
      sideChainListen (p.parameters, "sideChainListen", "Listen", "Plays the side chain input, to check it.", tone::text),
      lowFreq (p.parameters, "lowFreq", "Low", "Nothing below this is touched.", Kind::field, Unit::hz),
      highFreq (p.parameters, "highFreq", "High", "Nothing above this is touched.", Kind::field, Unit::hz),
      bypass (p.parameters, "bypass", "Bypass", "Hear the untouched sound. The track stays in time.", tone::text),
      delta (p.parameters, "delta", "Delta", "Hear only what is being taken out.", tone::gold),
      mix (p.parameters, "mix", "Mix", "How much treated sound you hear. 100% is fully treated.", Kind::footerRow, Unit::percent),
      outputGain (p.parameters, "outputGain", "Gain", "Final level, after everything else.", Kind::footerRow, Unit::signedDb)
{
    setOpaque (true);

    presets = std::make_unique<PresetBox> (p);
    reading = std::make_unique<PeakReading> (p);
    bandPicker = std::make_unique<BandPicker> (graph);
    advancedHeader = std::make_unique<AdvancedHeader> (e);

    addAndMakeVisible (graph);
    addAndMakeVisible (*presets);
    addAndMakeVisible (*reading);
    addAndMakeVisible (*bandPicker);
    addAndMakeVisible (*advancedHeader);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &depth, &sharpness, &selectivity, &attack, &release, &mode, &hardThreshold, &stereoMode,
                                                                        &link, &stereoFocus, &sharpnessTilt, &attackTilt, &releaseTilt, &wetTrim,
                                                                        &sideChain, &sideChainListen, &lowFreq, &highFreq, &bypass, &delta, &mix, &outputGain })
        addAndMakeVisible (c);

    for (int b = 0; b < smoothres::Params::numBands; ++b)
    {
        const auto number = juce::String (b + 1);
        bandFreq[b] = std::make_unique<ParamSlider> (p.parameters, "band" + number + "Freq", "Frequency", "Where this dot sits.", Kind::field, Unit::hz);
        bandSens[b] = std::make_unique<ParamSlider> (p.parameters, "band" + number + "Sens", "Sensitivity", "Up finds more here. Down protects this area.", Kind::field, Unit::signedDb);
        bandWidth[b] = std::make_unique<ParamSlider> (p.parameters, "band" + number + "Width", "Width", "How far this dot reaches on either side.", Kind::field, Unit::octaves);
        addChildComponent (*bandFreq[b]);
        addChildComponent (*bandSens[b]);
        addChildComponent (*bandWidth[b]);
    }

    hintLine.setInterceptsMouseClicks (false, false);
    hintLine.setJustificationType (juce::Justification::centredLeft);
    hintLine.setMinimumHorizontalScale (1.0f);
    hintLine.setFont (font (Weight::regular, 12.5f));
    hintLine.setColour (juce::Label::textColourId, tone::muted);
    hintLine.setBorderSize ({});
    addAndMakeVisible (hintLine);
    showHint ({});

    graph.onBandSelected = [this] (int b)
    {
        showBand (b);
        bandPicker->repaint();
    };

    graph.onHint = [this] (const juce::String& text) { showHint (text); };

    showBand (graph.getSelectedBand());

    // Hovering anything in the window updates the hint line.
    addMouseListener (this, true);
}

ResonanceEditor::Panel::~Panel()
{
    removeMouseListener (this);
}

void ResonanceEditor::Panel::showBand (int band)
{
    for (int b = 0; b < smoothres::Params::numBands; ++b)
    {
        bandFreq[b]->setVisible (b == band);
        bandSens[b]->setVisible (b == band);
        bandWidth[b]->setVisible (b == band);
    }
}

void ResonanceEditor::Panel::showHint (const juce::String& text)
{
    hintLine.setText (text.isEmpty() ? restingHint : text, juce::dontSendNotification);
}

void ResonanceEditor::Panel::hintFromComponent (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            showHint (hinted->hint);
            graph.setHighlight (c->getComponentID());
            return;
        }
    }

    if (c == this)
    {
        showHint ({});
        graph.setHighlight ({});
    }
}

void ResonanceEditor::Panel::mouseEnter (const juce::MouseEvent& e)
{
    if (e.eventComponent != &graph)
        hintFromComponent (e.eventComponent);
}

void ResonanceEditor::Panel::mouseExit (const juce::MouseEvent& e)
{
    if (e.eventComponent != &graph)
    {
        showHint ({});
        graph.setHighlight ({});
    }
}

int ResonanceEditor::Panel::countAdvancedChanges() const
{
    int count = 0;

    for (const auto* id : { "hardThreshold", "stereoMode", "link", "stereoFocus", "sharpnessTilt", "attackTilt", "releaseTilt", "wetTrim", "sideChain", "sideChainListen" })
    {
        const auto* p = processor.parameters.getParameter (id);

        if (std::abs (p->getValue() - p->getDefaultValue()) > 0.002f)
            ++count;
    }

    return count;
}

void ResonanceEditor::Panel::layoutSide()
{
    const int width = designWidth - sideLeft - margin;
    const float a = advancedAmount;
    const auto blend = [a] (float closed, float open) { return juce::roundToInt (closed + (open - closed) * a); };

    // Closed, the top of Depth lines up with the top of the graph. Open, Depth gives up its room.
    int y = blend ((float) graphTop, 14.0f);
    const int ring = blend (176.0f, 116.0f);
    depth.setBounds (sideLeft + (width - ring) / 2, y, ring, ring);
    y += ring + blend (36.0f, 12.0f);

    const int dialHeight = 46, dialGap = blend (24.0f, 12.0f), columnWidth = (width - 12) / 2;

    const auto rowOfDials = [&] (juce::Component& left, juce::Component& right)
    {
        left.setBounds (sideLeft, y, columnWidth, dialHeight);
        right.setBounds (sideLeft + columnWidth + 12, y, columnWidth, dialHeight);
        y += dialHeight + dialGap;
    };

    rowOfDials (sharpness, selectivity);
    rowOfDials (attack, release);
    y += blend (8.0f, 2.0f);

    constexpr int rowHeight = 28;
    const auto place = [&] (juce::Component& c)
    {
        c.setBounds (sideLeft, y, width, rowHeight);
        y += rowHeight;
    };

    place (mode);

    // Threshold belongs to Hard mode, so it is out with Mode whenever it does something, and in Advanced the rest of the time.
    const bool hard = mode.getSelected() == 1;
    const float thresholdAmount = hard ? 1.0f : a;
    hardThreshold.setBounds (sideLeft, y, width, rowHeight);
    hardThreshold.setAlpha (thresholdAmount);
    hardThreshold.setVisible (thresholdAmount > 0.01f);
    y += juce::roundToInt ((float) (rowHeight + 2) * thresholdAmount);

    y += blend (10.0f, 2.0f);
    advancedHeader->setBounds (sideLeft, y, width, rowHeight);
    y += rowHeight + 2;

    // The rest fades in below as the space opens up.
    const auto placeAdvanced = [&] (juce::Component& c)
    {
        c.setBounds (sideLeft, y, width, rowHeight);
        c.setAlpha (a);
        c.setVisible (a > 0.01f);
        y += rowHeight;
    };

    placeAdvanced (stereoMode);
    placeAdvanced (link);
    placeAdvanced (stereoFocus);
    placeAdvanced (sharpnessTilt);
    placeAdvanced (attackTilt);
    placeAdvanced (releaseTilt);
    placeAdvanced (wetTrim);

    const int half = (width - labelColumn - 6) / 2;
    sideChain.setBounds (sideLeft + labelColumn, y + 2, half, rowHeight - 2);
    sideChainListen.setBounds (sideLeft + labelColumn + half + 6, y + 2, half, rowHeight - 2);
    sideChain.setAlpha (a);
    sideChainListen.setAlpha (a);
    sideChain.setVisible (a > 0.01f);
    sideChainListen.setVisible (a > 0.01f);

    repaint (sideLeft, y, labelColumn, rowHeight + 4);
}

void ResonanceEditor::Panel::resized()
{
    constexpr int height = designHeight;
    const int footerTop = height - footerHeight;
    const int stripTop = footerTop - stripHeight;
    const int graphBottom = stripTop - axisHeight;

    graph.setBounds (margin, graphTop, graphRight - margin, graphBottom - graphTop);

    // Top bar.
    presets->setBounds (178, 16, 232, 32);
    reading->setBounds (graphRight - 340, 10, 340, graphTop - 18);

    // Strip below the graph, in the order things sit on the graph: Low, the dot, High.
    {
        auto strip = juce::Rectangle<int> (margin, stripTop, graphRight - margin, stripHeight);
        lowFreq.setBounds (strip.removeFromLeft (96));
        highFreq.setBounds (strip.removeFromRight (96));
        highFreq.setRightAligned (true);
        strip.removeFromLeft (40);
        bandPicker->setBounds (strip.removeFromLeft (96));
        strip.removeFromLeft (24);

        const auto fields = strip;
        const int each = fields.getWidth() / 3;

        for (int b = 0; b < smoothres::Params::numBands; ++b)
        {
            auto row = fields;
            bandFreq[b]->setBounds (row.removeFromLeft (each));
            bandSens[b]->setBounds (row.removeFromLeft (each));
            bandWidth[b]->setBounds (row);
        }
    }

    layoutSide();

    // Footer.
    {
        auto foot = juce::Rectangle<int> (margin, footerTop, graphRight - margin, footerHeight - 12);
        auto buttons = foot.removeFromLeft (150).withSizeKeepingCentre (150, 30);
        bypass.setBounds (buttons.removeFromLeft (72));
        buttons.removeFromLeft (6);
        delta.setBounds (buttons);

        foot.removeFromLeft (40);
        const int each = (foot.getWidth() - 40) / 2;
        mix.setBounds (foot.removeFromLeft (each).withSizeKeepingCentre (each, 36));
        foot.removeFromLeft (40);
        outputGain.setBounds (foot.withSizeKeepingCentre (foot.getWidth(), 36));

        hintLine.setBounds (sideLeft, footerTop, designWidth - sideLeft - margin - 8, footerHeight - 12);
    }
}

void ResonanceEditor::Panel::paint (juce::Graphics& g)
{
    // Flat, and only drawn when the layout changes. Nothing here runs per frame.
    g.fillAll (tone::base);

    g.setColour (tone::text);
    g.setFont (font (Weight::medium, 24.0f));
    g.drawText ("Resonance", juce::Rectangle<float> ((float) margin, 0.0f, 150.0f, (float) graphTop), juce::Justification::centredLeft, false);

    // Axes live outside the plot, so they cost nothing per frame.
    g.setFont (font (Weight::regular, 12.0f));
    g.setColour (tone::faint);

    const float axisY = (float) graph.getBottom() + 3.0f;

    const struct { float hz; const char* text; } ticks[] = { { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" },
                                                             { 1000.0f, "1k" }, { 2000.0f, "2k" }, { 5000.0f, "5k" }, { 10000.0f, "10k" } };

    for (const auto& t : ticks)
        g.drawText (t.text, juce::Rectangle<float> ((float) graph.getX() + graph.xForHz (t.hz) - 24.0f, axisY, 48.0f, 16.0f), juce::Justification::centred, false);

    const float scaleX = (float) graph.getRight() + 6.0f;

    for (int db = 6; db <= 24; db += 6)
        g.drawText (juce::String (db), juce::Rectangle<float> (scaleX, (float) graph.getY() + graph.yForReductionLine ((float) db) - 8.0f, 24.0f, 16.0f), juce::Justification::centredLeft, false);

    g.drawText ("dB", juce::Rectangle<float> (scaleX, (float) graph.getY() + 2.0f, 24.0f, 16.0f), juce::Justification::centredLeft, false);

    if (advancedAmount > 0.01f)
    {
        g.setColour (tone::muted.withMultipliedAlpha (advancedAmount));
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText ("Side chain", juce::Rectangle<float> ((float) sideLeft, (float) sideChain.getY(), (float) labelColumn, (float) sideChain.getHeight()), juce::Justification::centredLeft, false);
    }
}

// ---- the editor ------------------------------------------------------------------------------------------------

ResonanceEditor::ResonanceEditor (SmoothResonanceProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p),
      panel ((setLookAndFeel (&lookAndFeel), *this), p)
{
    addAndMakeVisible (panel);

    using Proc = SmoothResonanceProcessor;
    const double ratio = (double) designWidth / (double) designHeight;
    setResizable (true, true);
    setResizeLimits (Proc::minWindowWidth, juce::roundToInt (Proc::minWindowWidth / ratio), Proc::maxWindowWidth, juce::roundToInt (Proc::maxWindowWidth / ratio));
    getConstrainer()->setFixedAspectRatio (ratio);

    panel.advancedAmount = p.advancedOpen.load (std::memory_order_relaxed) ? 1.0f : 0.0f;

    const int savedWidth = juce::jlimit (Proc::minWindowWidth, Proc::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    constructed = true;

    panel.reading->advance (0.0);
    tick (1.0 / 60.0);

    // At most one frame per screen refresh, and never over the cap on faster screens. When nothing moves, nothing repaints.
    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
    {
        const double elapsed = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / 60.0;
        lastFrameTime = timestampSeconds;
        sinceDrawn += elapsed;

        // Spend whole refreshes against a running budget, so that the average lands on the cap at any screen rate, instead
        // of falling to 30 frames on a 60 Hz screen.
        frameBudget = juce::jmin (frameBudget + elapsed, 0.05);

        if (frameBudget + 0.001 < 1.0 / maxFramesPerSecond)
            return;

        frameBudget -= 1.0 / maxFramesPerSecond;
        const double step = sinceDrawn;
        sinceDrawn = 0.0;
        tick (step);
    });
}

ResonanceEditor::~ResonanceEditor()
{
    vblank.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void ResonanceEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::base);
}

void ResonanceEditor::resized()
{
    panel.setBounds (0, 0, designWidth, designHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

bool ResonanceEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void ResonanceEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
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

void ResonanceEditor::tick (double dtSeconds)
{
    if (panel.graph.advance (dtSeconds))
        panel.graph.repaint();

    panel.reading->advance (dtSeconds);
    panel.presets->refresh();

    // Advanced glides open and shut, in about a fifth of a second.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;

    if (panel.advancedAmount != target)
    {
        const float next = panel.advancedAmount + (target - panel.advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        panel.advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel.layoutSide();
        panel.repaint (sideLeft, 0, designWidth - sideLeft, designHeight - footerHeight);
    }

    // Threshold only does something in Hard mode, and comes out next to Mode when it does.
    const bool hard = panel.mode.getSelected() == 1;

    if (panel.hardThreshold.isEnabled() != hard)
    {
        panel.hardThreshold.setEnabled (hard);
        panel.layoutSide();
        panel.repaint (sideLeft, 0, designWidth - sideLeft, designHeight - footerHeight);
    }

    if (--changesCountdown <= 0)
    {
        changesCountdown = 12;
        panel.advancedHeader->setState (panel.advancedAmount, panel.countAdvancedChanges());
    }
    else if (panel.advancedAmount != target)
    {
        panel.advancedHeader->setState (panel.advancedAmount, panel.countAdvancedChanges());
    }
}

void ResonanceEditor::previewHover (const juce::String& paramId)
{
    panel.graph.setHighlight (paramId);

    if (auto* c = panel.findChildWithID (paramId))
    {
        if (auto* slider = dynamic_cast<ParamSlider*> (c))
            slider->setForcedHover (true);
        else if (auto* toggle = dynamic_cast<ParamToggle*> (c))
            toggle->setForcedHover (true);
        else if (auto* choice = dynamic_cast<ParamChoice*> (c))
            choice->setForcedHover (true);

        if (auto* hinted = dynamic_cast<Hinted*> (c))
            panel.showHint (hinted->hint);
    }
}
