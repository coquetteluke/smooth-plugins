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

    // How many display frames a second, at most. Every frame costs about 7.5 million instructions, nearly half of it AppKit and
    // CoreAnimation overhead that no drawing change can remove, so this is the number that sets the window's CPU. 30 is an
    // even cadence on a 60 or 120 Hz screen, where 45 would skip every fourth refresh. See docs/window-cpu.md.
    constexpr double maxFramesPerSecond = 30.0;

    const juce::String restingHint = "Drag the dots. Double-click the graph to add one, or anything else to reset it.";
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
        setOpaque (true);   // so a new number does not repaint the panel behind it
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
            refreshCountdown = 0.15f;

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
        g.fillAll (tone::base);

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
        hint = "Pick a dot. A faint number is a free dot: click it to add it.";
        setTitle ("Selected dot");
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText ("Dot", juce::Rectangle<float> (0.0f, 6.0f, 60.0f, 18.0f), juce::Justification::centredLeft, false);

        for (int b = 0; b < smoothres::Params::numBands; ++b)
        {
            const auto c = cellFor (b);
            const bool on = graph.isBandOn (b);
            const bool chosen = on && graph.getSelectedBand() == b;
            const bool lit = hovered == b;

            if (on)
            {
                g.setColour (chosen ? tone::text : tone::text.withAlpha (lit ? 0.2f : 0.11f));
                g.fillEllipse (c);
                g.setColour (chosen ? tone::well : (lit ? tone::text : tone::text.withAlpha (0.85f)));
            }
            else
            {
                // Free: an outline only, which turns into a plus when it would add the dot.
                g.setColour (tone::text.withAlpha (lit ? 0.45f : 0.16f));
                g.drawEllipse (c.reduced (0.5f), 1.0f);
                g.setColour (lit ? tone::text : tone::faint.withAlpha (0.8f));
            }

            g.setFont (font (Weight::medium, 12.0f));
            g.drawText (! on && lit ? juce::String ("+") : juce::String (b + 1), c, juce::Justification::centred, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (cellAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override   { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = cellAt (e.position);

        if (hit >= 0)
        {
            if (graph.isBandOn (hit))
                graph.setSelectedBand (hit);
            else
                graph.addBand (hit);

            repaint();
        }
    }

private:
    juce::Rectangle<float> cellFor (int b) const
    {
        return { (float) b * 24.0f, 27.0f, 20.0f, 20.0f };
    }

    int cellAt (juce::Point<float> p) const
    {
        for (int b = 0; b < smoothres::Params::numBands; ++b)
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

// ---- the two small buttons that belong to the selected dot -------------------------------------------------------

class ResonanceEditor::SmallButton final : public juce::Component,
                                           public Hinted
{
public:
    SmallButton (const juce::String& textToShow, const juce::String& id, const juce::String& hintText, juce::Colour onColourToUse)
        : text (textToShow), onColour (onColourToUse)
    {
        hint = hintText;
        setComponentID (id);
        setTitle (textToShow);
        setRepaintsOnMouseActivity (true);
    }

    std::function<void()> onClick;

    void setOn (bool shouldBeOn)
    {
        if (on != shouldBeOn)
        {
            on = shouldBeOn;
            repaint();
        }
    }

    bool isOn() const noexcept  { return on; }

    void setText (const juce::String& newText)
    {
        if (text != newText)
        {
            text = newText;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const bool lit = isEnabled() && isMouseOverOrDragging();
        g.setColour (on ? onColour : tone::text.withAlpha (lit ? 0.11f : 0.06f));
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour ((on ? tone::well : (lit ? tone::text : tone::muted)).withMultipliedAlpha (isEnabled() ? 1.0f : 0.4f));
        g.setFont (font (Weight::medium, 12.0f));
        g.drawText (text, b, juce::Justification::centred, false);
    }

    void enablementChanged() override  { repaint(); }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (isEnabled() && onClick != nullptr)
            onClick();
    }

private:
    juce::String text;
    const juce::Colour onColour;
    bool on = false;
};

// ---- how much each channel is cut, in surround ----------------------------------------------------------------

class ResonanceEditor::ChannelMeters final : public juce::Component
{
public:
    explicit ChannelMeters (SmoothResonanceProcessor& p) : processor (p)
    {
        setInterceptsMouseClicks (false, false);
        setOpaque (true);
    }

    void refreshLayout()
    {
        names = processor.getChannelNames();
        lfe = processor.getLfeChannel();
        layout = processor.getLayoutName();
        shown.fill (0.0f);
        repaint();
    }

    int heightWanted() const noexcept
    {
        return 22 + rowHeight * (names.size() > perRow ? 2 : 1);
    }

    void update (const smoothres::GraphFrame& frame, double dt)
    {
        bool moved = false;

        for (int ch = 0; ch < names.size(); ++ch)
        {
            const float target = ch < frame.numChannels ? frame.channelReductionDb[(size_t) ch] : 0.0f;
            auto& now = shown[(size_t) ch];
            const float next = target > now ? target : now + (target - now) * (1.0f - std::exp (-(float) dt / 0.2f));
            moved = moved || std::abs (next - drawn[(size_t) ch]) > 0.25f;
            now = next;
        }

        if (moved)
        {
            drawn = shown;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::base);
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText ("Channels", juce::Rectangle<float> (0.0f, 0.0f, 84.0f, 18.0f), juce::Justification::centredLeft, false);
        g.setColour (tone::faint);
        g.drawText (lfe >= 0 ? "LFE only delayed" : "All treated", juce::Rectangle<float> (84.0f, 0.0f, (float) getWidth() - 84.0f, 18.0f), juce::Justification::centredRight, false);

        const float cell = (float) getWidth() / (float) perRow;

        for (int ch = 0; ch < names.size(); ++ch)
        {
            const float x = cell * (float) (ch % perRow);
            const float y = 22.0f + (float) (rowHeight * (ch / perRow));
            const auto bar = juce::Rectangle<float> (x, y, cell - 5.0f, 4.0f);

            g.setColour (tone::text.withAlpha (0.08f));
            g.fillRoundedRectangle (bar, 2.0f);

            if (ch != lfe)
            {
                g.setColour (tone::gold);
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, drawn[(size_t) ch] / 24.0f)), 2.0f);
            }

            g.setColour (ch == lfe ? tone::faint : tone::muted);
            g.setFont (font (Weight::regular, 11.0f));
            g.drawText (names[ch], juce::Rectangle<float> (x, y + 6.0f, cell - 2.0f, 14.0f), juce::Justification::centredLeft, false);
        }
    }

private:
    static constexpr int perRow = 8, rowHeight = 23;

    SmoothResonanceProcessor& processor;
    juce::StringArray names;
    juce::String layout;
    int lfe = -1;
    std::array<float, smoothres::maxEngineChannels> shown {}, drawn {};
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
      depth (p.parameters, "depth", "Depth", "How hard it works. Also the most any spot is cut, unless Max Cut is lower.", Kind::bigDial, Unit::db, tone::gold),
      maxCut (p.parameters, "maxCut", "Max Cut", "No spot goes deeper than this. With high Depth, more of the area reaches it.", Kind::row, Unit::db, tone::gold),
      sharpness (p.parameters, "sharpness", "Sharpness", "Low treats wide areas. High treats narrow spots.", Kind::dial, Unit::percent),
      selectivity (p.parameters, "selectivity", "Selectivity", "Low catches small bumps too. High catches only the worst.", Kind::dial, Unit::percent),
      attack (p.parameters, "attack", "Attack", "How fast it grabs a harsh peak.", Kind::dial, Unit::ms),
      release (p.parameters, "release", "Release", "How fast it lets go.", Kind::dial, Unit::ms),
      mode (p.parameters, "mode", "Mode", "Soft compares each spot to its neighbours. Hard uses a fixed level."),
      quality (p.parameters, "quality", "Quality", "Low Latency 11 ms, for recording. Normal 43 ms. High 85 ms, finest detail, slower to react."),
      heldNotes (p.parameters, "heldNotes", "Held notes", "Also takes down one harmonic that rings on a held note, shown in rose on the graph. Soft mode only.", tone::text),
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
    channelMeters = std::make_unique<ChannelMeters> (p);
    listenButton = std::make_unique<SmallButton> ("Listen", "bandListen", "Hear only this dot's area. With Delta, only what it loses there. This changes the output.", tone::listen);
    listenButton->onClick = [this] { graph.toggleListen (graph.getSelectedBand()); };

    addAndMakeVisible (graph);
    addAndMakeVisible (*presets);
    addAndMakeVisible (*reading);
    addAndMakeVisible (*bandPicker);
    addAndMakeVisible (*advancedHeader);
    addChildComponent (*channelMeters);
    addAndMakeVisible (*listenButton);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &depth, &maxCut, &sharpness, &selectivity, &attack, &release, &mode, &quality, &heldNotes, &hardThreshold, &stereoMode,
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

        // One for each dot, since each is the home of that dot's real On parameter.
        removeButton[b] = std::make_unique<SmallButton> ("Remove", "band" + number + "On", "Take this dot away.", tone::text);
        removeButton[b]->onClick = [this, b]
        {
            if (graph.isBandOn (b))
                graph.removeBand (b);
            else
                graph.addBand (b);
        };
        addChildComponent (*removeButton[b]);
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
        refreshDots();
        bandPicker->repaint();
    };

    graph.onHint = [this] (const juce::String& text) { showHint (text); };

    showBand (graph.getSelectedBand());
    refreshDots();
    refreshChannels();

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
        removeButton[b]->setVisible (b == band);
    }
}

void ResonanceEditor::Panel::refreshDots()
{
    // Automation can turn dots on and off, and move the listening, so this runs on every display frame. Only a change costs anything.
    juce::uint32 mask = (juce::uint32) graph.getSelectedBand() << 8;

    for (int b = 0; b < smoothres::Params::numBands; ++b)
        if (graph.isBandOn (b))
            mask |= 1u << b;

    const int listening = graph.getListeningBand();

    if (mask == dotsShown && listening == listenShown)
        return;

    dotsShown = mask;
    listenShown = listening;

    const int selected = graph.getSelectedBand();
    const bool selectedOn = graph.isBandOn (selected);
    bandFreq[selected]->setEnabled (selectedOn);
    bandSens[selected]->setEnabled (selectedOn);
    bandWidth[selected]->setEnabled (selectedOn);
    listenButton->setEnabled (selectedOn || listening >= 0);
    removeButton[selected]->setText (selectedOn ? "Remove" : "Add");
    removeButton[selected]->hint = selectedOn ? "Take this dot away." : "Put this dot back.";
    listenButton->setOn (listening >= 0);
    bandPicker->repaint();
}

void ResonanceEditor::Panel::refreshChannels()
{
    const int channels = processor.getMainBusNumOutputChannels();

    if (channels == channelsShown)
        return;

    channelsShown = channels;

    // Mid and side only mean something on two channels.
    const bool surround = channels > 2;
    channelMeters->refreshLayout();
    channelMeters->setVisible (surround);
    stereoMode.setEnabled (channels == 2);
    stereoMode.hint = channels == 2 ? juce::String ("Stereo treats left and right. Mid/Side treats center and sides.")
                                    : juce::String ("Mid/Side only works on stereo. Here every channel is treated on its own, joined by Link.");
    link.hint = surround ? juce::String ("At 100% all channels move together, except the LFE. Lower treats them apart.")
                         : juce::String ("At 100% both sides move together. Lower treats them apart.");
    stereoFocus.hint = surround ? juce::String ("Moves the work toward the left or right side channels. Center channels stay as they are.")
                                : juce::String ("Moves the work toward one side. Left of center is left or mid.");
    layoutSide();
    repaint (sideLeft, 0, designWidth - sideLeft, designHeight - footerHeight);
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
    const int ring = blend (176.0f, 110.0f);
    depth.setBounds (sideLeft + (width - ring) / 2, y, ring, ring);
    y += ring + blend (10.0f, 2.0f);

    // The ceiling sits right under the control it limits.
    maxCut.setBounds (sideLeft, y, width, 28);
    y += 28 + blend (18.0f, 6.0f);

    const int dialHeight = 46, dialGap = blend (22.0f, 8.0f), columnWidth = (width - 12) / 2;

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

    place (quality);
    y += 2;

    // Held notes belongs to Soft mode, so it sits on Mode's row, at its end.
    constexpr int heldWidth = 86;
    mode.setBounds (sideLeft, y, width - heldWidth - 6, rowHeight);
    heldNotes.setBounds (sideLeft + width - heldWidth, y + 1, heldWidth, rowHeight - 2);
    y += rowHeight;

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

    // In surround, the channels come last, under whatever else is showing.
    if (channelMeters->isVisible())
    {
        const int top = juce::jmax (advancedHeader->getBottom() + 8, juce::roundToInt ((float) (advancedHeader->getBottom() + 8) + (float) (sideChain.getBottom() + 6 - advancedHeader->getBottom() - 8) * a));
        channelMeters->setBounds (sideLeft, top, width, channelMeters->heightWanted());
    }
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
        strip.removeFromLeft (10);
        bandPicker->setBounds (strip.removeFromLeft (192));
        strip.removeFromLeft (22);

        // Listen and Remove belong to the selected dot, so they sit straight after its values.
        auto buttons = strip.removeFromRight (64);
        strip.removeFromRight (16);
        buttons = buttons.withSizeKeepingCentre (64, 50);
        listenButton->setBounds (buttons.removeFromTop (24));
        buttons.removeFromTop (2);

        for (auto& button : removeButton)
            button->setBounds (buttons);

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
    g.drawText ("Velvet Rope", juce::Rectangle<float> ((float) margin, 0.0f, 150.0f, (float) graphTop), juce::Justification::centredLeft, false);

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
    setOpaque (true);   // saves clearing the window under every frame

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
    // The panel covers everything but the sliver rounding can leave at some window sizes. JUCE does not skip this paint for
    // a child with a transform, so filling the whole window here would run on every frame.
    g.setColour (tone::base);
    g.fillRect (getWidth() - 2, 0, 2, getHeight());
    g.fillRect (0, getHeight() - 2, getWidth(), 2);
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
        panel.graph.flushRepaint();

    panel.reading->advance (dtSeconds);
    panel.presets->refresh();
    panel.refreshDots();
    panel.refreshChannels();

    if (panel.channelMeters->isVisible())
        panel.channelMeters->update (panel.graph.getFrame(), dtSeconds);

    // Advanced glides open and shut, in about a fifth of a second.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;

    if (panel.advancedAmount != target)
    {
        const float next = panel.advancedAmount + (target - panel.advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        panel.advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel.layoutSide();
        panel.repaint (sideLeft, 0, designWidth - sideLeft, designHeight - footerHeight);
    }

    // Threshold only does something in Hard mode, and comes out next to Mode when it does. Held notes only does something
    // in Soft mode, and goes grey in Hard.
    const bool hard = panel.mode.getSelected() == 1;

    if (panel.hardThreshold.isEnabled() != hard)
    {
        panel.hardThreshold.setEnabled (hard);
        panel.layoutSide();
        panel.repaint (sideLeft, 0, designWidth - sideLeft, designHeight - footerHeight);
    }

    if (panel.heldNotes.isEnabled() == hard)
    {
        panel.heldNotes.setEnabled (! hard);
        panel.heldNotes.setAlpha (hard ? 0.4f : 1.0f);
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
