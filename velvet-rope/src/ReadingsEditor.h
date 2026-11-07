#pragma once
#include "PluginProcessor.h"

// The stock generic editor, plus read-only rows laid out like the stock rows. Not a designed window: that comes later.
// The stock editor leaves out parameters that are not automatable, and readings must not be automatable, so the rows are
// added here. They only poll atomics on a timer and have no link back to the host, so they cannot send a value change.
class ReadingsEditor final : public juce::GenericAudioProcessorEditor,
                             private juce::Timer
{
public:
    static constexpr int rowHeight = 40;

    ReadingsEditor (juce::AudioProcessor& p, std::initializer_list<ReadingParameter*> readings)
        : juce::GenericAudioProcessorEditor (p)
    {
        for (auto* r : readings)
            rows.add (new Row (*this, *r));
        setSize (getWidth(), getHeight() + rows.size() * rowHeight);
        timerCallback();
        startTimerHz (20);
    }

    ~ReadingsEditor() override { stopTimer(); }

    void resized() override
    {
        juce::GenericAudioProcessorEditor::resized();
        auto area = getLocalBounds();
        auto rowArea = area.removeFromBottom (rows.size() * rowHeight);
        if (auto* stockRows = getChildComponent (0)) // the tree view of stock rows is always the first child
            stockRows->setBounds (area);
        for (auto* r : rows)
            r->layout (rowArea.removeFromTop (rowHeight));
    }

private:
    struct Row
    {
        Row (ReadingsEditor& owner, ReadingParameter& r) : reading (r)
        {
            name.setText (r.getName (128), juce::dontSendNotification);
            name.setJustificationType (juce::Justification::centredRight);
            unit.setText (r.getLabel(), juce::dontSendNotification);
            value.setJustificationType (juce::Justification::centred);
            value.setBorderSize ({ 1, 1, 1, 1 });
            value.setColour (juce::Label::outlineColourId, bar.findColour (juce::Slider::textBoxOutlineColourId));
            bar.setRange (0.0, 1.0);
            bar.setEnabled (false);
            bar.setColour (juce::Slider::trackColourId, bar.findColour (juce::Slider::thumbColourId));
            for (auto* c : std::initializer_list<juce::Component*> { &name, &unit, &value, &bar })
            {
                c->setInterceptsMouseClicks (false, false);
                owner.addAndMakeVisible (c);
            }
        }

        void layout (juce::Rectangle<int> row)
        {
            row.removeFromLeft (20);
            name.setBounds (row.removeFromLeft (100));
            unit.setBounds (row.removeFromRight (50));
            auto middle = row.reduced (0, 10);
            value.setBounds (middle.removeFromRight (80));
            middle.removeFromLeft (6);
            bar.setBounds (middle);
        }

        void refresh()
        {
            bar.setValue (reading.getBarPosition(), juce::dontSendNotification);
            value.setText (reading.getCurrentValueAsText(), juce::dontSendNotification);
        }

        ReadingParameter& reading;
        juce::Label name, unit, value;
        juce::Slider bar { juce::Slider::LinearBar, juce::Slider::NoTextBox };
    };

    void timerCallback() override
    {
        for (auto* r : rows)
            r->refresh();
    }

    juce::OwnedArray<Row> rows;
};
