#include "Controls.h"

namespace ui
{
    namespace
    {
        constexpr float arcStart = juce::MathConstants<float>::pi * 1.25f;
        constexpr float arcEnd   = juce::MathConstants<float>::pi * 2.75f;

        void drawRing (juce::Graphics& g, juce::Point<float> centre, float radius, float proportion, juce::Colour accent, bool lit, float thickness)
        {
            const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            const float r = radius - thickness * 0.5f;

            juce::Path track;
            track.addCentredArc (centre.x, centre.y, r, r, 0.0f, arcStart, arcEnd, true);
            g.setColour (tone::text.withAlpha (lit ? 0.17f : 0.10f));
            g.strokePath (track, stroke);

            if (proportion > 0.002f)
            {
                juce::Path value;
                value.addCentredArc (centre.x, centre.y, r, r, 0.0f, arcStart, arcStart + proportion * (arcEnd - arcStart), true);
                g.setColour (accent.withAlpha (lit ? 1.0f : 0.88f));
                g.strokePath (value, stroke);
            }
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    ParamSlider::ParamSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText,
                              Kind kindToUse, Unit unitToUse, juce::Colour accentColour)
        : juce::Slider (kindToUse == Kind::row || kindToUse == Kind::footerRow ? juce::Slider::LinearHorizontal : juce::Slider::RotaryVerticalDrag,
                        juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          kind (kindToUse),
          unit (unitToUse),
          accent (accentColour)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (false);
        setMouseDragSensitivity (dragPixels());
        setRotaryParameters (arcStart, arcEnd, true);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));

        typeBox.setFont (font (Weight::medium, 15.0f));
        typeBox.setJustificationType (juce::Justification::centred);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    ParamSlider::~ParamSlider()
    {
        stopTimer();
    }

    void ParamSlider::enablementChanged()
    {
        juce::Slider::enablementChanged();
        repaint();
    }

    int ParamSlider::dragPixels() const noexcept
    {
        switch (kind)
        {
            case Kind::bigDial:   return 300;
            case Kind::dial:      return 220;
            case Kind::row:       return 180;
            case Kind::footerRow: return 280;
            case Kind::field:     return 260;
        }

        return 220;
    }

    juce::String ParamSlider::valueText() const
    {
        const auto v = (float) getValue();

        switch (unit)
        {
            case Unit::db:            return juce::String (v, 1) + " dB";
            case Unit::signedDb:      return (std::abs (v) < 0.05f ? juce::String ("0.0") : (v > 0.0f ? "+" : "") + juce::String (v, 1)) + " dB";
            case Unit::percent:       return juce::String (juce::roundToInt (v)) + "%";
            case Unit::signedPercent: return (juce::roundToInt (v) > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + "%";
            case Unit::ms:            return (v < 100.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v))) + " ms";
            case Unit::hz:            return hertzText (v);
            case Unit::octaves:       return juce::String (v, 2) + " oct";
        }

        return {};
    }

    juce::Rectangle<int> ParamSlider::valueArea() const
    {
        auto b = getLocalBounds();

        switch (kind)
        {
            case Kind::bigDial:   return b.withSizeKeepingCentre (96, 60);
            case Kind::dial:      return b.withTrimmedLeft (getHeight() + 6).removeFromBottom (getHeight() / 2 + 4);
            case Kind::row:       return b.removeFromRight (64);
            case Kind::footerRow: return b.removeFromRight (72);
            case Kind::field:     return b;
        }

        return b;
    }

    void ParamSlider::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        // Shift held as the drag starts gives a five times finer drag.
        setMouseDragSensitivity (dragPixels() * (e.mods.isShiftDown() ? 5 : 1));
        juce::Slider::mouseDown (e);
    }

    void ParamSlider::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && valueArea().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void ParamSlider::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDoubleClick (e);
    }

    void ParamSlider::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void ParamSlider::startTyping()
    {
        const auto area = valueArea().withSizeKeepingCentre (juce::jmin (valueArea().getWidth(), 88), juce::jmin (valueArea().getHeight(), 26));
        typeBox.setBounds (area);
        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void ParamSlider::applyTyped (const juce::String& typed)
    {
        auto text = typed.trim().toLowerCase();
        double value = text.getDoubleValue();

        if (! text.containsAnyOf ("0123456789"))
            return;

        // "3k" and "3.2 kHz" are the natural ways to type a frequency.
        if (unit == Unit::hz && text.containsChar ('k'))
            value *= 1000.0;

        if (! std::isfinite (value))
            return;

        param.beginChangeGesture();
        setValue (value, juce::sendNotificationSync);
        param.endChangeGesture();
    }

    void ParamSlider::paint (juce::Graphics& g)
    {
        const bool lit = isEnabled() && (isMouseOverOrDragging() || forcedHover);

        switch (kind)
        {
            case Kind::bigDial:   paintBigDial (g, lit); break;
            case Kind::dial:      paintDial (g, lit); break;
            case Kind::row:       paintRow (g, lit, 84.0f, 64.0f, 13.0f, 13.0f); break;
            case Kind::footerRow: paintRow (g, lit, 40.0f, 72.0f, 14.0f, 18.0f); break;
            case Kind::field:     paintField (g, lit); break;
        }
    }

    void ParamSlider::paintBigDial (juce::Graphics& g, bool lit)
    {
        const auto b = getLocalBounds().toFloat();
        const float radius = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        const auto centre = b.getCentre();
        const float proportion = (float) valueToProportionOfLength (getValue());

        drawRing (g, centre, radius, proportion, accent, lit, 3.0f);

        // The number in the middle, its unit under it, and the name in the ring's open bottom.
        const float scale = radius / 84.0f;
        const auto text = valueText();
        g.setColour (tone::text);
        g.setFont (font (Weight::regular, 48.0f * scale));
        g.drawText (text.upToFirstOccurrenceOf (" ", false, false),
                    juce::Rectangle<float> (centre.x - 76.0f * scale, centre.y - 38.0f * scale, 152.0f * scale, 62.0f * scale), juce::Justification::centred, false);
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (text.fromFirstOccurrenceOf (" ", false, false),
                    juce::Rectangle<float> (centre.x - 40.0f, centre.y + 20.0f * scale, 80.0f, 18.0f), juce::Justification::centred, false);
        g.setColour (lit ? tone::text : tone::muted);
        g.drawText (label, juce::Rectangle<float> (centre.x - 40.0f, b.getBottom() - 22.0f, 80.0f, 18.0f), juce::Justification::centred, false);
    }

    void ParamSlider::paintDial (juce::Graphics& g, bool lit)
    {
        const auto b = getLocalBounds().toFloat();
        const float diameter = b.getHeight();
        const float proportion = (float) valueToProportionOfLength (getValue());
        drawRing (g, { diameter * 0.5f, b.getCentreY() }, diameter * 0.5f, proportion, accent, lit, 2.5f);

        const auto text = b.withTrimmedLeft (diameter + 12.0f);
        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, text.withY (b.getCentreY() - 21.0f).withHeight (18.0f), juce::Justification::centredLeft, false);
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 17.0f));
        g.drawText (valueText(), text.withY (b.getCentreY() - 2.0f).withHeight (24.0f), juce::Justification::centredLeft, false);
    }

    void ParamSlider::paintRow (juce::Graphics& g, bool lit, float labelWidth, float valueWidth, float labelSize, float valueSize)
    {
        auto b = getLocalBounds().toFloat();
        const float strength = isEnabled() ? 1.0f : 0.4f;

        g.setColour ((lit ? tone::text : tone::muted).withMultipliedAlpha (strength));
        g.setFont (font (Weight::regular, labelSize));
        g.drawText (label, b.removeFromLeft (labelWidth), juce::Justification::centredLeft, false);

        g.setColour (tone::text.withMultipliedAlpha (strength));
        g.setFont (font (valueSize > 15.0f ? Weight::medium : Weight::regular, valueSize));
        g.drawText (valueText(), b.removeFromRight (valueWidth), juce::Justification::centredRight, false);

        auto track = b.withSizeKeepingCentre (b.getWidth() - 12.0f, 2.0f);
        g.setColour (tone::text.withAlpha ((lit ? 0.2f : 0.13f) * strength));
        g.fillRoundedRectangle (track, 1.0f);

        const float proportion = (float) valueToProportionOfLength (getValue());
        const float x = track.getX() + track.getWidth() * proportion;

        // Fill grows from the middle for a control whose rest position is the middle.
        const float rest = (float) valueToProportionOfLength (param.convertFrom0to1 (param.getDefaultValue()));
        const float origin = track.getX() + track.getWidth() * (getMinimum() < 0.0 && getMaximum() > 0.0 ? rest : 0.0f);
        g.setColour (accent.withAlpha (0.8f * strength));
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (origin, x), track.getY(), std::abs (x - origin), track.getHeight()), 1.0f);

        const float thumb = lit ? 6.0f : 5.0f;
        g.setColour (accent.withMultipliedAlpha (strength));
        g.fillEllipse (x - thumb, track.getCentreY() - thumb, thumb * 2.0f, thumb * 2.0f);
    }

    void ParamSlider::paintField (juce::Graphics& g, bool lit)
    {
        auto text = getLocalBounds().toFloat().reduced (0.0f, 6.0f);

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        const auto side = rightAligned ? juce::Justification::centredRight : juce::Justification::centredLeft;
        g.drawText (label, text.removeFromTop (18.0f), side, false);
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 17.0f));
        g.drawText (valueText(), text.removeFromTop (24.0f), side, false);
    }

    // ----------------------------------------------------------------------------------------------------------

    ParamToggle::ParamToggle (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour onColourToUse)
        : juce::ToggleButton (labelText), onColour (onColourToUse)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        attachment = std::make_unique<juce::ButtonParameterAttachment> (*state.getParameter (paramId), *this);
    }

    void ParamToggle::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        const auto b = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const bool lit = isOver || isDown || forcedHover;

        g.setColour (on ? onColour : tone::text.withAlpha (lit ? 0.11f : 0.06f));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (on ? tone::well : (lit ? tone::text : tone::muted));
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (getButtonText(), b, juce::Justification::centred, false);
    }

    // ----------------------------------------------------------------------------------------------------------

    ParamChoice::ParamChoice (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : label (labelText),
          choices (dynamic_cast<juce::AudioParameterChoice&> (*state.getParameter (paramId)).choices),
          attachment (*state.getParameter (paramId), [this] (float v) { selected = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        attachment.sendInitialUpdate();
    }

    juce::Rectangle<float> ParamChoice::buttonsArea() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft (84.0f).withSizeKeepingCentre ((float) getWidth() - 84.0f, 28.0f);
    }

    int ParamChoice::segmentAt (juce::Point<float> p) const
    {
        const auto area = buttonsArea();

        if (! area.contains (p))
            return -1;

        return juce::jlimit (0, choices.size() - 1, (int) ((p.x - area.getX()) / area.getWidth() * (float) choices.size()));
    }

    void ParamChoice::mouseMove (const juce::MouseEvent& e)
    {
        const int now = segmentAt (e.position);

        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void ParamChoice::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    void ParamChoice::mouseDown (const juce::MouseEvent& e)
    {
        const int hit = segmentAt (e.position);

        if (hit >= 0 && hit != selected)
            attachment.setValueAsCompleteGesture ((float) hit);
    }

    void ParamChoice::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (hovered >= 0 ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, b.removeFromLeft (84.0f), juce::Justification::centredLeft, false);

        const auto area = buttonsArea();
        g.setColour (tone::text.withAlpha (0.06f));
        g.fillRoundedRectangle (area, 7.0f);

        const float segment = area.getWidth() / (float) choices.size();

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto cell = juce::Rectangle<float> (area.getX() + segment * (float) i, area.getY(), segment, area.getHeight());

            if (i == selected)
            {
                g.setColour (tone::text.withAlpha (0.14f));
                g.fillRoundedRectangle (cell.reduced (2.0f), 5.5f);
            }

            g.setColour (i == selected || i == hovered ? tone::text : tone::muted);
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (choices[i], cell, juce::Justification::centred, false);
        }
    }
}
