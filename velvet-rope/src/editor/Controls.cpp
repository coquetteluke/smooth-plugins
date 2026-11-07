#include "Controls.h"

namespace ui
{
    namespace
    {
        constexpr float arcStart = juce::MathConstants<float>::pi * 1.25f;
        constexpr float arcEnd   = juce::MathConstants<float>::pi * 2.75f;

        void drawDialBody (juce::Graphics& g, juce::Point<float> centre, float radius, float proportion, juce::Colour accent, bool lit, float arcWidth, float pointerLength)
        {
            const float bodyRadius = radius - arcWidth - 3.5f;

            // A soft shadow, then a glazed cabochon lit from above.
            for (int i = 3; i > 0; --i)
            {
                const float grow = (float) i * 1.4f;
                g.setColour (juce::Colours::black.withAlpha (0.10f));
                g.fillEllipse (centre.x - bodyRadius - grow, centre.y - bodyRadius - grow + 2.0f, (bodyRadius + grow) * 2.0f, (bodyRadius + grow) * 2.0f);
            }

            juce::ColourGradient body (tone::raised, centre.x - bodyRadius * 0.4f, centre.y - bodyRadius,
                                       tone::well, centre.x + bodyRadius * 0.4f, centre.y + bodyRadius, false);
            body.addColour (0.55, tone::lapisDark);
            g.setGradientFill (body);
            g.fillEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f);

            g.setColour (tone::bone.withAlpha (0.14f));
            juce::Path rim;
            rim.addCentredArc (centre.x, centre.y, bodyRadius - 0.5f, bodyRadius - 0.5f, 0.0f, -1.1f, 1.1f, true);
            g.strokePath (rim, juce::PathStrokeType (1.0f));

            // The track and the value arc.
            juce::Path track;
            track.addCentredArc (centre.x, centre.y, radius - arcWidth * 0.5f, radius - arcWidth * 0.5f, 0.0f, arcStart, arcEnd, true);
            g.setColour (juce::Colours::black.withAlpha (0.32f));
            g.strokePath (track, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const float angle = arcStart + proportion * (arcEnd - arcStart);

            if (proportion > 0.001f)
            {
                juce::Path value;
                value.addCentredArc (centre.x, centre.y, radius - arcWidth * 0.5f, radius - arcWidth * 0.5f, 0.0f, arcStart, angle, true);

                if (lit)
                {
                    g.setColour (accent.withAlpha (0.25f));
                    g.strokePath (value, juce::PathStrokeType (arcWidth + 3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }

                g.setColour (accent);
                g.strokePath (value, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // The pointer.
            const auto inner = centre.getPointOnCircumference (bodyRadius - pointerLength, angle);
            const auto outer = centre.getPointOnCircumference (bodyRadius - 2.0f, angle);
            g.setColour (lit ? accent.brighter (0.3f) : accent);
            g.drawLine ({ inner, outer }, 2.2f);
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

        typeBox.setFont (font (Weight::semibold, 20.0f));
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
        setAlpha (isEnabled() ? 1.0f : 0.42f);
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
            case Kind::bigDial:   return b.withSizeKeepingCentre (96, 56);
            case Kind::dial:      return b.withTrimmedLeft (getHeight() + 2).removeFromBottom (getHeight() / 2 + 6);
            case Kind::row:       return b.removeFromRight (60);
            case Kind::footerRow: return b.removeFromRight (82);
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
        const auto area = valueArea().withSizeKeepingCentre (juce::jmin (valueArea().getWidth(), 96), juce::jmin (valueArea().getHeight(), 30));
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
        const bool lit = isMouseOverOrDragging() || forcedHover;

        switch (kind)
        {
            case Kind::bigDial:   paintBigDial (g, lit); break;
            case Kind::dial:      paintDial (g, lit); break;
            case Kind::row:       paintRow (g, lit, 96.0f, 60.0f, 16.5f, 17.0f); break;
            case Kind::footerRow: paintRow (g, lit, 44.0f, 82.0f, 19.0f, 24.0f); break;
            case Kind::field:     paintField (g, lit); break;
        }
    }

    void ParamSlider::paintBigDial (juce::Graphics& g, bool lit)
    {
        const auto b = getLocalBounds().toFloat();
        const float radius = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        const auto centre = b.getCentre();
        const float proportion = (float) valueToProportionOfLength (getValue());

        drawDialBody (g, centre, radius, proportion, accent, lit, 5.0f, 10.0f);

        // Value in the middle of the dial, large enough to read across the room.
        const auto text = valueText();
        const auto number = text.upToFirstOccurrenceOf (" ", false, false);
        const auto suffix = text.fromFirstOccurrenceOf (" ", false, false);
        g.setColour (tone::bone);
        g.setFont (font (Weight::regular, 42.0f));
        g.drawText (number, juce::Rectangle<float> (centre.x - 60.0f, centre.y - 28.0f, 120.0f, 44.0f), juce::Justification::centred, false);
        g.setColour (tone::boneSoft);
        g.setFont (font (Weight::regular, 17.0f));
        g.drawText (suffix, juce::Rectangle<float> (centre.x - 40.0f, centre.y + 14.0f, 80.0f, 20.0f), juce::Justification::centred, false);
    }

    void ParamSlider::paintDial (juce::Graphics& g, bool lit)
    {
        const auto b = getLocalBounds().toFloat();
        const float diameter = b.getHeight();
        const float proportion = (float) valueToProportionOfLength (getValue());
        drawDialBody (g, { diameter * 0.5f, b.getCentreY() }, diameter * 0.5f, proportion, accent, lit, 3.5f, 9.0f);

        const auto text = b.withTrimmedLeft (diameter + 10.0f);
        g.setColour (lit ? tone::bone : tone::boneSoft);
        g.setFont (font (Weight::regular, 17.5f));
        g.drawText (label, text.withTrimmedTop (b.getHeight() * 0.5f - 23.0f).withHeight (20.0f), juce::Justification::centredLeft, false);
        g.setColour (tone::bone);
        g.setFont (font (Weight::semibold, 22.0f));
        g.drawText (valueText(), text.withTrimmedTop (b.getHeight() * 0.5f - 3.0f).withHeight (26.0f), juce::Justification::centredLeft, false);
    }

    void ParamSlider::paintRow (juce::Graphics& g, bool lit, float labelWidth, float valueWidth, float labelSize, float valueSize)
    {
        auto b = getLocalBounds().toFloat();

        g.setColour (lit ? tone::bone : tone::boneSoft);
        g.setFont (font (Weight::regular, labelSize));
        g.drawText (label, b.removeFromLeft (labelWidth), juce::Justification::centredLeft, false);

        g.setColour (tone::bone);
        g.setFont (font (Weight::semibold, valueSize));
        g.drawText (valueText(), b.removeFromRight (valueWidth), juce::Justification::centredRight, false);

        auto track = b.withSizeKeepingCentre (b.getWidth() - 6.0f, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRoundedRectangle (track, 1.5f);

        const float proportion = (float) valueToProportionOfLength (getValue());
        const float x = track.getX() + track.getWidth() * proportion;

        // Fill grows from the middle for a control whose rest position is the middle.
        const float rest = (float) valueToProportionOfLength (param.convertFrom0to1 (param.getDefaultValue()));
        const float origin = track.getX() + track.getWidth() * (getMinimum() < 0.0 && getMaximum() > 0.0 ? rest : 0.0f);
        g.setColour (accent.withAlpha (lit ? 1.0f : 0.88f));
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (origin, x), track.getY(), std::abs (x - origin), track.getHeight()), 1.5f);

        if (lit)
        {
            g.setColour (accent.withAlpha (0.2f));
            g.fillEllipse (x - 11.0f, track.getCentreY() - 11.0f, 22.0f, 22.0f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (x - 7.5f, track.getCentreY() - 6.5f, 15.0f, 15.0f);
        g.setColour (accent);
        g.fillEllipse (x - 6.5f, track.getCentreY() - 6.5f, 13.0f, 13.0f);
    }

    void ParamSlider::paintField (juce::Graphics& g, bool lit)
    {
        auto b = getLocalBounds().toFloat();

        if (lit)
        {
            g.setColour (tone::bone.withAlpha (0.07f));
            g.fillRoundedRectangle (b.reduced (0.0f, 2.0f), 6.0f);
        }

        auto text = b.reduced (10.0f, 6.0f);
        g.setColour (lit ? tone::bone : tone::boneSoft);
        g.setFont (font (Weight::regular, 16.5f));
        g.drawText (label, text.removeFromTop (20.0f), juce::Justification::centredLeft, false);
        g.setColour (tone::bone);
        g.setFont (font (Weight::semibold, 23.0f));
        g.drawText (valueText(), text.removeFromTop (26.0f), juce::Justification::centredLeft, false);
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
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        const bool lit = isOver || isDown || forcedHover;

        if (on)
        {
            g.setColour (onColour);
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (tone::well);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (lit ? 0.14f : 0.26f));
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (tone::bone.withAlpha (lit ? 0.65f : 0.30f));
            g.drawRoundedRectangle (b, 6.0f, 1.0f);
            g.setColour (lit ? tone::bone : tone::boneSoft);
        }

        g.setFont (font (Weight::semibold, 18.0f));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
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
        return getLocalBounds().toFloat().withTrimmedLeft (96.0f).withSizeKeepingCentre ((float) getWidth() - 96.0f, 28.0f);
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
        g.setColour (hovered >= 0 ? tone::bone : tone::boneSoft);
        g.setFont (font (Weight::regular, 16.5f));
        g.drawText (label, b.removeFromLeft (96.0f), juce::Justification::centredLeft, false);

        const auto area = buttonsArea();
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillRoundedRectangle (area, 6.0f);

        const float segment = area.getWidth() / (float) choices.size();

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto cell = juce::Rectangle<float> (area.getX() + segment * (float) i, area.getY(), segment, area.getHeight());

            if (i == selected)
            {
                g.setColour (tone::bone);
                g.fillRoundedRectangle (cell.reduced (2.0f), 4.5f);
            }
            else if (i == hovered)
            {
                g.setColour (tone::bone.withAlpha (0.12f));
                g.fillRoundedRectangle (cell.reduced (2.0f), 4.5f);
            }

            g.setColour (i == selected ? tone::well : tone::boneSoft);
            g.setFont (font (Weight::semibold, 16.5f));
            g.drawText (choices[i], cell, juce::Justification::centred, false);
        }
    }
}
