#include "SliceEditor.h"

using namespace ui;

namespace
{
    constexpr int margin = 20;
    constexpr double fastRate = 30.0, slowRate = 15.0;
    const char* const fileTypes = "wav;aif;aiff;flac;ogg;mp3;m4a;caf";
    const juce::String restingHint = "Click a value to type one. Double-click anything to reset it.";

    juce::String minusSign() { return juce::String::fromUTF8 ("\xe2\x88\x92"); }

    juce::String plusMinus (float v, int places, const char* unit)
    {
        const float rounded = std::round (v * std::pow (10.0f, (float) places)) / std::pow (10.0f, (float) places);
        const juce::String sign = rounded > 0.0f ? juce::String ("+") : (rounded < 0.0f ? minusSign() : juce::String());
        return sign + juce::String (std::abs (rounded), places) + unit;
    }

    juce::String timeText (float ms)
    {
        if (ms >= 1000.0f) return juce::String (ms * 0.001f, 2) + " s";
        if (ms <= 0.0f)    return "0 ms";
        if (ms < 10.0f)    return juce::String (ms, 1) + " ms";
        return juce::String (juce::roundToInt (ms)) + " ms";
    }

    juce::String percentText (float v)
    {
        return juce::String (v, std::abs (v - std::round (v)) < 0.05f ? 0 : 1) + "%";
    }

    juce::String semitoneText (float v)
    {
        const bool whole = std::abs (v - std::round (v)) < 0.005f;
        return plusMinus (v, whole ? 0 : 2, " st");
    }

    // Understands 250, 250 ms and 1.5 s. Parameters here are all in milliseconds.
    float parseTime (const juce::String& text, bool& understood)
    {
        understood = text.containsAnyOf ("0123456789");
        const auto lower = text.toLowerCase().trim();
        const float number = lower.getFloatValue();
        return lower.endsWith ("s") && ! lower.endsWith ("ms") ? number * 1000.0f : number;
    }

    // Understands -40, 40 L, L40, 40 R, R40 and C.
    float parsePan (const juce::String& text, bool& understood)
    {
        const auto lower = text.toLowerCase().trim();
        if (lower == "c" || lower == "center" || lower == "centre")
        {
            understood = true;
            return 0.0f;
        }
        understood = lower.containsAnyOf ("0123456789");
        const float number = std::abs (lower.retainCharacters ("0123456789.").getFloatValue());
        if (lower.contains ("l") || lower.startsWith ("-"))
            return -number;
        return number;
    }

    juce::String panText (float v)
    {
        if (std::abs (v) < 0.05f) return "Center";
        return juce::String (std::abs (v), std::abs (std::abs (v) - std::round (std::abs (v))) < 0.05f ? 0 : 1) + (v < 0.0f ? "% L" : "% R");
    }
}

// ---- which slice ---------------------------------------------------------------------------------------------------

class SliceEditor::SliceStepper final : public juce::Component,
                                        public Hinted,
                                        private juce::Timer
{
public:
    explicit SliceStepper (juce::AudioProcessorValueTreeState& state)
        : param (*state.getParameter ("selected")),
          attachment (param, [this] (float v) { value = juce::roundToInt (v); repaint(); }, nullptr)
    {
        setComponentID ("selected");
        setTitle (param.getName (64));
        hint = "Which slice these controls edit. All 32 are here, even ones not cut yet. Click the number to type one.";
        attachment.sendInitialUpdate();

        box.setFont (font (Weight::medium, 20.0f, true));
        box.setJustificationType (juce::Justification::centred);
        box.setEditable (false, false, true);
        box.onTextChange = [this]
        {
            const auto text = box.getText().trim();
            if (text.containsOnly ("0123456789") && text.isNotEmpty())
                attachment.setValueAsCompleteGesture ((float) juce::jlimit (1, 32, text.getIntValue()));
        };
        box.onEditorHide = [this] { box.setVisible (false); };
        addChildComponent (box);
    }

    ~SliceStepper() override { stopTimer(); }

    int getValue() const noexcept { return value; }
    void step (int by) { attachment.setValueAsCompleteGesture ((float) juce::jlimit (1, 32, value + by)); }

    void setDetail (const juce::String& noteName, const juce::String& line)
    {
        if (noteName != note || line != detail)
        {
            note = noteName;
            detail = line;
            repaint();
        }
    }

    void setUsed (bool isUsed)
    {
        if (isUsed != used)
        {
            used = isUsed;
            repaint();
        }
    }

    juce::Rectangle<float> chip() const  { return juce::Rectangle<float> (0.0f, 0.0f, 46.0f, 46.0f); }
    juce::Rectangle<float> previousZone() const  { return { 52.0f, 24.0f, 26.0f, 22.0f }; }
    juce::Rectangle<float> nextZone() const      { return { 78.0f, 24.0f, 26.0f, 22.0f }; }

    void paint (juce::Graphics& g) override
    {
        const auto colour = sliceColour (value - 1);
        const auto c = chip();

        if (used)
        {
            g.setColour (colour);
            g.fillRoundedRectangle (c, 9.0f);
        }
        else
        {
            g.setColour (colour.withAlpha (0.8f));
            g.drawRoundedRectangle (c.reduced (0.75f), 8.25f, 1.5f);
        }

        if (! box.isVisible())
        {
            g.setColour (used ? tone::ink : colour);
            g.setFont (font (Weight::medium, 22.0f, true));
            g.drawText (juce::String (value), c, juce::Justification::centred, false);
        }

        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 15.0f));
        g.drawText ("Slice " + juce::String (value), juce::Rectangle<float> (56.0f, 0.0f, 70.0f, 22.0f), juce::Justification::centredLeft, false);

        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 12.5f, true));
        g.drawText (note, juce::Rectangle<float> (112.0f, 26.0f, (float) getWidth() - 112.0f, 18.0f), juce::Justification::centredLeft, true);
        g.drawText (detail, juce::Rectangle<float> (122.0f, 0.0f, (float) getWidth() - 122.0f, 22.0f), juce::Justification::centredRight, true);

        const auto arrow = [&] (juce::Rectangle<float> zone, int direction, bool enabled, bool over)
        {
            if (over && enabled)
            {
                g.setColour (tone::hover);
                g.fillRoundedRectangle (zone.reduced (1.0f), 4.0f);
            }
            else if (enabled)
            {
                g.setColour (tone::raised);
                g.fillRoundedRectangle (zone.reduced (1.0f), 4.0f);
            }
            drawChevron (g, zone.getCentre(), direction, enabled ? (over ? tone::text : tone::muted) : tone::faint.withAlpha (0.5f));
        };
        const auto mouse = getMouseXYRelative().toFloat();
        arrow (previousZone(), 2, value > 1, isMouseOver (true) && previousZone().contains (mouse));
        arrow (nextZone(), 1, value < 32, isMouseOver (true) && nextZone().contains (mouse));
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    void mouseMove (const juce::MouseEvent&) override  { repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (previousZone().contains (e.position))
            step (-1);
        else if (nextZone().contains (e.position))
            step (1);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (chip().contains (e.position) && e.getNumberOfClicks() == 1 && ! e.mouseWasDraggedSinceMouseDown())
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        stopTimer();
        if (chip().contains (e.position))
            attachment.setValueAsCompleteGesture (1.0f);
    }

private:
    void timerCallback() override
    {
        stopTimer();
        box.setBounds (chip().toNearestInt());
        box.setText (juce::String (value), juce::dontSendNotification);
        box.setVisible (true);
        box.showEditor();
    }

    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    juce::Label box;
    int value = 1;
    bool used = false;
    juce::String note, detail;
};

// ---- the Advanced row ---------------------------------------------------------------------------------------------

class SliceEditor::AdvancedHeader final : public juce::Component,
                                          public Hinted
{
public:
    explicit AdvancedHeader (SliceEditor& o) : owner (o)
    {
        setComponentID ("advanced");
        setTitle ("Advanced");
        hint = "Min Slice, Transpose, Bend Range and what the plug-in is reading right now.";
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
        drawChevron (g, { mark.getCentreX() - 2.0f, mark.getCentreY() }, open ? 0 : 1, lit ? tone::text : tone::muted);

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText ("Advanced", b.removeFromLeft (66.0f), juce::Justification::centredLeft, false);

        if (summary.isNotEmpty() && ! open)
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (summary, b.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    void mouseDown (const juce::MouseEvent&) override  { owner.setAdvancedOpen (! owner.isAdvancedOpen()); }

private:
    SliceEditor& owner;
    juce::String summary;
};

// ---- the editor ---------------------------------------------------------------------------------------------------

SliceEditor::SliceEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processor (p),
      waveform (p),
      keyMap (p, waveform),
      ampGraph (p.apvts),
      pitchGraph (p.apvts),
      presets (p),
      loadButton ("Load sample", "Pick a vocal file. It loads in the background, so notes keep playing.", true),
      zoomButton ("Zoom slice", "Fills the waveform with the selected slice, to place its markers closely.", false),
      bypass (p.apvts, "bypass", "Bypass", "Turns the sound off, with a short fade."),
      sliceMode (p.apvts, "slice_mode", "How to cut", "Transient cuts where each sound starts. Grid cuts evenly. Manual keeps your own markers."),
      gridType (p.apvts, "grid_type", "Grid", "Cut into a number of equal parts, or by note length at a tempo."),
      keyMode (p.apvts, "key_mode", "Keys", "Slices puts each slice on its own key. Chromatic plays the selected slice across the keyboard."),
      grid (p.apvts, "grid", "Parts", "How many equal slices.", " slices"),
      gridNote (p.apvts, "grid_note", "Note length", "How long each slice is, as a note at the tempo here."),
      playMode (p.apvts, "play_mode", "Play mode", "Gate plays while held. One Shot plays it out. Loop repeats. To End runs on. Boomerang goes there and back."),
      sensitivity (p.apvts, "sensitivity", "Sensitivity", "How soft a sound can be and still get a cut. Higher makes more slices.", percentText),
      sliceStart (p.apvts, "slice_start", "Start", "Nothing before this point gets cut. You can also drag the Start tab on the waveform.", timeText, parseTime),
      gridTempo (p.apvts, "grid_tempo", "Tempo", "The tempo for note lengths. Set by hand, so a bounce never depends on the session.",
                 [] (float v) { return juce::String (v, std::abs (v - std::round (v)) < 0.005f ? 0 : 2) + " BPM"; }),
      output (p.apvts, "output", "Output", "The level of everything coming out.", [] (float v) { return plusMinus (v, 1, " dB"); }),
      root (p.apvts, "root_note", "Root", "The key slice 1 sits on. You can also drag the Root tab on the keyboard.",
            [] (float v) { return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, true, 3); }),
      polyphony (p.apvts, "polyphony", "Voices", "How many notes can sound at once. Past that, the oldest fades out.", [] (float v) { return juce::String (juce::roundToInt (v)); }),
      velSens (p.apvts, "vel_sens", "Velocity", "How much playing softer makes it quieter. 0% ignores how hard you play.", percentText),
      loopFade (p.apvts, "loop_fade", "Crossfade", "In Loop mode, how much the end of the loop blends into its start.", timeText, parseTime),
      decay (p.apvts, "decay", "Decay", "After the Attack, how long it takes to fall to the Sustain level. All slices.", timeText, parseTime),
      sustain (p.apvts, "sustain", "Sustain", "The level it holds at. All slices. 100% skips the Decay, and 0% ends the note after it.", percentText),
      penvAmount (p.apvts, "penv_amount", "Amount", "Each note starts this many semitones off pitch and glides back.", semitoneText),
      penvTime (p.apvts, "penv_time", "Time", "How long the glide back to pitch takes.", timeText, parseTime),
      minSlice (p.apvts, "min_slice", "Min Slice", "Two cuts never come closer than this. The stronger one stays.", timeText, parseTime),
      transpose (p.apvts, "transpose", "Transpose", "Moves every slice up or down in semitones.", semitoneText),
      bendRange (p.apvts, "bend_range", "Bend Range", "How far the pitch wheel bends, in semitones.", [] (float v) { return juce::String (juce::roundToInt (v)) + " st"; }),
      slotPitch (p.apvts, PluginProcessor::slotId (0, "pitch"), "Pitch", "Plays this slice higher or lower. It speeds up or slows down, like tape.", semitoneText),
      slotLevel (p.apvts, PluginProcessor::slotId (0, "level"), "Level", "How loud this slice is. The waveform above grows and shrinks with it.", [] (float v) { return plusMinus (v, 1, " dB"); }),
      slotPan (p.apvts, PluginProcessor::slotId (0, "pan"), "Pan", "Where this slice sits, left to right.", panText, parsePan),
      slotAttack (p.apvts, PluginProcessor::slotId (0, "attack"), "Attack", "How long this slice takes to rise.", timeText, parseTime),
      slotRelease (p.apvts, PluginProcessor::slotId (0, "release"), "Release", "How long this slice takes to fade after the key goes up.", timeText, parseTime),
      slotReverse (p.apvts, PluginProcessor::slotId (0, "reverse"), "Reverse", "Plays this slice backwards. The waveform above flips to match."),
      slicesFound ("slices_found", "Slices Found", "How many slices the sample is cut into right now."),
      sampleLength ("sample_length", "Sample Length", "How long the loaded sample is."),
      activeVoices ("active_voices", "Active Voices", "How many notes are sounding right now.")
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (false);
    setOpaque (true);

    stepper = std::make_unique<SliceStepper> (p.apvts);
    advancedHeader = std::make_unique<AdvancedHeader> (*this);

    loadButton.setComponentID ("load");
    loadButton.onClick = [this] { chooseFile(); };
    zoomButton.setComponentID ("zoom");
    zoomButton.onClick = [this] { setZoomed (! isZoomed()); };
    waveform.onEmptyClick = [this] { chooseFile(); };

    pitchGraph.setAccent (tone::text);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &waveform, &keyMap, &ampGraph, &pitchGraph, &presets, &loadButton, &zoomButton, &bypass, &sliceMode, &keyMode, &playMode, &output, &root, &polyphony, &velSens,
             &loopFade, &decay, &sustain, &penvAmount, &penvTime, &slotPitch, &slotLevel, &slotPan, &slotAttack, &slotRelease, &slotReverse, stepper.get(), advancedHeader.get() })
        addAndMakeVisible (c);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &sensitivity, &sliceStart, &gridType, &grid, &gridNote, &gridTempo, &minSlice, &transpose, &bendRange, &slicesFound, &sampleLength, &activeVoices })
        addChildComponent (c);

    slotReverse.toFront (false);   // it sits in the stepper's row

    hintText = restingHint;
    addMouseListener (this, true);

    setResizable (true, true);
    using P = PluginProcessor;
    setResizeLimits (P::minWindowWidth, P::minWindowHeight, P::maxWindowWidth, P::maxWindowHeight);
    setSize (juce::jlimit (P::minWindowWidth, P::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed)),
             juce::jlimit (P::minWindowHeight, P::maxWindowHeight, p.windowHeight.load (std::memory_order_relaxed)));

    constructed = true;
    shownMode = (int) p.apvts.getRawParameterValue ("slice_mode")->load();
    refresh();
    startTimerHz ((int) slowRate);
}

SliceEditor::~SliceEditor()
{
    stopTimer();
    removeMouseListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

void SliceEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::base);

    if (g.clipRegionIntersects (titleArea))
    {
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 22.0f));
        g.drawText (getWindowTitle(), titleArea, juce::Justification::centredLeft, false);
    }

    if (g.clipRegionIntersects (infoArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f, true));
        g.drawFittedText (statusText, infoArea, juce::Justification::centredLeft, 2, 1.0f);
    }

    if (g.clipRegionIntersects (hintArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 12.5f));
        g.drawText (hintText, hintArea, juce::Justification::centredRight, true);
    }

    if (! manualNoteArea.isEmpty() && g.clipRegionIntersects (manualNoteArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawFittedText ("Your own markers. Drag one to move it, double-click to add one, Option-click to remove one.",
                          manualNoteArea, juce::Justification::centredLeft, 2, 1.0f);
    }

    g.setColour (tone::text);
    g.setFont (font (Weight::medium, 13.0f));
    if (g.clipRegionIntersects (ampLabel))
        g.drawText ("Amp", ampLabel, juce::Justification::centredLeft, false);
    if (g.clipRegionIntersects (pitchLabel))
        g.drawText ("Pitch glide", pitchLabel, juce::Justification::centredLeft, false);
}

void SliceEditor::resized()
{
    if (! constructed)
        return;

    processor.windowWidth.store (getWidth(), std::memory_order_relaxed);
    processor.windowHeight.store (getHeight(), std::memory_order_relaxed);

    auto r = getLocalBounds();

    // Top bar.
    auto bar = r.removeFromTop (52).reduced (margin, 0).withTrimmedTop (11).withTrimmedBottom (5);
    titleArea = bar.removeFromLeft (70);
    presets.setBounds (bar.removeFromLeft (212));
    bar.removeFromLeft (10);
    loadButton.setBounds (bar.removeFromLeft (96));
    bar.removeFromLeft (14);
    bypass.setBounds (bar.removeFromRight (72));
    bar.removeFromRight (20);
    output.setBounds (bar.removeFromRight (104).withY (bar.getY() - 7).withHeight (46));
    bar.removeFromRight (16);
    infoArea = bar;

    // Slicing tools.
    auto tools = r.removeFromTop (58).reduced (margin, 0).withTrimmedTop (6).withTrimmedBottom (6);
    sliceMode.setBounds (tools.removeFromLeft (232));
    tools.removeFromLeft (20);
    zoomButton.setBounds (tools.removeFromRight (90).withTrimmedTop (18));
    tools.removeFromRight (20);

    const int mode = (int) processor.apvts.getRawParameterValue ("slice_mode")->load();
    const int gridKind = (int) processor.apvts.getRawParameterValue ("grid_type")->load();
    manualNoteArea = {};

    if (mode == PluginProcessor::modeTransient)
    {
        const int w = juce::jmin (260, (tools.getWidth() - 20) / 2);
        sensitivity.setBounds (tools.removeFromLeft (w));
        tools.removeFromLeft (20);
        sliceStart.setBounds (tools.removeFromLeft (w));
    }
    else if (mode == PluginProcessor::modeGrid)
    {
        gridType.setBounds (tools.removeFromLeft (200));
        tools.removeFromLeft (14);
        if (gridKind == PluginProcessor::gridEqual)
        {
            grid.setBounds (tools.removeFromLeft (112));
            tools.removeFromLeft (14);
        }
        else
        {
            gridNote.setBounds (tools.removeFromLeft (118));
            tools.removeFromLeft (14);
            gridTempo.setBounds (tools.removeFromLeft (84));
            tools.removeFromLeft (14);
        }
        sliceStart.setBounds (tools.removeFromLeft (juce::jmin (200, tools.getWidth())));
    }
    else
    {
        manualNoteArea = tools.withTrimmedTop (10);
    }

    // Bottom up: the footer, Advanced, the control band. The hero takes what is left.
    auto foot = r.removeFromBottom (34).reduced (margin, 0).withTrimmedBottom (6);
    advancedHeader->setBounds (foot.removeFromLeft (300));
    hintArea = foot.withTrimmedRight (10);

    const bool open = isAdvancedOpen();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &minSlice, &transpose, &bendRange, &slicesFound, &sampleLength, &activeVoices })
        c->setVisible (open);

    if (open)
    {
        auto drawer = r.removeFromBottom (70).reduced (margin, 0).withTrimmedTop (14).withTrimmedBottom (10);
        const int gap = 22;
        const int w = (drawer.getWidth() - 5 * gap) / 6;
        for (juce::Component* c : std::initializer_list<juce::Component*> { &minSlice, &transpose, &bendRange, &slicesFound, &sampleLength, &activeVoices })
        {
            c->setBounds (drawer.removeFromLeft (w));
            drawer.removeFromLeft (gap);
        }
    }

    auto band = r.removeFromBottom (160).reduced (margin, 0).withTrimmedTop (14).withTrimmedBottom (4);

    const int gap = 28;
    const int total = band.getWidth() - 3 * gap;
    const int sliceWidth = total * 24 / 100, ampWidth = total * 29 / 100, pitchWidth = total * 17 / 100;

    // The selected slice. Its cells line up with the cells in the other groups, set below.
    const auto sliceColumn = band.removeFromLeft (sliceWidth);
    stepper->setBounds (sliceColumn.withHeight (46));
    slotReverse.setBounds (sliceColumn.getRight() - 72, sliceColumn.getY() + 22, 72, 24);
    band.removeFromLeft (gap);

    // Amp: Attack and Release follow the slice, Decay and Sustain are shared.
    {
        auto c = band.removeFromLeft (ampWidth);
        ampLabel = c.removeFromTop (18);
        auto cells = c.removeFromBottom (46);
        c.removeFromBottom (4);
        ampGraph.setBounds (c);
        const int cellGap = 8;
        const int w = (cells.getWidth() - 3 * cellGap) / 4;
        for (auto* s : { &slotAttack, &decay, &sustain, &slotRelease })
        {
            s->setBounds (cells.removeFromLeft (w));
            cells.removeFromLeft (cellGap);
        }
    }
    band.removeFromLeft (gap);

    {
        auto c = band.removeFromLeft (pitchWidth);
        pitchLabel = c.removeFromTop (18);
        auto cells = c.removeFromBottom (46);
        c.removeFromBottom (4);
        pitchGraph.setBounds (c);
        const int cellGap = 10;
        const int w = (cells.getWidth() - cellGap) / 2;
        penvAmount.setBounds (cells.removeFromLeft (w));
        cells.removeFromLeft (cellGap);
        penvTime.setBounds (cells);
    }
    band.removeFromLeft (gap);

    {
        auto c = band;
        auto top = c.removeFromTop (46);
        playMode.setBounds (top.removeFromLeft (juce::jmax (104, top.getWidth() - 12 - 156)));
        top.removeFromLeft (12);
        keyMode.setBounds (top);
        auto cells = c.removeFromBottom (46);
        const int cellGap = 10;
        const int cw = (cells.getWidth() - 3 * cellGap) / 4;
        for (auto* s : { &root, &polyphony, &velSens, &loopFade })
        {
            s->setBounds (cells.removeFromLeft (cw));
            cells.removeFromLeft (cellGap);
        }
    }

    {
        auto cells = sliceColumn.withTrimmedTop (sliceColumn.getHeight() - 46);
        const int cellGap = 12;
        const int w = (cells.getWidth() - 2 * cellGap) / 3;
        for (auto* c : { &slotPitch, &slotLevel, &slotPan })
        {
            c->setBounds (cells.removeFromLeft (w));
            cells.removeFromLeft (cellGap);
        }
    }

    // The hero: full width, edge to edge.
    auto map = r.removeFromBottom (KeyMap::ribbonHeight + KeyMap::keysHeight);
    keyMap.setBounds (map);
    waveform.setBounds (r);
}

bool SliceEditor::isAdvancedOpen() const noexcept
{
    return processor.advancedOpen.load (std::memory_order_relaxed);
}

void SliceEditor::setAdvancedOpen (bool shouldBeOpen)
{
    processor.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);
    resized();
    repaint();
}

void SliceEditor::setZoomed (bool shouldZoom)
{
    zoomButton.setLatched (shouldZoom);
    refresh();
}

void SliceEditor::bindSelected (int index)
{
    if (index == boundSlot || isDraggingSlotControl())
        return;

    boundSlot = index;
    slotPitch.bind (PluginProcessor::slotId (index, "pitch"));
    slotLevel.bind (PluginProcessor::slotId (index, "level"));
    slotPan.bind (PluginProcessor::slotId (index, "pan"));
    slotAttack.bind (PluginProcessor::slotId (index, "attack"));
    slotRelease.bind (PluginProcessor::slotId (index, "release"));
    slotReverse.bind (PluginProcessor::slotId (index, "reverse"));
    ampGraph.bindSlot (index);

    const auto colour = sliceColour (index);
    for (auto* s : { &slotPitch, &slotLevel, &slotPan, &slotAttack, &slotRelease })
        s->setAccent (colour);
    slotReverse.setAccent (colour);
    ampGraph.setAccent (colour);
}

bool SliceEditor::isDraggingSlotControl() const
{
    for (auto* s : { &slotPitch, &slotLevel, &slotPan, &slotAttack, &slotRelease })
        if (s->isBeingDragged())
            return true;
    return ampGraph.isDraggingHandle();
}

void SliceEditor::updateToolbar()
{
    const int mode = (int) processor.apvts.getRawParameterValue ("slice_mode")->load();
    const bool transient = mode == PluginProcessor::modeTransient, onGrid = mode == PluginProcessor::modeGrid;
    const bool equal = (int) processor.apvts.getRawParameterValue ("grid_type")->load() == PluginProcessor::gridEqual;

    sensitivity.setVisible (transient);
    sliceStart.setVisible (transient || onGrid);
    gridType.setVisible (onGrid);
    grid.setVisible (onGrid && equal);
    gridNote.setVisible (onGrid && ! equal);
    gridTempo.setVisible (onGrid && ! equal);
}

void SliceEditor::updateInfo()
{
    auto s = processor.getSample();
    const bool hasSample = s != nullptr && ! s->isEmpty();
    juce::String text = processor.getStatus();

    if (text.isEmpty())
    {
        if (hasSample)
            text << s->name << ", " << juce::String (s->frames() / s->rate, 2) << " s, " << juce::String (s->rate / 1000.0, 1) << " kHz, "
                 << (s->audio.getNumChannels() > 1 ? "stereo" : "mono") << ", " << shownTable.count << " slices";
        else
            text = "No sample loaded. It stays silent until one is.";
    }

    if (text != statusText)
    {
        statusText = text;
        repaint (infoArea);
    }

    const int selected = juce::jlimit (0, PluginProcessor::kSlots - 1, processor.getSelectedIndex());
    const bool used = hasSample && selected < shownTable.count;
    juce::String detail;
    if (used)
        detail = juce::String ((double) (shownTable.start[selected + 1] - shownTable.start[selected]) / s->rate, 2) + " s";
    else if (hasSample)
        detail = "Not cut yet";
    stepper->setUsed (used);
    stepper->setDetail (juce::MidiMessage::getMidiNoteName (processor.noteForSlice (selected), true, true, 3), detail);

    // The three readings: what a host sees, as a host sees it.
    for (auto* p : processor.getParameters())
        if (auto* reading = dynamic_cast<ReadingParameter*> (p))
        {
            const auto value = reading->getCurrentValueAsText();
            const auto id = reading->paramID;
            if (id == "slices_found")        slicesFound.setValueText (value);
            else if (id == "sample_length")  sampleLength.setValueText (value + " s");
            else if (id == "active_voices")  activeVoices.setValueText (value);
        }

    juce::StringArray changed;
    const auto note = [&] (ValueCell& cell, const char* id, const juce::String& name)
    {
        auto* prm = processor.apvts.getParameter (id);
        if (std::abs (prm->getValue() - prm->getDefaultValue()) > 1.0e-4f)
            changed.add (name + " " + cell.valueText());
    };
    note (minSlice, "min_slice", "Min Slice");
    note (transpose, "transpose", "Transpose");
    note (bendRange, "bend_range", "Bend Range");
    advancedHeader->setSummary (changed.joinIntoString (", "));

    presets.refresh();
}

void SliceEditor::pollFast()
{
    const int actualMode = (int) processor.apvts.getRawParameterValue ("slice_mode")->load();

    // Picking Manual before any edit starts from the markers that were on show.
    if (actualMode == PluginProcessor::modeManual && shownMode != PluginProcessor::modeManual && ! processor.isManualEdited())
        processor.adoptAsManual (shownTable);

    shownMode = actualMode;

    // The tools above the waveform change with how slices are cut.
    const int layout = actualMode * 10 + (int) processor.apvts.getRawParameterValue ("grid_type")->load();
    if (layout != shownLayout)
    {
        shownLayout = layout;
        updateToolbar();
        resized();
        repaint (manualNoteArea.isEmpty() ? juce::Rectangle<int> (0, 52, getWidth(), 58) : manualNoteArea);
    }

    const auto current = processor.getShownTable();
    shownTable = current;

    const int selected = juce::jlimit (0, PluginProcessor::kSlots - 1, processor.getSelectedIndex());
    bindSelected (selected);

    waveform.update (current, selected, isZoomed());
    keyMap.update (current, selected);
}

void SliceEditor::pollSlow()
{
    updateInfo();
}

void SliceEditor::refresh()
{
    pollFast();
    pollSlow();
    waveform.updateVoices();
    keyMap.updateVoices();
}

void SliceEditor::timerCallback()
{
    pollFast();

    if (++slowCounter >= (fast ? 6 : 3))
    {
        slowCounter = 0;
        pollSlow();
    }

    const bool moving = waveform.updateVoices();
    keyMap.updateVoices();

    if (moving != fast)
    {
        fast = moving;
        startTimerHz ((int) (fast ? fastRate : slowRate));
    }
}

juce::Component& SliceEditor::getStepperComponent() noexcept { return *stepper; }
juce::Point<float> SliceEditor::stepperNextArrow() const { return stepper->nextZone().getCentre(); }
juce::Point<float> SliceEditor::stepperPreviousArrow() const { return stepper->previousZone().getCentre(); }

juce::String SliceEditor::getSelectedText() const
{
    const int selected = juce::jlimit (0, PluginProcessor::kSlots - 1, processor.getSelectedIndex());
    return "Slice " + juce::String (selected + 1) + "  " + juce::MidiMessage::getMidiNoteName (processor.noteForSlice (selected), true, true, 3);
}

void SliceEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a vocal sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& c)
    {
        const auto file = c.getResult();
        if (file.existsAsFile())
            processor.loadFile (file);
    });
}

bool SliceEditor::acceptsFile (const juce::String& path) const
{
    return juce::File (path).hasFileExtension (fileTypes);
}

bool SliceEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (acceptsFile (f)) return true;
    return false;
}

void SliceEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    waveform.setDropActive (true);
}

void SliceEditor::fileDragExit (const juce::StringArray&)
{
    waveform.setDropActive (false);
}

void SliceEditor::filesDropped (const juce::StringArray& files, int, int)
{
    waveform.setDropActive (false);
    for (const auto& f : files)
        if (acceptsFile (f))
        {
            processor.loadFile (juce::File (f));
            return;
        }
}

// ---- the hint line -------------------------------------------------------------------------------------------------

void SliceEditor::setHint (const juce::String& text)
{
    if (text != hintText)
    {
        hintText = text;
        repaint (hintArea);
    }
}

void SliceEditor::showHintFor (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            setHint (hinted->hint);
            return;
        }
    setHint (restingHint);
}

void SliceEditor::mouseEnter (const juce::MouseEvent& e)  { showHintFor (e.eventComponent); }
void SliceEditor::mouseMove (const juce::MouseEvent& e)   { showHintFor (e.eventComponent); }
void SliceEditor::mouseExit (const juce::MouseEvent&)     { setHint (restingHint); }

void SliceEditor::previewHint (const juce::String& componentId)
{
    for (auto* c : getChildren())
        if (c->getComponentID() == componentId)
        {
            showHintFor (c);
            return;
        }
}
