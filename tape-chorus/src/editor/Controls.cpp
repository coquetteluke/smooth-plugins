#include "Controls.h"

namespace ui
{
    ParamStrip::ParamStrip (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Style styleToUse)
        : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          style (styleToUse)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (false);
        setMouseDragSensitivity (style.size == Size::large ? 260 : 220);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));

        typeBox.setFont (font (Weight::light, style.size == Size::large ? 28.0f : 17.0f));
        typeBox.setJustificationType (juce::Justification::centredLeft);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    ParamStrip::~ParamStrip()
    {
        stopTimer();
    }

    void ParamStrip::setForcedHover (bool shouldShow)
    {
        if (forcedHover != shouldShow)
        {
            forcedHover = shouldShow;
            repaint();
        }
    }

    juce::String ParamStrip::numberText() const
    {
        const auto v = (float) getValue();

        switch (style.unit)
        {
            case Unit::count:   return juce::String (juce::roundToInt (v));
            case Unit::percent: return juce::String (juce::roundToInt (v));
            case Unit::ms:      return juce::String (v, style.decimals);
            case Unit::db:
            {
                const auto rounded = juce::roundToInt (v * 10.0f);
                return (rounded > 0 ? "+" : (rounded < 0 ? minusSign() : juce::String())) + juce::String (std::abs ((float) rounded) * 0.1f, 1);
            }
            case Unit::hz:      return juce::String (v, v < 9.995f ? 2 : 1);
            case Unit::lowCut:  return v <= 20.05f ? "Off" : juce::String (juce::roundToInt (v));
            case Unit::highCut: return v >= 19999.5f ? "Off" : juce::String (v * 0.001f, v >= 9995.0f ? 1 : 2);
        }

        return {};
    }

    juce::String ParamStrip::unitText() const
    {
        const auto v = (float) getValue();

        switch (style.unit)
        {
            case Unit::count:   return {};
            case Unit::percent: return "%";
            case Unit::ms:      return "ms";
            case Unit::db:      return "dB";
            case Unit::hz:      return "Hz";
            case Unit::lowCut:  return v <= 20.05f ? "" : "Hz";
            case Unit::highCut: return v >= 19999.5f ? "" : "kHz";
        }

        return {};
    }

    juce::Rectangle<int> ParamStrip::valueArea() const
    {
        return style.size == Size::large ? juce::Rectangle<int> (0, 20, getWidth(), 38)
                                         : juce::Rectangle<int> (0, 17, getWidth(), 24);
    }

    void ParamStrip::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDown (e);
    }

    void ParamStrip::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && valueArea().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void ParamStrip::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDoubleClick (e);
    }

    void ParamStrip::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void ParamStrip::startTyping()
    {
        const auto area = valueArea();
        typeBox.setBounds (area.withWidth (juce::jmin (area.getWidth(), style.size == Size::large ? 96 : 76)).withX (style.alignRight ? area.getRight() - (style.size == Size::large ? 96 : 76) : 0));
        typeBox.setText (numberText().replace (minusSign(), "-"), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void ParamStrip::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim().toLowerCase().replace (minusSign(), "-");
        double value = text.getDoubleValue();

        if (text == "off")
        {
            if (style.unit == Unit::lowCut)       value = getMinimum();
            else if (style.unit == Unit::highCut) value = getMaximum();
            else return;
        }
        else
        {
            if (! text.containsAnyOf ("0123456789") || ! std::isfinite (value))
                return;

            // "3k" and "3.2 kHz" are the natural ways to type a frequency. High Cut reads in kHz, so a small number there means kHz too.
            if (text.containsChar ('k') || (style.unit == Unit::highCut && value < 21.0))
                value *= 1000.0;
        }

        param.beginChangeGesture();
        setValue (value, juce::sendNotificationSync);
        param.endChangeGesture();
    }

    void ParamStrip::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOverOrDragging() || forcedHover;
        const bool large = style.size == Size::large;
        const auto width = (float) getWidth();
        const auto align = style.alignRight ? juce::Justification::centredRight : juce::Justification::centredLeft;

        g.setColour (lit ? tone::mint : tone::muted);
        g.setFont (font (Weight::medium, large ? 13.0f : 12.0f));
        g.drawText (label, juce::Rectangle<float> (0.0f, 0.0f, width, large ? 18.0f : 15.0f), align, false);

        // The number in large type, the unit small beside it, on one baseline.
        const auto numberFont = font (Weight::light, large ? 31.0f : 19.0f);
        const auto unitFont = font (Weight::medium, large ? 12.0f : 11.0f);
        const auto number = numberText(), unit = unitText();
        const float numberWidth = juce::GlyphArrangement::getStringWidth (numberFont, number);
        const float unitGap = unit == "%" ? 2.0f : 4.0f;
        const float unitWidth = unit.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (unitFont, unit) + unitGap;
        const auto area = valueArea().toFloat();
        const float baseline = large ? area.getY() + 31.0f : area.getY() + 18.0f;
        const float x = style.alignRight ? width - numberWidth - unitWidth : 0.0f;

        g.setColour (tone::mint);
        g.setFont (numberFont);
        g.drawSingleLineText (number, juce::roundToInt (x), juce::roundToInt (baseline));

        if (unit.isNotEmpty())
        {
            g.setColour (tone::muted);
            g.setFont (unitFont);
            g.drawSingleLineText (unit, juce::roundToInt (x + numberWidth + unitGap), juce::roundToInt (baseline));
        }

        // The ruler. Flat, with one short upright mark at the value.
        const float rulerY = large ? 64.0f : 45.0f;
        const float thick = lit ? 3.0f : 2.0f;
        const auto proportion = (float) valueToProportionOfLength (getValue());

        if (style.segments > 0)
        {
            const float gap = 3.0f;
            const float each = (width - gap * (float) (style.segments - 1)) / (float) style.segments;
            const int on = juce::roundToInt (getValue());

            for (int i = 0; i < style.segments; ++i)
            {
                g.setColour (i < on ? tone::mint : tone::track);
                g.fillRect ((each + gap) * (float) i, rulerY, each, thick + (i < on ? 1.0f : 0.0f));
            }
            return;
        }

        g.setColour (tone::track);
        g.fillRect (0.0f, rulerY, width, thick);

        const float at = juce::jlimit (0.0f, width - 2.0f, proportion * width);
        const float from = style.fromCentre ? (float) valueToProportionOfLength (param.convertFrom0to1 (param.getDefaultValue())) * width : 0.0f;
        g.setColour (tone::mint);
        g.fillRect (juce::jmin (from, at), rulerY, std::abs (at - from), thick);
        g.fillRect (at, rulerY - (lit ? 5.0f : 4.0f), 2.0f, thick + (lit ? 10.0f : 8.0f));
    }

    ParamSwitch::ParamSwitch (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind kindToUse)
        : juce::ToggleButton (labelText),
          label (labelText),
          kind (kindToUse)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        attachment = std::make_unique<juce::ButtonParameterAttachment> (*state.getParameter (paramId), *this);
    }

    void ParamSwitch::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        const bool on = getToggleState();
        const auto width = (float) getWidth();

        if (kind == Kind::pill)
        {
            const auto b = getLocalBounds().toFloat();
            g.setColour (on ? tone::mint : tone::mint.withAlpha (isDown ? 0.18f : (isOver ? 0.13f : 0.08f)));
            g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
            g.setColour (on ? tone::base : tone::mint);
            g.setFont (font (Weight::medium, 13.5f));
            g.drawText (label, b, juce::Justification::centred, false);
            return;
        }

        g.setColour (isOver ? tone::mint : tone::muted);
        g.setFont (font (Weight::medium, 12.0f));
        g.drawText (label, juce::Rectangle<float> (0.0f, 0.0f, width, 15.0f), juce::Justification::centredLeft, false);

        g.setColour (tone::mint);
        g.setFont (font (Weight::light, 19.0f));
        g.drawSingleLineText (on ? "On" : "Off", 0, 35);

        const float half = (width - 3.0f) * 0.5f, thick = isOver ? 3.0f : 2.0f;
        g.setColour (on ? tone::track : tone::mint);
        g.fillRect (0.0f, 45.0f, half, thick);
        g.setColour (on ? tone::mint : tone::track);
        g.fillRect (half + 3.0f, 45.0f, half, thick);
    }

    Readout::Readout (const juce::String& id, const juce::String& hintText, bool alignRight)
        : right (alignRight)
    {
        hint = hintText;
        setComponentID (id);
        setOpaque (true);
    }

    void Readout::setText (const juce::String& newText, bool hot)
    {
        if (newText != text || hot != isHot)
        {
            text = newText;
            isHot = hot;
            repaint();
        }
    }

    void Readout::paint (juce::Graphics& g)
    {
        g.fillAll (tone::base);
        g.setColour (isHot ? tone::hot : tone::muted);
        g.setFont (font (Weight::medium, 12.0f));
        g.drawText (text, getLocalBounds(), right ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
    }
}
