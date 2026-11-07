#include "TuneFxEditor.h"

TuneFxEditor::TuneFxEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), generic (p)
{
    addAndMakeVisible (generic);
    setSize (juce::jmax (520, generic.getWidth()), generic.getHeight() + stripHeight);
    startTimerHz (20);
}

void TuneFxEditor::resized()
{
    generic.setBounds (getLocalBounds().withTrimmedTop (stripHeight));
}

juce::String TuneFxEditor::readingText() const
{
    const auto r = processor.getReadings();
    auto text = juce::String ("Active Key ") + smoothtune::noteNames[juce::jlimit (0, 11, r.activeKey)] + " " + smoothtune::scaleNames[juce::jlimit (0, smoothtune::numScales - 1, r.activeScale)];
    if (! r.voiced)
        return text + "  |  no pitch";
    const juce::String note = r.targetNote < 0 ? juce::String ("no note") : juce::String (smoothtune::noteNames[r.targetNote % 12]) + juce::String (r.targetNote / 12 - 1);
    return text + "  |  " + juce::String (r.detectedHz, 1) + " Hz  |  Target " + note
           + "  |  Correction " + juce::String (juce::roundToInt (r.correctionCents)) + " cents"
           + (r.heldNotes > 0 ? "  |  MIDI notes held " + juce::String (r.heldNotes) : juce::String());
}

void TuneFxEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    const auto r = processor.getReadings();

    auto strip = getLocalBounds().withHeight (stripHeight).reduced (10, 6).toFloat();
    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    g.drawText (readingText(), strip.removeFromTop (18.0f), juce::Justification::centredLeft, true);

    // Pitch Off Target as a bar from -100 to +100 cents around the middle.
    auto bar = strip.removeFromTop (16.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (bar);
    g.setColour (juce::Colours::grey);
    g.fillRect (bar.getCentreX() - 0.5f, bar.getY(), 1.0f, bar.getHeight());
    if (r.voiced)
    {
        const float amount = juce::jlimit (-1.0f, 1.0f, r.centsOffTarget / 100.0f) * bar.getWidth() * 0.5f;
        g.setColour (std::abs (r.centsOffTarget) < 10.0f ? juce::Colours::lightgreen : juce::Colours::orange);
        if (amount >= 0.0f) g.fillRect (bar.getCentreX(), bar.getY() + 2.0f, amount, bar.getHeight() - 4.0f);
        else g.fillRect (bar.getCentreX() + amount, bar.getY() + 2.0f, -amount, bar.getHeight() - 4.0f);
    }
    g.setColour (juce::Colours::white);
    g.setFont (12.0f);
    g.drawText ("Pitch Off Target " + (r.voiced ? juce::String (juce::roundToInt (r.centsOffTarget)) + " cents" : juce::String ("-")),
                strip, juce::Justification::centredLeft, true);
}
