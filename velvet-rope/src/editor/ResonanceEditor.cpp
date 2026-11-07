#include "ResonanceEditor.h"

using namespace ui;

namespace
{
    constexpr int graphWidth = 848;       // the side panel fills the rest
    constexpr int topBarHeight = 64;
    constexpr int stripHeight = 66;
    constexpr int footerHeight = 72;
    constexpr int sideMargin = 20;

    const juce::String restingHint = "Drag the dots on the graph. Double-click anything to reset it. Click a value to type one.";
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
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.26f));
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (tone::bone.withAlpha (zone != none ? 0.5f : 0.22f));
        g.drawRoundedRectangle (b, 6.0f, 1.0f);

        if (zone != none)
        {
            g.setColour (tone::bone.withAlpha (0.08f));
            const auto area = zone == previous ? b.withWidth (arrowWidth) : (zone == next ? b.withTrimmedLeft (b.getWidth() - arrowWidth) : b.reduced (arrowWidth, 0.0f));
            g.fillRoundedRectangle (area, 6.0f);
        }

        g.setColour (tone::bone);
        g.setFont (font (Weight::semibold, 19.0f));
        g.drawText (processor.getProgramName (processor.getCurrentProgram()), b.reduced (arrowWidth, 0.0f), juce::Justification::centred, true);

        const auto chevron = [&] (float cx, float direction)
        {
            juce::Path p;
            p.startNewSubPath (cx - 3.0f * direction, b.getCentreY() - 6.0f);
            p.lineTo (cx + 3.0f * direction, b.getCentreY());
            p.lineTo (cx - 3.0f * direction, b.getCentreY() + 6.0f);
            g.setColour (tone::boneSoft);
            g.strokePath (p, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        chevron (b.getX() + arrowWidth * 0.5f, -1.0f);
        chevron (b.getRight() - arrowWidth * 0.5f, 1.0f);
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

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (30),
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

        auto amount = b.removeFromRight (120.0f);
        g.setColour (active && ! bypassed ? tone::gold : tone::boneSoft);
        g.setFont (font (Weight::semibold, 34.0f));
        g.drawText (amountText, amount, juce::Justification::centredRight, false);

        b.removeFromRight (14.0f);
        g.setColour (tone::boneSoft);
        g.setFont (font (Weight::regular, 17.0f));
        g.drawText (bypassed ? "Would take out" : "Taking out", b.withTrimmedBottom (b.getHeight() * 0.5f - 1.0f), juce::Justification::bottomRight, false);
        g.setColour (active ? tone::bone : tone::boneSoft);
        g.drawText (whereText, b.withTrimmedTop (b.getHeight() * 0.5f + 1.0f), juce::Justification::topRight, false);
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
        hint = "Pick a dot. The three values next to this belong to it.";
        setTitle ("Selected band");
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (tone::boneSoft);
        g.setFont (font (Weight::regular, 16.5f));
        g.drawText ("Dot", juce::Rectangle<float> (4.0f, 6.0f, 60.0f, 20.0f), juce::Justification::centredLeft, false);

        for (int b = 0; b < 3; ++b)
        {
            const auto c = cellFor (b);
            const bool on = graph.getSelectedBand() == b;
            g.setColour (on ? tone::bone : juce::Colours::black.withAlpha (hovered == b ? 0.1f : 0.26f));
            g.fillEllipse (c);
            g.setColour (tone::bone.withAlpha (on || hovered == b ? 1.0f : 0.5f));
            g.drawEllipse (c, 1.4f);
            g.setColour (on ? tone::well : tone::bone);
            g.setFont (font (Weight::semibold, 16.5f));
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
        return { 4.0f + (float) b * 31.0f, 27.0f, 26.0f, 26.0f };
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
      sideChain (p.parameters, "sideChain", "Use", "Listens to another track to decide what to turn down.", tone::bone),
      sideChainListen (p.parameters, "sideChainListen", "Listen", "Plays the side chain input, to check it.", tone::bone),
      lowFreq (p.parameters, "lowFreq", "Low", "Nothing below this is touched.", Kind::field, Unit::hz),
      highFreq (p.parameters, "highFreq", "High", "Nothing above this is touched.", Kind::field, Unit::hz),
      bypass (p.parameters, "bypass", "Bypass", "Hear the untouched sound. The track stays in time.", tone::bone),
      delta (p.parameters, "delta", "Delta", "Hear only what is being taken out.", tone::gold),
      mix (p.parameters, "mix", "Mix", "How much treated sound you hear. 100% is fully treated.", Kind::footerRow, Unit::percent),
      outputGain (p.parameters, "outputGain", "Gain", "Final level, after everything else.", Kind::footerRow, Unit::signedDb)
{
    setOpaque (true);

    presets = std::make_unique<PresetBox> (p);
    reading = std::make_unique<PeakReading> (p);
    bandPicker = std::make_unique<BandPicker> (graph);

    addAndMakeVisible (graph);
    addAndMakeVisible (*presets);
    addAndMakeVisible (*reading);
    addAndMakeVisible (*bandPicker);

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
    hintLine.setFont (font (Weight::regular, 17.0f));
    hintLine.setColour (juce::Label::textColourId, tone::boneSoft);
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

void ResonanceEditor::Panel::resized()
{
    constexpr int width = designWidth, height = designHeight;
    const int graphBottom = height - footerHeight - stripHeight;

    graph.setBounds (0, topBarHeight, graphWidth, graphBottom - topBarHeight);

    // Top bar.
    presets->setBounds (196, 13, 268, 38);
    reading->setBounds (graphWidth - 340 - 18, 8, 340, topBarHeight - 16);

    // Strip below the graph, in the order things sit on the graph: Low, the dot, High.
    {
        auto strip = juce::Rectangle<int> (12, graphBottom, graphWidth - 24, stripHeight);
        lowFreq.setBounds (strip.removeFromLeft (118));
        highFreq.setBounds (strip.removeFromRight (118));
        strip.removeFromLeft (30);
        bandPicker->setBounds (strip.removeFromLeft (108).withTrimmedBottom (2));
        strip.removeFromLeft (16);

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

    // Side panel.
    {
        auto side = juce::Rectangle<int> (graphWidth + sideMargin, 0, width - graphWidth - sideMargin * 2, height - footerHeight);

        side.removeFromTop (12);
        auto depthLabel = side.removeFromTop (22);
        juce::ignoreUnused (depthLabel);
        depth.setBounds (side.removeFromTop (134).withSizeKeepingCentre (134, 134));
        side.removeFromTop (12);

        const int dialHeight = 54, dialGap = 10;
        auto rowOfDials = [&] (juce::Component& left, juce::Component& right)
        {
            auto row = side.removeFromTop (dialHeight);
            left.setBounds (row.removeFromLeft (row.getWidth() / 2 - 2));
            row.removeFromLeft (4);
            right.setBounds (row);
            side.removeFromTop (dialGap);
        };

        rowOfDials (sharpness, selectivity);
        rowOfDials (attack, release);

        const int rowHeight = 28;
        auto next = [&] (juce::Component& c) { c.setBounds (side.removeFromTop (rowHeight)); };
        auto gap = [&] { side.removeFromTop (17); };

        side.removeFromTop (11);
        next (mode);
        next (hardThreshold);
        gap();
        next (stereoMode);
        next (link);
        next (stereoFocus);
        gap();
        next (sharpnessTilt);
        next (attackTilt);
        next (releaseTilt);
        gap();
        next (wetTrim);

        side.removeFromTop (4);
        auto sc = side.removeFromTop (rowHeight);
        sc.removeFromLeft (96);
        const int half = (sc.getWidth() - 6) / 2;
        sideChain.setBounds (sc.removeFromLeft (half));
        sc.removeFromLeft (6);
        sideChainListen.setBounds (sc);
    }

    // Footer.
    {
        auto foot = juce::Rectangle<int> (24, height - footerHeight, graphWidth - 24 - 18, footerHeight);
        auto buttons = foot.removeFromLeft (182).withSizeKeepingCentre (182, 38);
        bypass.setBounds (buttons.removeFromLeft (88));
        buttons.removeFromLeft (6);
        delta.setBounds (buttons);

        foot.removeFromLeft (32);
        const int each = (foot.getWidth() - 32) / 2;
        mix.setBounds (foot.removeFromLeft (each).withSizeKeepingCentre (each, 40));
        foot.removeFromLeft (32);
        outputGain.setBounds (foot.withSizeKeepingCentre (foot.getWidth(), 40));

        hintLine.setBounds (graphWidth + sideMargin, height - footerHeight + 8, width - graphWidth - sideMargin - 28, footerHeight - 16);
    }
}

void ResonanceEditor::Panel::paint (juce::Graphics& g)
{
    constexpr float width = designWidth, height = designHeight;

    g.drawImage (assets->glaze, getLocalBounds().toFloat());
    g.setColour (tone::gold.withAlpha (0.28f));
    g.fillPath (assets->flecks);

    const float stripTop = height - footerHeight - stripHeight;
    const float footerTop = height - footerHeight;

    // Pressed into the glaze: a dark line and a light one.
    const auto ridgeH = [&] (float y, float x1, float x2)
    {
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRect (juce::Rectangle<float> (x1, y - 1.0f, x2 - x1, 1.0f));
        g.setColour (tone::bone.withAlpha (0.10f));
        g.fillRect (juce::Rectangle<float> (x1, y, x2 - x1, 1.0f));
    };

    ridgeH (footerTop, 0.0f, width);

    g.setColour (juce::Colours::black.withAlpha (0.38f));
    g.fillRect (juce::Rectangle<float> ((float) graphWidth, 0.0f, 1.0f, footerTop));
    g.setColour (tone::bone.withAlpha (0.10f));
    g.fillRect (juce::Rectangle<float> ((float) graphWidth + 1.0f, 0.0f, 1.0f, footerTop));

    // The strip's inner dividers, so Low and High read apart from the dot's values.
    g.setColour (tone::bone.withAlpha (0.12f));
    g.fillRect (juce::Rectangle<float> ((float) lowFreq.getRight() + 12.0f, stripTop + 14.0f, 1.0f, (float) stripHeight - 28.0f));
    g.fillRect (juce::Rectangle<float> ((float) highFreq.getX() - 12.0f, stripTop + 14.0f, 1.0f, (float) stripHeight - 28.0f));

    // Side panel groups.
    g.setColour (tone::bone.withAlpha (0.12f));

    for (auto* c : { (juce::Component*) &mode, (juce::Component*) &stereoMode, (juce::Component*) &sharpnessTilt, (juce::Component*) &wetTrim })
        g.fillRect (juce::Rectangle<float> ((float) (graphWidth + sideMargin), (float) c->getY() - 9.0f, (float) (designWidth - graphWidth - sideMargin * 2), 1.0f));

    // The two side chain switches share one label.
    g.setColour (tone::boneSoft);
    g.setFont (font (Weight::regular, 16.5f));
    g.drawText ("Side chain", juce::Rectangle<float> ((float) (graphWidth + sideMargin), (float) sideChain.getY(), 96.0f, (float) sideChain.getHeight()), juce::Justification::centredLeft, false);

    // Name.
    g.setColour (tone::bone);
    g.setFont (font (Weight::regular, 40.0f));
    g.drawText ("Resonance", juce::Rectangle<float> (24.0f, 0.0f, 170.0f, (float) topBarHeight), juce::Justification::centredLeft, false);

    // Depth's label sits over the dial.
    g.setColour (tone::boneSoft);
    g.setFont (font (Weight::regular, 19.0f));
    g.drawText ("Depth", juce::Rectangle<float> ((float) graphWidth, 8.0f, (float) (designWidth - graphWidth), 24.0f), juce::Justification::centred, false);
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

    const int savedWidth = juce::jlimit (Proc::minWindowWidth, Proc::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    constructed = true;

    panel.reading->advance (0.0);
    tick (1.0 / 60.0);

    // One repaint per screen refresh at most, capped near 60 a second on faster screens. When nothing moves, nothing repaints.
    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
    {
        if (timestampSeconds - lastFrameTime < 1.0 / 70.0)
            return;

        const double dt = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / 60.0;
        lastFrameTime = timestampSeconds;
        tick (dt);
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
    g.fillAll (tone::lapisDark);
}

void ResonanceEditor::resized()
{
    panel.setBounds (0, 0, designWidth, designHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

void ResonanceEditor::tick (double dtSeconds)
{
    if (panel.graph.advance (dtSeconds))
        panel.graph.repaint();

    panel.reading->advance (dtSeconds);
    panel.presets->refresh();

    // Hard threshold only does something in Hard mode.
    const bool hard = panel.mode.getSelected() == 1;

    if (panel.hardThreshold.isEnabled() != hard)
        panel.hardThreshold.setEnabled (hard);
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
