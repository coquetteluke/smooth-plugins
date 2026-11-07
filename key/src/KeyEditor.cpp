#include "KeyEditor.h"

namespace
{
    const juce::Colour background (0xff12151c), panel (0xff1c212b), ink (0xfff3f5f8), faint (0xff8b93a3);
    const juce::Colour accent (0xff55d6be), warning (0xffffa94d), keyOff (0xff2a303c), keyOn (0xffd9dee7);

    constexpr const char* flatNames[12] = { "", "Db", "", "Eb", "", "", "Gb", "", "Ab", "", "Bb", "" };
    constexpr int majorSteps[7] = { 0, 2, 4, 5, 7, 9, 11 };
    constexpr int minorSteps[7] = { 0, 2, 3, 5, 7, 8, 10 };

    bool inScale (int note, int key, bool minor)
    {
        const int step = (note - key + 12) % 12;
        for (int s : (minor ? minorSteps : majorSteps))
            if (s == step) return true;
        return false;
    }
}

KeyEditor::KeyEditor (PluginProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    addChoice (responseBox, responseLabel, "response", "Response");
    addChoice (sourceBox, sourceLabel, "source", "Source");
    addChoice (modeBox, modeLabel, "mode", "Mode");
    addChoice (manualKeyBox, manualKeyLabel, "manualKey", "Manual Key");
    addChoice (manualScaleBox, manualScaleLabel, "manualScale", "Manual Scale");
    addChoice (tempoScaleBox, tempoScaleLabel, "tempoScale", "Tempo Scale");
    responseAttach = std::make_unique<ComboAttachment> (p.apvts, "response", responseBox);
    sourceAttach = std::make_unique<ComboAttachment> (p.apvts, "source", sourceBox);
    modeAttach = std::make_unique<ComboAttachment> (p.apvts, "mode", modeBox);
    manualKeyAttach = std::make_unique<ComboAttachment> (p.apvts, "manualKey", manualKeyBox);
    manualScaleAttach = std::make_unique<ComboAttachment> (p.apvts, "manualScale", manualScaleBox);
    tempoScaleAttach = std::make_unique<ComboAttachment> (p.apvts, "tempoScale", tempoScaleBox);

    for (auto* b : { &listeningButton, &swapButton })
    {
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonColourId, panel);
        b->setColour (juce::TextButton::buttonOnColourId, accent.darker (0.3f));
        addAndMakeVisible (*b);
    }
    listeningAttach = std::make_unique<ButtonAttachment> (p.apvts, "listening", listeningButton);
    swapAttach = std::make_unique<ButtonAttachment> (p.apvts, "swapRelative", swapButton);

    resetButton.setColour (juce::TextButton::buttonColourId, panel);
    resetButton.onStateChange = [this]
    {
        // Held on while the mouse is down, so a host recording automation sees a real press.
        auto* param = processor.apvts.getParameter ("reset");
        const bool down = resetButton.isDown();
        if (down == (param->getValue() >= 0.5f))
            return;
        if (down) processor.requestReset();
        param->beginChangeGesture();
        param->setValueNotifyingHost (down ? 1.0f : 0.0f);
        param->endChangeGesture();
    };
    addAndMakeVisible (resetButton);

    setResizable (true, true);
    setResizeLimits (560, 420, 1600, 1200);
    setSize (820, 600);
    refresh();
    startTimerHz (15);
}

KeyEditor::~KeyEditor()
{
    stopTimer();
}

void KeyEditor::addChoice (juce::ComboBox& box, juce::Label& label, const char* paramId, const char* labelText)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter (paramId)))
        box.addItemList (choice->choices, 1);
    box.setColour (juce::ComboBox::backgroundColourId, panel);
    box.setColour (juce::ComboBox::outlineColourId, keyOff);
    addAndMakeVisible (box);
    label.setText (labelText, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, faint);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (label);
}

void KeyEditor::refresh()
{
    const auto d = processor.getDisplay();
    shownKey = d.key;
    shownMinor = d.minor;
    confidence = d.confidence;
    chroma = d.chroma;

    if (d.key >= 0)
    {
        keyText = smoothkey::keyNames[d.key];
        scaleText = d.minor ? "minor" : "major";
        otherNameText = juce::String (flatNames[d.key]).isNotEmpty() ? "also written " + juce::String (flatNames[d.key]) : juce::String();
        const int rel = d.minor ? (d.key + 3) % 12 : (d.key + 9) % 12;
        relativeText = "Relative: " + juce::String (smoothkey::keyNames[rel]) + (d.minor ? " major" : " minor");
    }
    else
    {
        keyText = "--";
        scaleText = "";
        otherNameText = "";
        relativeText = "";
    }

    if (d.manual) statusText = "Set by hand";
    else if (! d.active) statusText = "Paused";
    else if (! d.signal) statusText = d.key >= 0 ? "No audio, last key heard" : "Waiting for audio";
    else statusText = d.key >= 0 ? "Listening" : "Listening, no key yet";
    dimmed = d.key < 0 || (! d.manual && d.active && ! d.signal);

    noteText = processor.hasFileFailed() ? "Could not save key.json, so other plug-ins will not see this key" : "";

    if (d.manual)
    {
        referenceText = "A = 440 Hz";
        referenceOff = false;
        tempoText = "--- BPM";
    }
    else
    {
        referenceText = d.key >= 0 ? "A = " + juce::String (d.referenceHz, 1) + " Hz" : "A = --- Hz";
        referenceOff = d.key >= 0 && std::abs (d.referenceHz - 440.0f) >= 1.0f;
        tempoText = d.shownBpm >= 1.0f ? juce::String (d.shownBpm, 1) + " BPM" : "--- BPM";
    }
    repaint();
}

void KeyEditor::resized()
{
    auto area = getLocalBounds().reduced (juce::jmax (10, getWidth() / 50));
    const int rowHeight = juce::jlimit (26, 34, getHeight() / 20);
    const int labelHeight = 16;

    auto controls = area.removeFromBottom (rowHeight * 2 + labelHeight * 2 + 12);
    const auto rowOf = [&] (std::initializer_list<std::pair<juce::Label*, juce::ComboBox*>> boxes, std::initializer_list<juce::Button*> buttons)
    {
        auto row = controls.removeFromTop (rowHeight + labelHeight + 6);
        const int count = (int) (boxes.size() + buttons.size());
        const int width = row.getWidth() / juce::jmax (1, count);
        for (auto& pair : boxes)
        {
            auto cell = row.removeFromLeft (width).reduced (4, 0);
            pair.first->setBounds (cell.removeFromTop (labelHeight));
            pair.second->setBounds (cell.removeFromTop (rowHeight));
        }
        for (auto* b : buttons)
        {
            auto cell = row.removeFromLeft (width).reduced (4, 0);
            cell.removeFromTop (labelHeight);
            b->setBounds (cell.removeFromTop (rowHeight));
        }
    };
    rowOf ({ { &modeLabel, &modeBox }, { &responseLabel, &responseBox }, { &sourceLabel, &sourceBox } }, { &listeningButton, &resetButton });
    rowOf ({ { &manualKeyLabel, &manualKeyBox }, { &manualScaleLabel, &manualScaleBox }, { &tempoScaleLabel, &tempoScaleBox } }, { &swapButton });

    area.removeFromBottom (8);
    keyboardArea = area.removeFromBottom (juce::jmax (46, area.getHeight() / 7));
    area.removeFromBottom (6);
    meterArea = area.removeFromBottom (juce::jmax (30, area.getHeight() / 9));
    area.removeFromBottom (6);
    infoArea = area.removeFromBottom (juce::jmax (22, area.getHeight() / 11));
    readoutArea = area;
}

void KeyEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto top = readoutArea;
    const float small = juce::jlimit (13.0f, 20.0f, (float) getHeight() / 36.0f);
    auto header = top.removeFromTop ((int) small + 8);
    g.setFont (juce::FontOptions (small, juce::Font::bold));
    g.setColour (faint);
    g.drawText ("Smooth Key", header, juce::Justification::centredLeft);
    g.setColour (dimmed ? faint : accent);
    g.drawText (statusText, header, juce::Justification::centredRight);

    // The reading itself. The whole area goes to it, and the text is fitted to the width.
    auto line = top.toFloat();
    auto sub = line.removeFromBottom (small + 10.0f);
    const float keySize = line.getHeight() * 0.92f;
    juce::Font keyFont { juce::FontOptions (keySize, juce::Font::bold) };
    juce::Font scaleFont { juce::FontOptions (keySize * 0.42f) };
    float keyWidth = juce::GlyphArrangement::getStringWidth (keyFont, keyText);
    float scaleWidth = scaleText.isNotEmpty() ? juce::GlyphArrangement::getStringWidth (scaleFont, " " + scaleText) : 0.0f;
    const float fit = juce::jmin (1.0f, line.getWidth() / juce::jmax (1.0f, keyWidth + scaleWidth));
    keyFont.setHeight (keySize * fit);
    scaleFont.setHeight (keySize * 0.42f * fit);
    keyWidth *= fit;
    scaleWidth *= fit;
    const float startX = line.getCentreX() - (keyWidth + scaleWidth) * 0.5f;
    const float alpha = dimmed ? 0.4f : 0.45f + 0.55f * juce::jlimit (0.0f, 1.0f, confidence * 1.4f);
    g.setColour (ink.withAlpha (alpha));
    g.setFont (keyFont);
    g.drawText (keyText, juce::Rectangle<float> (startX, line.getY(), keyWidth + 4.0f, line.getHeight()), juce::Justification::centredLeft, false);
    g.setFont (scaleFont);
    g.drawText (" " + scaleText, juce::Rectangle<float> (startX + keyWidth, line.getY(), scaleWidth + 4.0f, line.getHeight()), juce::Justification::centredLeft, false);

    g.setFont (juce::FontOptions (small));
    g.setColour (faint);
    g.drawText (otherNameText, sub, juce::Justification::centredLeft);
    g.drawText (relativeText, sub, juce::Justification::centredRight);

    // Reference pitch and tempo. Orange when the music is not at A = 440.
    auto info = infoArea.toFloat();
    g.setFont (juce::FontOptions (juce::jlimit (15.0f, 26.0f, info.getHeight() * 0.8f), juce::Font::bold));
    g.setColour (referenceOff ? warning : faint);
    g.drawText (referenceText, info.removeFromLeft (info.getWidth() * 0.5f), juce::Justification::centredLeft);
    g.setColour (faint);
    g.drawText (tempoText, info, juce::Justification::centredRight);

    // Confidence
    auto meter = meterArea.toFloat();
    const float meterFont = juce::jlimit (18.0f, 44.0f, meter.getHeight() * 0.85f);
    auto number = meter.removeFromRight (meterFont * 2.6f);
    g.setFont (juce::FontOptions (meterFont, juce::Font::bold));
    g.setColour (ink);
    g.drawText (juce::String (juce::roundToInt (confidence * 100.0f)) + "%", number, juce::Justification::centredRight);
    meter = meter.reduced (0.0f, meter.getHeight() * 0.18f);
    g.setColour (panel);
    g.fillRoundedRectangle (meter, 5.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (meter.withWidth (meter.getWidth() * juce::jlimit (0.0f, 1.0f, confidence)), 5.0f);

    // One octave of keys: scale notes lit and named, the home note bold, and a level for how much each note is sounding.
    const auto keys = keyboardArea.toFloat();
    constexpr int whiteNotes[7] = { 0, 2, 4, 5, 7, 9, 11 };
    constexpr int blackNotes[5] = { 1, 3, 6, 8, 10 };
    constexpr float blackAfter[5] = { 1.0f, 2.0f, 4.0f, 5.0f, 6.0f };
    const float whiteWidth = keys.getWidth() / 7.0f;
    const float labelSize = juce::jlimit (11.0f, 18.0f, keys.getHeight() * 0.28f);
    const auto drawKey = [&] (juce::Rectangle<float> r, int note)
    {
        const bool lit = shownKey >= 0 && inScale (note, shownKey, shownMinor);
        const bool home = note == shownKey;
        g.setColour (lit ? (home ? accent : keyOn) : keyOff);
        g.fillRoundedRectangle (r.reduced (1.0f), 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (r.reduced (1.0f).removeFromBottom (r.getHeight() * 0.12f * chroma[(size_t) note]));
        if (lit)
        {
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (labelSize, home ? juce::Font::bold : juce::Font::plain));
            g.drawText (smoothkey::keyNames[note], r, juce::Justification::centredBottom);
        }
    };
    for (int i = 0; i < 7; ++i)
        drawKey ({ keys.getX() + (float) i * whiteWidth, keys.getY(), whiteWidth, keys.getHeight() }, whiteNotes[i]);
    for (int i = 0; i < 5; ++i)
    {
        const float w = whiteWidth * 0.62f;
        juce::Rectangle<float> r (keys.getX() + blackAfter[i] * whiteWidth - w * 0.5f, keys.getY(), w, keys.getHeight() * 0.62f);
        g.setColour (background);
        g.fillRect (r);
        drawKey (r, blackNotes[i]);
    }

    if (noteText.isNotEmpty())
    {
        g.setColour (warning);
        g.setFont (juce::FontOptions (small * 0.8f));
        g.drawText (noteText, header.translated (0, (int) small + 6), juce::Justification::centred);
    }
}
