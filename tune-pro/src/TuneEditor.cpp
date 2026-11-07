#include "TuneEditor.h"

using namespace smoothtune;

namespace
{
    const juce::Colour background { 0xff1c1f24 }, panel { 0xff262a31 }, grid { 0xff3a3f48 }, gridStrong { 0xff565d69 }, text { 0xffe8eaee }, dimText { 0xff9aa2ae };
    const juce::Colour inputColour { 0xffb0b6c0 }, outputColour { 0xff4fd18b }, targetColour { 0xfff2c14e }, removeFill { 0xffd9534f }, bypassFill { 0xffe69a3c };

    juce::String noteName (int midi) { return midi < 0 ? juce::String ("no note") : juce::String (noteNames[midi % 12]) + juce::String (midi / 12 - 1); }

    // Key layout across one octave, in white key widths.
    constexpr bool isBlack[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    constexpr float whiteIndex[12] = { 0, 0.68f, 1, 1.68f, 2, 3, 3.68f, 4, 4.68f, 5, 5.68f, 6 };
}

// ------------------------------------------------------------------------------------------------------------ pitch

void PitchView::followPitch (int points) noexcept
{
    // Keep the middle of what was sung lately on screen. It moves in whole notes, so grid lines stay put between moves.
    const auto count = processor.getPitchHistoryCount();
    float lowest = 1000.0f, highest = -1000.0f;
    for (int k = 0; k < points && k < count; ++k)
    {
        const float n = processor.getPitchPoint (count - 1 - k).outputNote.load (std::memory_order_relaxed);
        if (n > 0.0f)
        {
            lowest = juce::jmin (lowest, n);
            highest = juce::jmax (highest, n);
        }
    }
    if (highest < lowest)
        return;
    if (lowest < lowNote + 2.0f || highest > lowNote + spanNotes - 2.0f)
        lowNote = std::round (0.5f * (lowest + highest) - 0.5f * spanNotes);
}

void PitchView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, 4.0f);

    const int points = (int) (secondsShown * 200.0);
    followPitch (points);

    const auto area = bounds.reduced (6.0f).withTrimmedLeft (34.0f);
    const auto yOf = [&] (float note) { return area.getBottom() - (note - lowNote) / spanNotes * area.getHeight(); };

    // Grid: every semitone, stronger on C, labelled where notes are in the current scale.
    const auto inScale = processor.getInScaleMask();
    g.setFont (11.0f);
    for (int n = (int) std::ceil (lowNote); n <= (int) std::floor (lowNote + spanNotes); ++n)
    {
        const float y = yOf ((float) n);
        const int pitchClass = ((n % 12) + 12) % 12;
        const bool in = (inScale >> pitchClass) & 1;
        g.setColour (pitchClass == 0 ? gridStrong : grid.withAlpha (in ? 1.0f : 0.45f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        if (in)
        {
            g.setColour (pitchClass == 0 ? text : dimText);
            g.drawText (noteName (n), juce::Rectangle<float> (bounds.getX() + 4.0f, y - 7.0f, 32.0f, 14.0f), juce::Justification::centredLeft, false);
        }
    }

    const auto count = processor.getPitchHistoryCount();
    const auto xOf = [&] (int k) { return area.getRight() - (float) k / (float) points * area.getWidth(); };

    // Target note, as a soft band behind the lines.
    g.setColour (targetColour.withAlpha (0.22f));
    for (int k = 0; k < points && k < count; ++k)
    {
        const float t = processor.getPitchPoint (count - 1 - k).targetNote.load (std::memory_order_relaxed);
        if (t >= 0.0f)
        {
            const float y = yOf (t);
            g.fillRect (xOf (k + 1), y - 2.0f, juce::jmax (1.0f, xOf (k) - xOf (k + 1)), 4.0f);
        }
    }

    const auto drawLine = [&] (auto read, juce::Colour colour, float width)
    {
        juce::Path path;
        bool open = false;
        float previous = 0.0f;
        for (int k = 0; k < points && k < count; ++k)
        {
            const float n = read (processor.getPitchPoint (count - 1 - k));
            if (n <= 0.0f || n < lowNote - 2.0f || n > lowNote + spanNotes + 2.0f)
            {
                open = false;
                continue;
            }
            const float x = xOf (k), y = yOf (n);
            if (open && std::abs (n - previous) < 5.0f) path.lineTo (x, y);
            else path.startNewSubPath (x, y);
            open = true;
            previous = n;
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (width));
    };
    g.saveState();
    g.reduceClipRegion (area.toNearestInt());
    drawLine ([] (const PluginProcessor::PitchPoint& p) { return p.inputNote.load (std::memory_order_relaxed); }, inputColour, 1.2f);
    drawLine ([] (const PluginProcessor::PitchPoint& p) { return p.outputNote.load (std::memory_order_relaxed); }, outputColour, 2.0f);
    g.restoreState();

    // Legend
    g.setFont (12.0f);
    auto legend = juce::Rectangle<float> (area.getX() + 8.0f, area.getY() + 4.0f, 280.0f, 16.0f);
    const auto entry = [&] (const char* name, juce::Colour c, float width)
    {
        g.setColour (c);
        g.fillRect (legend.getX(), legend.getCentreY() - 1.0f, 14.0f, 2.5f);
        g.setColour (text);
        g.drawText (name, legend.withTrimmedLeft (18.0f).withWidth (width), juce::Justification::centredLeft, false);
        legend.removeFromLeft (18.0f + width);
    };
    entry ("Input", inputColour, 50.0f);
    entry ("Corrected", outputColour, 72.0f);
    entry ("Target", targetColour, 50.0f);
}

// --------------------------------------------------------------------------------------------------------- keyboard

int KeyboardView::modeOf (int pitchClass) const noexcept
{
    return juce::roundToInt (processor.apvts.getRawParameterValue ("note" + juce::String (pitchClass))->load (std::memory_order_relaxed));
}

juce::Rectangle<float> KeyboardView::keyBounds (int pc) const noexcept
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const float white = area.getWidth() / 7.0f;
    if (isBlack[pc])
        return { area.getX() + whiteIndex[pc] * white, area.getY(), white * 0.64f, area.getHeight() * 0.6f };
    return { area.getX() + whiteIndex[pc] * white, area.getY(), white, area.getHeight() };
}

int KeyboardView::noteAt (juce::Point<float> p) const noexcept
{
    for (int pc = 0; pc < 12; ++pc)
        if (isBlack[pc] && keyBounds (pc).contains (p))
            return pc;
    for (int pc = 0; pc < 12; ++pc)
        if (! isBlack[pc] && keyBounds (pc).contains (p))
            return pc;
    return -1;
}

void KeyboardView::cycleNote (int pc)
{
    if (! juce::isPositiveAndBelow (pc, 12))
        return;
    auto* parameter = processor.apvts.getParameter ("note" + juce::String (pc));
    const int next = (modeOf (pc) + 1) % 3;
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) next));
    parameter->endChangeGesture();
    repaint();
}

void KeyboardView::mouseDown (const juce::MouseEvent& e)
{
    cycleNote (noteAt (e.position));
}

void KeyboardView::paint (juce::Graphics& g)
{
    const auto r = processor.getReadings();
    const int targetClass = r.voiced && r.targetNote >= 0 ? r.targetNote % 12 : -1;
    const auto inScale = processor.getInScaleMask();

    const auto drawKey = [&] (int pc)
    {
        const auto box = keyBounds (pc).reduced (1.0f);
        const int mode = modeOf (pc);
        const bool black = isBlack[pc], in = ((inScale >> pc) & 1) != 0;
        auto fill = black ? juce::Colour (0xff23262c) : juce::Colour (0xffe9ebef);
        if (mode == noteRemove) fill = removeFill;
        else if (mode == noteBypass) fill = bypassFill;
        else if (! in) fill = black ? juce::Colour (0xff17191d) : juce::Colour (0xff8a8f98);
        g.setColour (fill);
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (juce::Colour (0xff0d0e10));
        g.drawRoundedRectangle (box, 3.0f, 1.0f);

        const bool lightKey = ! black && mode == noteTune;
        const auto ink = lightKey ? juce::Colour (0xff15171a) : juce::Colours::white;
        g.setColour (ink);
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        auto labelArea = box.withTrimmedTop (juce::jmax (0.0f, box.getHeight() - 38.0f));
        g.drawText (noteNames[pc], labelArea.removeFromTop (18.0f), juce::Justification::centred, false);
        g.setFont (11.0f);
        const char* state = mode == noteRemove ? "Remove" : mode == noteBypass ? "Bypass" : in ? "Tune" : "Off scale";
        g.drawText (state, labelArea, juce::Justification::centred, false);

        if (pc == targetClass)
        {
            g.setColour (outputColour);
            g.drawRoundedRectangle (box.reduced (1.5f), 3.0f, 3.5f);
            g.fillEllipse (box.getCentreX() - 6.0f, box.getY() + 8.0f, 12.0f, 12.0f);
        }
    };
    for (int pc = 0; pc < 12; ++pc) if (! isBlack[pc]) drawKey (pc);
    for (int pc = 0; pc < 12; ++pc) if (isBlack[pc]) drawKey (pc);
}

// ------------------------------------------------------------------------------------------------------------- main

TuneEditor::TuneEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), pitch (p), keyboard (p)
{
    addAndMakeVisible (pitch);
    addAndMakeVisible (keyboard);

    for (auto* id : { "key", "scale", "inputType", "engine", "midiOctave" })
        addChoice (id);
    for (auto* id : { "followKey", "midiTargets", "classic", "formantPreserve", "bypass" })
        addToggle (id);
    for (auto* id : { "retune", "flex", "humanize", "naturalVibrato", "throat", "transpose",
                      "reference", "tracking", "mix", "vibratoAmount", "vibratoRate", "vibratoDelay" })
        addKnob (id);

    setSize (980, 760);
    startTimerHz (30);
}

void TuneEditor::addKnob (const char* id)
{
    auto* k = knobs.add (new Knob());
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 18);
    k->slider.setColour (juce::Slider::rotarySliderFillColourId, outputColour);
    k->slider.setColour (juce::Slider::thumbColourId, text);
    k->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k->label.setText (processor.apvts.getParameter (id)->getName (32), juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    k->label.setFont (juce::Font (juce::FontOptions (13.0f)));
    k->label.setColour (juce::Label::textColourId, text);
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, id, k->slider);
}

void TuneEditor::addChoice (const char* id)
{
    auto* c = choices.add (new Choice());
    auto* parameter = processor.apvts.getParameter (id);
    c->box.addItemList (parameter->getAllValueStrings(), 1);
    c->label.setText (parameter->getName (32), juce::dontSendNotification);
    c->label.setJustificationType (juce::Justification::centredLeft);
    c->label.setFont (juce::Font (juce::FontOptions (13.0f)));
    c->label.setColour (juce::Label::textColourId, text);
    addAndMakeVisible (c->box);
    addAndMakeVisible (c->label);
    c->attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (processor.apvts, id, c->box);
}

void TuneEditor::addToggle (const char* id)
{
    auto* t = toggles.add (new Toggle());
    t->button.setButtonText (processor.apvts.getParameter (id)->getName (32));
    t->button.setColour (juce::ToggleButton::textColourId, text);
    t->button.setColour (juce::ToggleButton::tickColourId, outputColour);
    addAndMakeVisible (t->button);
    t->attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.apvts, id, t->button);
}

void TuneEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    area.removeFromTop (stripHeight - 6);
    pitch.setBounds (area.removeFromTop (270));
    area.removeFromTop (8);
    keyboard.setBounds (area.removeFromTop (120).withSizeKeepingCentre (juce::jmin (area.getWidth(), 620), 120));
    area.removeFromTop (8);

    auto left = area.removeFromLeft (250);
    area.removeFromLeft (10);
    for (auto* c : choices)
    {
        auto row = left.removeFromTop (28);
        c->label.setBounds (row.removeFromLeft (92));
        c->box.setBounds (row.reduced (0, 2));
    }
    left.removeFromTop (6);
    for (auto* t : toggles)
        t->button.setBounds (left.removeFromTop (24));

    const int columns = 6, rows = 2;
    const int w = area.getWidth() / columns, h = area.getHeight() / rows;
    for (int i = 0; i < knobs.size(); ++i)
    {
        juce::Rectangle<int> box (area.getX() + (i % columns) * w, area.getY() + (i / columns) * h, w, h);
        knobs[i]->label.setBounds (box.removeFromTop (18));
        knobs[i]->slider.setBounds (box);
    }
}

juce::String TuneEditor::readingText() const
{
    const auto r = processor.getReadings();
    auto left = juce::String ("Active Key ") + noteNames[juce::jlimit (0, 11, r.activeKey)] + " " + scaleNames[juce::jlimit (0, numScales - 1, r.activeScale)];
    if (! r.voiced)
        return left + "  |  no pitch";
    return left + "  |  In " + juce::String (r.detectedHz, 1) + " Hz  |  Out " + juce::String (r.outputHz, 1) + " Hz  |  Target " + noteName (r.targetNote)
           + "  |  Correction " + juce::String (juce::roundToInt (r.correctionCents)) + " cents";
}

void TuneEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    auto strip = getLocalBounds().withHeight (stripHeight).reduced (12, 6).toFloat();
    g.setColour (text);
    g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    g.drawText ("Smooth Tune Pro", strip.removeFromLeft (150.0f), juce::Justification::centredLeft, false);
    g.setFont (13.0f);
    const double latencyMs = processor.getLatencySamples() * 1000.0 / juce::jmax (1.0, processor.getSampleRate());
    const auto r = processor.getReadings();
    g.setColour (dimText);
    g.drawText ((r.activeEngine == TuneEngine::highQuality ? juce::String ("High Quality") : juce::String ("Low Latency")) + ", " + juce::String (latencyMs, 1) + " ms",
                strip.removeFromRight (170.0f), juce::Justification::centredRight, false);
    g.setColour (text);
    g.drawText (readingText(), strip, juce::Justification::centredLeft, true);
}
