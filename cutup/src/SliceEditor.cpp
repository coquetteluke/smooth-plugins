#include "SliceEditor.h"

// ---- waveform ----

float SliceWaveform::xForFrame (int frame) const
{
    auto s = processor.getSample();
    const int frames = s != nullptr ? s->frames() : 0;
    return frames > 0 ? (float) getWidth() * (float) frame / (float) frames : 0.0f;
}

int SliceWaveform::frameAtX (float x) const
{
    auto s = processor.getSample();
    const int frames = s != nullptr ? s->frames() : 0;
    if (frames == 0 || getWidth() <= 0) return 0;
    return juce::jlimit (0, frames - 1, (int) ((double) x / getWidth() * frames));
}

int SliceWaveform::sliceAtFrame (int frame) const
{
    for (int i = 0; i < table.count; ++i)
        if (frame >= table.start[i] && frame < table.start[i + 1]) return i;
    return -1;
}

int SliceWaveform::markerNearX (float x) const
{
    int best = -1;
    float distance = 5.0f;
    for (int i = 0; i < table.count; ++i)
    {
        const float d = std::abs (xForFrame (table.start[i]) - x);
        if (d <= distance) { distance = d; best = i; }
    }
    return best;
}

void SliceWaveform::refresh (const slicecore::SliceTable& t, int newSelected, bool playing)
{
    if (t != table || newSelected != selected || playing || wasPlaying)
    {
        table = t;
        selected = newSelected;
        wasPlaying = playing;
        repaint();
    }
}

void SliceWaveform::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff15171a));
    g.fillRect (bounds);

    auto s = processor.getSample();
    if (s == nullptr || s->isEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (16.0f);
        g.drawText ("Drop a vocal file here, or press Load", getLocalBounds(), juce::Justification::centred);
        return;
    }

    if (juce::isPositiveAndBelow (selected, table.count))
    {
        const float a = xForFrame (table.start[selected]), b = xForFrame (table.start[selected + 1]);
        g.setColour (juce::Colour (0xff2f4a66));
        g.fillRect (a, 0.0f, std::max (1.0f, b - a), bounds.getHeight());
    }

    const float mid = bounds.getHeight() * 0.5f;
    float largest = 0.01f;
    for (size_t i = 0; i < s->peakMax.size(); ++i)
        largest = std::max ({ largest, s->peakMax[i], -s->peakMin[i] });
    const float scale = (mid - 16.0f) / largest;

    g.setColour (juce::Colour (0xff9fc6e8));
    const int bins = (int) s->peakMax.size();
    for (int x = 0; x < getWidth(); ++x)
    {
        const int from = (int) ((juce::int64) x * bins / getWidth());
        const int to = std::max (from + 1, (int) ((juce::int64) (x + 1) * bins / getWidth()));
        float lo = 0.0f, hi = 0.0f;
        for (int b = from; b < std::min (to, bins); ++b)
        {
            lo = std::min (lo, s->peakMin[(size_t) b]);
            hi = std::max (hi, s->peakMax[(size_t) b]);
        }
        g.drawVerticalLine (x, mid - hi * scale, mid - lo * scale + 1.0f);
    }

    g.setFont (11.0f);
    for (int i = 0; i < table.count; ++i)
    {
        const float x = xForFrame (table.start[i]);
        g.setColour (i == selected ? juce::Colours::white : juce::Colour (0xfff0b64a));
        g.drawVerticalLine (juce::roundToInt (x), 0.0f, bounds.getHeight());
        const float next = xForFrame (table.start[i + 1]);
        if (next - x > 16.0f)
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (x + 3.0f, 2.0f, 26.0f, 13.0f), juce::Justification::centredLeft);
    }

    g.setColour (juce::Colours::lightgreen);
    for (int i = 0; i < PluginProcessor::kPlayheads; ++i)
    {
        const float p = processor.getPlayhead (i);
        if (p >= 0.0f)
            g.drawVerticalLine (juce::roundToInt (p * bounds.getWidth()), 0.0f, bounds.getHeight());
    }
}

void SliceWaveform::mouseDown (const juce::MouseEvent& e)
{
    dragging = -1;
    const int marker = markerNearX ((float) e.x);
    if (marker >= 0 && (e.mods.isAltDown() || e.mods.isPopupMenu()))
    {
        processor.removeMarker (marker);
        return;
    }
    if (marker >= 0)
    {
        dragging = marker;
        return;
    }
    const int slice = sliceAtFrame (frameAtX ((float) e.x));
    if (slice >= 0)
    {
        processor.selectSlice (slice);
        processor.audition (slice);
    }
}

void SliceWaveform::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging >= 0 && e.getDistanceFromDragStart() > 1)
        processor.moveMarker (dragging, frameAtX ((float) e.x));
}

void SliceWaveform::mouseUp (const juce::MouseEvent&) { dragging = -1; }

void SliceWaveform::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (markerNearX ((float) e.x) < 0)
        processor.addMarker (frameAtX ((float) e.x));
}

void SliceWaveform::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (markerNearX ((float) e.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

// ---- window ----

void SliceEditor::setupKnob (Knob& k, const char* text, const char* paramId)
{
    k.label.setText (text, juce::dontSendNotification);
    k.label.setFont (juce::FontOptions (12.0f));
    k.label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (k.label);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 18);
    addAndMakeVisible (k.slider);
    if (paramId != nullptr)
        k.attachment = std::make_unique<SliderAttachment> (processor.apvts, paramId, k.slider);
}

void SliceEditor::setupChoice (Choice& c, const char* text, const char* paramId)
{
    c.label.setText (text, juce::dontSendNotification);
    c.label.setFont (juce::FontOptions (12.0f));
    c.label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (c.label);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter (paramId)))
        c.box.addItemList (choice->choices, 1);
    addAndMakeVisible (c.box);
    c.attachment = std::make_unique<ComboAttachment> (processor.apvts, paramId, c.box);
}

SliceEditor::SliceEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), waveform (p)
{
    addAndMakeVisible (waveform);

    addAndMakeVisible (loadButton);
    loadButton.onClick = [this] { chooseFile(); };

    statusLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (statusLabel);
    selectedLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    addAndMakeVisible (selectedLabel);
    helpLabel.setFont (juce::FontOptions (11.0f));
    helpLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    helpLabel.setText ("Click a slice to select and hear it. Drag a marker to move it. Double click to add one. Option click or right click a marker to remove it.", juce::dontSendNotification);
    addAndMakeVisible (helpLabel);

    setupChoice (sliceMode, "Slice Mode", "slice_mode");
    setupChoice (grid, "Grid", "grid");
    setupChoice (playMode, "Play Mode", "play_mode");
    setupChoice (keyMode, "Key Mode", "key_mode");
    setupKnob (sensitivity, "Sensitivity", "sensitivity");
    setupKnob (minSlice, "Min Slice", "min_slice");
    setupKnob (polyphony, "Polyphony", "polyphony");
    setupKnob (transpose, "Transpose", "transpose");
    setupKnob (rootNote, "Root Note", "root_note");
    setupKnob (velSens, "Velocity Sens", "vel_sens");
    setupKnob (penvAmount, "Pitch Env Amount", "penv_amount");
    setupKnob (penvTime, "Pitch Env Time", "penv_time");
    setupKnob (bendRange, "Bend Range", "bend_range");
    setupKnob (output, "Output", "output");

    setupKnob (slotPitch, "Pitch", nullptr);
    setupKnob (slotAttack, "Attack", nullptr);
    setupKnob (slotRelease, "Release", nullptr);
    setupKnob (slotLevel, "Level", nullptr);
    addAndMakeVisible (slotReverse);

    setResizable (true, true);
    setResizeLimits (820, 520, 2000, 1400);
    setSize (980, 600);

    shownMode = (int) processor.apvts.getRawParameterValue ("slice_mode")->load();
    refresh();
    startTimerHz (30);
}

SliceEditor::~SliceEditor() { stopTimer(); }

void SliceEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff23262b));
}

void SliceEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto top = area.removeFromTop (28);
    loadButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (10);
    statusLabel.setBounds (top);
    area.removeFromTop (6);

    const auto column = [] (juce::Rectangle<int>& row, int width) { auto c = row.removeFromLeft (width); row.removeFromLeft (10); return c; };
    const auto placeKnob = [] (Knob& k, juce::Rectangle<int> c) { k.label.setBounds (c.removeFromTop (16)); k.slider.setBounds (c.removeFromTop (24)); };
    const auto placeChoice = [] (Choice& k, juce::Rectangle<int> c) { k.label.setBounds (c.removeFromTop (16)); k.box.setBounds (c.removeFromTop (24)); };

    auto row = area.removeFromTop (42);
    const int unit = (row.getWidth() - 30) / 4;
    placeChoice (sliceMode, column (row, unit));
    placeKnob (sensitivity, column (row, unit));
    placeChoice (grid, column (row, unit));
    placeKnob (minSlice, column (row, unit));
    area.removeFromTop (6);

    auto bottom = area.removeFromBottom (42);
    placeKnob (bendRange, column (bottom, unit));
    placeKnob (penvAmount, column (bottom, unit));
    placeKnob (penvTime, column (bottom, unit));
    placeKnob (output, column (bottom, unit));
    area.removeFromBottom (6);

    auto bottom2 = area.removeFromBottom (42);
    placeChoice (playMode, column (bottom2, unit));
    placeChoice (keyMode, column (bottom2, unit));
    placeKnob (polyphony, column (bottom2, unit));
    placeKnob (transpose, column (bottom2, unit));
    area.removeFromBottom (6);

    auto bottom3 = area.removeFromBottom (42);
    placeKnob (rootNote, column (bottom3, unit));
    placeKnob (velSens, column (bottom3, unit));
    area.removeFromBottom (6);

    auto slot = area.removeFromBottom (42);
    const int slotUnit = (slot.getWidth() - 50) / 6;
    selectedLabel.setBounds (column (slot, slotUnit));
    placeKnob (slotPitch, column (slot, slotUnit));
    placeKnob (slotAttack, column (slot, slotUnit));
    placeKnob (slotRelease, column (slot, slotUnit));
    placeKnob (slotLevel, column (slot, slotUnit));
    slotReverse.setBounds (column (slot, slotUnit).withTrimmedTop (14));
    area.removeFromBottom (4);

    helpLabel.setBounds (area.removeFromBottom (16));
    area.removeFromBottom (2);
    waveform.setBounds (area);
}

void SliceEditor::bindSelected (int index)
{
    if (index == boundSlot) return;
    boundSlot = index;
    auto& state = processor.apvts;
    slotPitch.attachment.reset();
    slotAttack.attachment.reset();
    slotRelease.attachment.reset();
    slotLevel.attachment.reset();
    slotReverseAttachment.reset();
    slotPitch.attachment = std::make_unique<SliderAttachment> (state, PluginProcessor::slotId (index, "pitch"), slotPitch.slider);
    slotAttack.attachment = std::make_unique<SliderAttachment> (state, PluginProcessor::slotId (index, "attack"), slotAttack.slider);
    slotRelease.attachment = std::make_unique<SliderAttachment> (state, PluginProcessor::slotId (index, "release"), slotRelease.slider);
    slotLevel.attachment = std::make_unique<SliderAttachment> (state, PluginProcessor::slotId (index, "level"), slotLevel.slider);
    slotReverseAttachment = std::make_unique<ButtonAttachment> (state, PluginProcessor::slotId (index, "reverse"), slotReverse);
}

void SliceEditor::refresh()
{
    const auto table = processor.getShownTable();
    const int mode = (int) processor.apvts.getRawParameterValue ("slice_mode")->load();

    // Picking Manual before any edit starts from the markers that were on show.
    if (mode == PluginProcessor::modeManual && shownMode != PluginProcessor::modeManual && ! processor.isManualEdited())
        processor.adoptAsManual (shownTable);
    shownMode = mode;

    const auto current = processor.getShownTable();
    shownTable = current;

    const int selected = juce::jlimit (0, PluginProcessor::kSlots - 1, processor.getSelectedIndex());
    bindSelected (selected);

    bool playing = false;
    for (int i = 0; i < PluginProcessor::kPlayheads && ! playing; ++i)
        playing = processor.getPlayhead (i) >= 0.0f;
    waveform.refresh (current, selected, playing);

    auto s = processor.getSample();
    juce::String text = processor.getStatus();
    if (text.isEmpty())
    {
        if (s != nullptr && ! s->isEmpty())
            text << s->name << "   " << juce::String (s->frames() / s->rate, 2) << " s   " << juce::String (s->rate / 1000.0, 1) << " kHz   "
                 << (s->audio.getNumChannels() > 1 ? "stereo" : "mono") << "   " << current.count << " slices";
        else
            text = "No sample loaded. It stays silent until one is.";
    }
    statusLabel.setText (text, juce::dontSendNotification);

    const juce::String note = juce::MidiMessage::getMidiNoteName (processor.noteForSlice (selected), true, true, 3);
    selectedLabel.setText ("Slice " + juce::String (selected + 1) + "  " + note, juce::dontSendNotification);
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

bool SliceEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;m4a;caf")) return true;
    return false;
}

void SliceEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;m4a;caf"))
        {
            processor.loadFile (juce::File (f));
            return;
        }
}
