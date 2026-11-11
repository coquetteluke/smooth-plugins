#include "PunchEditor.h"

using namespace ui;
using namespace punchview;

namespace
{
    constexpr int margin = 22;
    constexpr int largeHeight = 84, regularHeight = 44, footerHeight = 42, stripHeight = 44;
    constexpr double fastRate = 30.0, slowRate = 8.0;
    constexpr double quietAfter = 0.3, meterFallPerSecond = 40.0;

    const juce::String restingHint = "Double-click anything to reset it. Click a value to type one.";
    const juce::String minus = juce::String::fromUTF8 ("\xe2\x88\x92");

    juce::String signedDb (float v, const char* unit = " dB")
    {
        const auto digits = juce::String (std::abs (v), 1);
        const bool zero = ! digits.containsAnyOf ("123456789");
        return (zero ? juce::String() : (v > 0.0f ? juce::String ("+") : minus)) + digits + unit;
    }

    juce::String percent (float v)  { return juce::String (juce::roundToInt (v)) + " %"; }
}

// ---- the Advanced row -------------------------------------------------------------------------------------------

class PunchEditor::AdvancedHeader final : public juce::Component,
                                          public Hinted
{
public:
    explicit AdvancedHeader (PunchEditor& o) : owner (o)
    {
        setComponentID ("advanced");
        setTitle ("Advanced");
        hint = "Gain in front of everything, and Mix with the dry sound.";
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
    PunchEditor& owner;
    juce::String summary;
};

// ---- the editor -------------------------------------------------------------------------------------------------

PunchEditor::PunchEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p),
      hitView (p),
      presets (p),
      bypass (p.apvts, "bypass", "Bypass", "Takes everything out. The delay stays the same, so nothing shifts in time."),
      attack (p.apvts, "attack", "Attack", "Boosts or cuts the first moments of each sound.", BarSlider::Size::large, [] (float v) { return signedDb (v); }),
      sustain (p.apvts, "sustain", "Sustain", "Boosts or cuts the tail of each sound as it dies away.", BarSlider::Size::large, [] (float v) { return signedDb (v); }),
      punch (p.apvts, "punch", "Punch", "Tightens each sound and keeps the overall level the same.", BarSlider::Size::regular, percent),
      impact (p.apvts, "impact", "Impact", "Evens out the performance and raises it. Use with the Ceiling on.", BarSlider::Size::regular, percent),
      ceiling (p.apvts, "ceiling", "Ceiling", "The most the output can reach while On is lit.", BarSlider::Size::regular, [] (float v) { return minusText (v, 1) + " dB"; }),
      ceilingOn (p.apvts, "ceilingOn", "On", "Turns the Ceiling on or off."),
      steadyNotes (p.apvts, "steadyNotes", "Steady", "For a voice: Sustain and Punch leave held notes alone."),
      gain (p.apvts, "gain", "Gain", "Level in front of everything, for quiet takes. It drives Impact harder.", BarSlider::Size::regular, [] (float v) { return signedDb (v); }),
      mix (p.apvts, "mix", "Mix", "Blends the effect with the untouched sound.", BarSlider::Size::regular, percent)
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (false);
    setOpaque (true);

    attack.setEndWords ("Softer start", "Harder start");
    sustain.setEndWords ("Shorter tail", "Longer tail");

    presets.nameNow = [&p]
    {
        const auto name = p.getProgramName (p.getCurrentProgram());
        return p.isCurrentProgramEdited() ? name + ", edited" : name;
    };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &timeline, &hitView, &meters, &readouts, &presets, &bypass, &attack, &sustain, &punch, &impact, &ceiling, &ceilingOn, &steadyNotes })
        addAndMakeVisible (c);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &gain, &mix })
        addChildComponent (c);

    advancedHeader = std::make_unique<AdvancedHeader> (*this);
    addAndMakeVisible (*advancedHeader);

    hintText = restingHint;
    addMouseListener (this, true);

    lastBin = p.envelopeTap.getWritten();
    lastSettings = readSettings();

    setResizable (true, true);
    const bool open = isAdvancedOpen();
    using P = PluginProcessor;
    const int extra = open ? drawerHeight : 0;
    setResizeLimits (P::minWindowWidth, P::minWindowHeight + extra, P::maxWindowWidth, P::maxWindowHeight);
    setSize (juce::jlimit (P::minWindowWidth, P::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed)),
             juce::jlimit (P::minWindowHeight + extra, P::maxWindowHeight, p.windowHeight.load (std::memory_order_relaxed)));
    constructed = true;
    resized();

    updateAdvancedSummary();
    presets.refresh();
    tick (1.0 / fastRate);
    startTimerHz ((int) slowRate);
}

PunchEditor::~PunchEditor()
{
    stopTimer();
    removeMouseListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void PunchEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::black);

    if (g.clipRegionIntersects (titleArea))
    {
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 26.0f));
        g.drawText ("Punch", titleArea, juce::Justification::centredLeft, false);
    }

    if (g.clipRegionIntersects (hintArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (hintText, hintArea, juce::Justification::centredRight, true);
    }
}

void PunchEditor::resized()
{
    if (! constructed)
        return;

    processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
    processorRef.windowHeight.store (getHeight(), std::memory_order_relaxed);

    auto r = getLocalBounds().withTrimmedLeft (margin).withTrimmedRight (margin);

    // Top bar.
    auto bar = r.removeFromTop (60).withTrimmedTop (14).withTrimmedBottom (12);
    titleArea = bar.removeFromLeft (92);
    presets.setBounds (bar.removeFromLeft (300)); // wide enough for the longest preset name with ", edited" after it
    bypass.setBounds (bar.removeFromRight (84));

    // The timeline runs the whole width.
    timeline.setBounds (r.removeFromTop (stripHeight));
    r.removeFromTop (12);

    // Bottom up: the fixed rows. The hit takes whatever is left.
    auto foot = r.removeFromBottom (footerHeight).withTrimmedBottom (10).withTrimmedTop (6);
    advancedHeader->setBounds (foot.removeFromLeft (foot.getWidth() * 2 / 5));
    hintArea = foot;

    const bool open = isAdvancedOpen();
    gain.setVisible (open);
    mix.setVisible (open);

    juce::Rectangle<int> drawer;
    if (open)
    {
        drawer = r.removeFromBottom (drawerHeight - 12);
        r.removeFromBottom (12);
    }

    auto regularRow = r.removeFromBottom (regularHeight);
    r.removeFromBottom (14);
    auto largeRow = r.removeFromBottom (largeHeight);
    r.removeFromBottom (4);

    // The display, then its columns set the columns under it.
    const int rightColumn = MeterPanel::preferredWidth;
    auto heroRow = r;
    auto right = heroRow.removeFromRight (rightColumn);
    hitView.setBounds (heroRow);

    const auto plot = hitView.getPlot().translated (heroRow.getX(), heroRow.getY());
    meters.setBounds (right.getX(), plot.getY(), rightColumn, plot.getHeight());
    // The numbers under In and Out are wider than their lanes, so their row starts a little further left.
    constexpr int readoutOverhang = 12;
    auto lanes = meters.getLanes();
    for (auto& lane : lanes)
        lane.translate (readoutOverhang, 0);
    readouts.setLanes (lanes);

    const int split = heroRow.getX() + hitView.getSplitX();
    const int gapBetween = 28;
    attack.setBounds (largeRow.withRight (split - gapBetween / 2));
    {
        // Steady Notes belongs to Sustain, so it sits at the end of Sustain's bar.
        auto cell = largeRow.withLeft (split + gapBetween / 2).withRight (plot.getRight());
        steadyNotes.setBounds (cell.removeFromRight (64).withTrimmedTop (1).withHeight (26));
        cell.removeFromRight (12);
        sustain.setBounds (cell);
    }
    readouts.setBounds (right.getX() - readoutOverhang, largeRow.getY() + 2, rightColumn + readoutOverhang, 54);

    punch.setBounds (regularRow.withRight (split - gapBetween / 2));
    impact.setBounds (regularRow.withLeft (split + gapBetween / 2).withRight (plot.getRight()));
    {
        auto cell = regularRow.withLeft (right.getX());
        ceilingOn.setBounds (cell.removeFromRight (42).withTrimmedTop (1).withHeight (26));
        cell.removeFromRight (12);
        ceiling.setBounds (cell);
    }

    if (open)
    {
        gain.setBounds (drawer.withRight (split - gapBetween / 2));
        mix.setBounds (drawer.withLeft (split + gapBetween / 2).withRight (plot.getRight()));
    }

    repaint();
}

bool PunchEditor::isAdvancedOpen() const noexcept
{
    return processorRef.advancedOpen.load (std::memory_order_relaxed);
}

void PunchEditor::setAdvancedOpen (bool shouldBeOpen)
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

void PunchEditor::updateAdvancedSummary()
{
    // Hidden settings still change the sound, so a closed Advanced says which ones have moved.
    juce::StringArray moved;
    if (std::abs (gain.getValue()) > 1.0e-3)
        moved.add ("Gain " + gain.valueText());
    if (std::abs (mix.getValue() - 100.0) > 1.0e-3)
        moved.add ("Mix " + mix.valueText());
    advancedHeader->setSummary (moved.joinIntoString (", "));
}

Settings PunchEditor::readSettings() const
{
    const auto value = [this] (const char* id) { return processorRef.apvts.getRawParameterValue (id)->load (std::memory_order_relaxed); };
    Settings s;
    s.punch = value ("punch");
    s.attack = value ("attack");
    s.sustain = value ("sustain");
    s.mix = value ("mix");
    s.gain = value ("gain");
    s.impact = value ("impact");
    s.ceiling = value ("ceiling");
    s.ceilingOn = value ("ceilingOn") > 0.5f;
    s.steadyNotes = value ("steadyNotes") > 0.5f;
    s.bypass = value ("bypass") > 0.5f;
    return s;
}

// ---- frames -----------------------------------------------------------------------------------------------------

bool PunchEditor::tick (double seconds)
{
    auto& tap = processorRef.envelopeTap;
    const auto written = tap.getWritten();
    auto available = written - lastBin;

    // After a long gap, only the newest bins are still certain to be there.
    if (available > (std::uint32_t) EnvelopeTap::numBins - 512u)
    {
        lastBin = written - 2048u;
        available = 2048u;
    }

    bool sound = false;

    for (; lastBin != written; ++lastBin)
    {
        const float before = tap.before (lastBin), after = tap.after (lastBin);
        sound = sound || before > 1.0e-4f || after > 1.0e-4f;

        if (hitView.feed (before, after))
            timeline.markHit();

        timeline.feed (before, after);
    }

    sinceBins = available > 0 ? 0.0 : sinceBins + seconds;
    sinceSound = sound ? 0.0 : sinceSound + seconds;

    const auto settings = readSettings();
    if (settings != lastSettings)
    {
        lastSettings = settings;
        sinceChange = 0.0;
    }
    else
    {
        sinceChange += seconds;
    }

    bool moving = hitView.update (settings, sinceSound > quietAfter);
    moving = timeline.update() || moving;

    // The readings stop being updated when the host stops calling, so here they fall on their own.
    const bool running = sinceBins < 0.25;
    const float fall = (float) (meterFallPerSecond * seconds);
    const auto follow = [running, fall] (float& shown, float wanted, float rest)
    {
        if (running)
            shown = wanted;
        else
            shown = rest < shown ? juce::jmax (rest, shown - fall) : juce::jmin (rest, shown + fall);
    };
    follow (shownIn, processorRef.inputLevel->getDb(), -60.0f);
    follow (shownOut, processorRef.outputLevel->getDb(), -60.0f);
    follow (shownImpact, -processorRef.impactReduction->getDb(), 0.0f);
    follow (shownCeiling, -processorRef.ceilingReduction->getDb(), 0.0f);

    meters.setCeiling (settings.ceiling, settings.ceilingOn);
    moving = meters.setLevels (shownIn, shownOut, shownImpact, settings.ceilingOn ? shownCeiling : 0.0f) || moving;

    // Numbers do not need to change as fast as bars, and drawing text costs more.
    sinceReadout += seconds;
    if (sinceReadout >= 0.12)
    {
        sinceReadout = 0.0;
        readouts.setValues (shownIn, shownOut, shownImpact, shownCeiling, settings.ceilingOn);
    }

    ceiling.setDimmed (! settings.ceilingOn);

    if (--slowCounter <= 0)
    {
        slowCounter = fast ? 8 : 2;
        presets.refresh();
        updateAdvancedSummary();
    }

    return moving || sinceChange < 0.5;
}

void PunchEditor::timerCallback()
{
    if (! isShowing())
        return;

    const double seconds = getTimerInterval() * 0.001;
    const bool moving = tick (seconds);

    // When nothing moves, nothing repaints, and the timer only looks in a few times a second to see whether sound has started.
    if (moving != fast)
    {
        fast = moving;
        startTimerHz ((int) (fast ? fastRate : slowRate));
    }
}

// ---- the hint line ----------------------------------------------------------------------------------------------

void PunchEditor::setHint (const juce::String& text)
{
    if (text != hintText)
    {
        hintText = text;
        repaint (hintArea);
    }
}

void PunchEditor::showHintFor (juce::Component* c)
{
    // A control lights up the part of the hit it works on.
    hitView.setZone (c == &attack ? HitView::Zone::start : (c == &sustain ? HitView::Zone::tail : HitView::Zone::none));

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

void PunchEditor::mouseEnter (const juce::MouseEvent& e)  { showHintFor (e.eventComponent); }
void PunchEditor::mouseMove (const juce::MouseEvent& e)   { showHintFor (e.eventComponent); }
void PunchEditor::mouseExit (const juce::MouseEvent&)
{
    hitView.setZone (HitView::Zone::none);
    setHint (restingHint);
}

void PunchEditor::previewHint (const juce::String& paramId)
{
    if (auto* c = findChildWithID (paramId))
    {
        showHintFor (c);
        if (auto* s = dynamic_cast<BarSlider*> (c))
            s->setForcedHover (true);
    }
}
