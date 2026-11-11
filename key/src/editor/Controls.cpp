#include "Controls.h"

#include "../KeyAnalyser.h"

namespace keyui
{
    namespace
    {
        constexpr float corner = 4.0f;
        constexpr float controlTextSize = 15.0f;
    }

    // ------------------------------------------------------------------------------------------------------------

    Segments::Segments (Apvts& state, const juce::String& paramId, juce::StringArray labelsToUse, std::vector<int> valuesToUse, const juce::String& hintText)
        : labels (std::move (labelsToUse)),
          values (std::move (valuesToUse)),
          param (*state.getParameter (paramId)),
          attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment.sendInitialUpdate();
    }

    int Segments::segmentAt (juce::Point<float> p) const
    {
        if (! getLocalBounds().toFloat().contains (p))
            return -1;

        return juce::jlimit (0, labels.size() - 1, (int) (p.x / (float) getWidth() * (float) labels.size()));
    }

    void Segments::mouseMove (const juce::MouseEvent& e)
    {
        const int now = segmentAt (e.position);

        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void Segments::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    void Segments::mouseDown (const juce::MouseEvent& e)
    {
        const int hit = segmentAt (e.position);

        if (hit >= 0 && values[(size_t) hit] != selected)
            attachment.setValueAsCompleteGesture ((float) values[(size_t) hit]);
    }

    void Segments::mouseDoubleClick (const juce::MouseEvent&)
    {
        attachment.setValueAsCompleteGesture (defaultOf (param));
    }

    void Segments::paint (juce::Graphics& g)
    {
        ++paintCount;
        const auto b = getLocalBounds().toFloat();
        const float segment = b.getWidth() / (float) labels.size();
        const float strength = dimmed ? 0.4f : 1.0f;

        g.setColour (tone::wash.withMultipliedAlpha (strength));
        g.fillRoundedRectangle (b, corner);

        for (int i = 0; i < labels.size(); ++i)
        {
            const auto cell = juce::Rectangle<float> (b.getX() + segment * (float) i, b.getY(), segment, b.getHeight());
            const bool chosen = values[(size_t) i] == selected;

            if (chosen)
            {
                g.setColour (tone::deep.withMultipliedAlpha (strength));
                g.fillRoundedRectangle (cell, corner);
            }

            g.setColour ((chosen || i == hovered ? tone::ink : tone::soft).withMultipliedAlpha (strength));
            drawTextIn (g, labels[i], chosen ? semi : medium, controlTextSize, cell, Align::centre);
        }
    }

    // ------------------------------------------------------------------------------------------------------------

    Pill::Pill (Apvts& state, const juce::String& paramId, const juce::String& label, const juce::String& hintText, bool strongWhenOn)
        : Pill (*state.getParameter (paramId), label, hintText, strongWhenOn)
    {
    }

    Pill::Pill (juce::RangedAudioParameter& parameter, const juce::String& label, const juce::String& hintText, bool strongWhenOn)
        : juce::ToggleButton (label),
          param (parameter),
          strong (strongWhenOn)
    {
        hint = hintText;
        setComponentID (param.getParameterID());
        setTitle (param.getName (64));
        attachment = std::make_unique<juce::ButtonParameterAttachment> (param, *this);
    }

    void Pill::mouseDoubleClick (const juce::MouseEvent&)
    {
        // Both clicks of the double-click have already toggled it, so this is the only change left to make.
        setAsGesture (param, defaultOf (param));
    }

    void Pill::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        ++paintCount;
        const auto b = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const float strength = dimmed ? 0.4f : 1.0f;

        const auto body = on ? (strong ? tone::ink : tone::deep) : (isOver || isDown ? tone::deep.interpolatedWith (tone::wash, 0.5f) : tone::wash);
        const auto words = on ? (strong ? tone::paper : tone::ink) : (isOver || isDown ? tone::ink : tone::soft);

        g.setColour (body.withMultipliedAlpha (strength));
        g.fillRoundedRectangle (b, corner);
        g.setColour (words.withMultipliedAlpha (strength));
        drawTextIn (g, getButtonText(), on ? semi : medium, controlTextSize, b, Align::centre);
    }

    // ------------------------------------------------------------------------------------------------------------

    ResetButton::ResetButton (Apvts& state, const juce::String& hintText, std::function<void()> onPress)
        : juce::Button ("Reset"),
          param (*state.getParameter ("reset")),
          pressed (std::move (onPress))
    {
        hint = hintText;
        setComponentID ("reset");
        setTitle (param.getName (64));
    }

    ResetButton::~ResetButton()
    {
        // The window closed with the button held: let go, or the parameter stays on.
        if (param.getValue() >= 0.5f)
        {
            param.beginChangeGesture();
            param.setValueNotifyingHost (0.0f);
            param.endChangeGesture();
        }
    }

    void ResetButton::buttonStateChanged()
    {
        const bool down = isDown();

        if (down == (param.getValue() >= 0.5f))
            return;

        if (down && pressed != nullptr)
            pressed();

        param.beginChangeGesture();
        param.setValueNotifyingHost (down ? 1.0f : 0.0f);
        param.endChangeGesture();
    }

    void ResetButton::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        ++paintCount;
        const auto b = getLocalBounds().toFloat();

        g.setColour (isDown ? tone::ink : (isOver ? tone::deep : tone::wash));
        g.fillRoundedRectangle (b, corner);
        g.setColour (isDown ? tone::paper : (isOver ? tone::ink : tone::soft));
        drawTextIn (g, getButtonText(), medium, controlTextSize, b, Align::centre);
    }

    // ------------------------------------------------------------------------------------------------------------

    KeyField::KeyField (Apvts& state, const juce::String& hintText, std::function<void (int, int)> onTyped)
        : param (*state.getParameter ("manualKey")),
          typed (std::move (onTyped)),
          attachment (param, [this] (float v) { value = juce::jlimit (0, 11, juce::roundToInt (v)); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID ("manualKey");
        setTitle (param.getName (64));
        setMouseCursor (juce::MouseCursor::IBeamCursor);
        attachment.sendInitialUpdate();

        typeBox.setFont (typingFont (18.0f));
        typeBox.setJustificationType (juce::Justification::centred);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    KeyField::~KeyField()
    {
        stopTimer();
    }

    void KeyField::mouseDown (const juce::MouseEvent&)
    {
        stopTimer();
        dragStartValue = value;
    }

    void KeyField::mouseDrag (const juce::MouseEvent& e)
    {
        const int steps = -e.getDistanceFromDragStartY() / 10;
        const int next = juce::jlimit (0, 11, dragStartValue + steps);

        if (next != value)
            attachment.setValueAsCompleteGesture ((float) next);
    }

    void KeyField::mouseUp (const juce::MouseEvent& e)
    {
        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1)
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void KeyField::mouseDoubleClick (const juce::MouseEvent&)
    {
        stopTimer();
        attachment.setValueAsCompleteGesture (defaultOf (param));
    }

    void KeyField::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
    {
        wheelOwed += wheel.deltaY * 8.0f;

        if (std::abs (wheelOwed) >= 1.0f)
        {
            const int next = juce::jlimit (0, 11, value + (int) wheelOwed);
            wheelOwed -= (float) (int) wheelOwed;

            if (next != value)
                attachment.setValueAsCompleteGesture ((float) next);
        }
    }

    void KeyField::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void KeyField::startTyping()
    {
        typeBox.setBounds (getLocalBounds());
        typeBox.setText (smoothkey::keyNames[value], juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void KeyField::applyTyped (const juce::String& text)
    {
        int key = 0, scale = -1;

        if (parseKey (text, key, scale) && typed != nullptr)
            typed (key, scale);
    }

    void KeyField::paint (juce::Graphics& g)
    {
        ++paintCount;
        const auto b = getLocalBounds().toFloat();
        g.setColour (isMouseOver (true) ? tone::deep : tone::wash);
        g.fillRoundedRectangle (b, corner);
        g.setColour (tone::ink);
        drawTextIn (g, smoothkey::keyNames[value], semi, 17.0f, b, Align::centre);
    }
}
