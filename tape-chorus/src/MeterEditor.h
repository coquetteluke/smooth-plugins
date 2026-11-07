#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// Read-only reading: level or pitch swing. An atomic, never saved, never automated, and writes from a host or slider are ignored.
class Reading final : public juce::AudioProcessorParameterWithID
{
public:
    Reading (const char* id, const char* name, const char* label, float lowest, float highest, int decimalPlaces, const char* textAtLowest,
             juce::AudioProcessorParameter::Category category = juce::AudioProcessorParameter::otherMeter)
        : juce::AudioProcessorParameterWithID (juce::ParameterID { id, 1 }, name,
                                               juce::AudioProcessorParameterWithIDAttributes()
                                                   .withLabel (label)
                                                   .withAutomatable (false)
                                                   .withCategory (category)),
          lo (lowest), hi (highest), decimals (decimalPlaces), lowestText (textAtLowest), now (lowest) {}

    void set (float v) noexcept       { now.store (juce::jlimit (lo, hi, v), std::memory_order_relaxed); }
    float get() const noexcept        { return now.load (std::memory_order_relaxed); }

    float getValue() const override        { return (get() - lo) / (hi - lo); }
    void setValue (float) override         {}
    float getDefaultValue() const override { return 0.0f; }

    juce::String getText (float normalised, int) const override
    {
        const auto v = lo + normalised * (hi - lo);
        return v <= lo + 1.0e-3f * (hi - lo) ? juce::String (lowestText) : juce::String (v, decimals);
    }

    float getValueForText (const juce::String& text) const override
    {
        return juce::jlimit (0.0f, 1.0f, (text.getFloatValue() - lo) / (hi - lo));
    }

private:
    const float lo, hi;
    const int decimals;
    const char* const lowestText;
    std::atomic<float> now;
};

// JUCE's generic editor plus one live row for each reading, because its stock rows never move for a parameter that only reads.
class MeterEditor final : public juce::GenericAudioProcessorEditor,
                          private juce::Timer
{
public:
    static constexpr int rowHeight = 34;

    MeterEditor (juce::AudioProcessor& p, std::initializer_list<Reading*> readings)
        : juce::GenericAudioProcessorEditor (p)
    {
        for (auto* m : readings) addRow (m);
        setSize (getWidth(), 12 * 40 + rows.size() * rowHeight);
        refreshReading();
        startTimerHz (20);
    }

    ~MeterEditor() override { stopTimer(); }

    void resized() override
    {
        juce::GenericAudioProcessorEditor::resized();
        auto area = getLocalBounds();
        auto rowArea = area.removeFromBottom (rows.size() * rowHeight);
        if (auto* stockRows = getChildComponent (0))
            stockRows->setBounds (area);
        for (auto* r : rows)
        {
            auto row = rowArea.removeFromTop (rowHeight);
            row.removeFromLeft (10);
            r->name.setBounds (row.removeFromLeft (120));
            r->unit.setBounds (row.removeFromRight (50));
            auto middle = row.reduced (0, 7);
            r->value.setBounds (middle.removeFromRight (70));
            middle.removeFromLeft (6);
            r->bar.setBounds (middle);
        }
    }

    void refreshReading()
    {
        for (auto* r : rows)
        {
            r->bar.setValue ((double) r->param->getValue(), juce::dontSendNotification);
            r->value.setText (r->param->getCurrentValueAsText(), juce::dontSendNotification);
        }
    }

    int getNumRows() const noexcept                   { return rows.size(); }
    const juce::Label& getValueText (int i = 0) const { return rows[i]->value; }
    const juce::Label& getNameText (int i = 0) const  { return rows[i]->name; }

private:
    struct Row
    {
        juce::AudioProcessorParameter* param = nullptr;
        juce::Label name, unit, value;
        juce::Slider bar { juce::Slider::LinearBar, juce::Slider::NoTextBox };
    };

    void addRow (juce::AudioProcessorParameter* param)
    {
        auto* r = rows.add (new Row());
        r->param = param;
        r->name.setText (param->getName (128), juce::dontSendNotification);
        r->name.setJustificationType (juce::Justification::centredRight);
        r->unit.setText (param->getLabel(), juce::dontSendNotification);
        r->value.setJustificationType (juce::Justification::centred);
        r->bar.setRange (0.0, 1.0);
        r->bar.setEnabled (false);
        for (auto* c : std::initializer_list<juce::Component*> { &r->name, &r->unit, &r->value, &r->bar })
        {
            c->setInterceptsMouseClicks (false, false);
            addAndMakeVisible (c);
        }
    }

    void timerCallback() override { refreshReading(); }

    juce::OwnedArray<Row> rows;
};
