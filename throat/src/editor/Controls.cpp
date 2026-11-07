#include "Controls.h"

namespace ui
{
    ParamSlider::ParamSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind kindToUse, Unit unitToUse)
        : juce::Slider (kindToUse == Kind::cell ? juce::Slider::LinearVertical : juce::Slider::LinearHorizontal, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          kind (kindToUse),
          unit (unitToUse)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (false);
        setMouseDragSensitivity (220);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));

        typeBox.setFont (kind == Kind::big ? font (Weight::regular, 36.0f) : font (Weight::medium, 15.0f));
        typeBox.setJustificationType (juce::Justification::centredLeft);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    ParamSlider::~ParamSlider()
    {
        stopTimer();
    }

    void ParamSlider::resized()
    {
        juce::Slider::resized();

        // A drag across the track covers the whole range, so the thumb stays under the mouse.
        if (kind != Kind::cell)
            setMouseDragSensitivity (juce::jmax (60, kind == Kind::row ? getWidth() - (int) rowLabelWidth - 70 : getWidth()));
    }

    void ParamSlider::setForcedHover (bool shouldShow)
    {
        if (forcedHover != shouldShow)
        {
            forcedHover = shouldShow;
            repaint();
        }
    }

    juce::String ParamSlider::valueText() const
    {
        const auto v = (float) getValue();

        switch (unit)
        {
            case Unit::ratio:         return juce::String (v, 2);
            case Unit::percent:       return juce::String (juce::roundToInt (v)) + "%";
            case Unit::signedPercent: return (juce::roundToInt (v) > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + "%";
            case Unit::hz:            return hertzText (v);
            case Unit::signedDb:      return (std::abs (v) < 0.05f ? juce::String ("0.0") : (v > 0.0f ? "+" : "") + juce::String (v, 1)) + " dB";
        }

        return {};
    }

    juce::Rectangle<int> ParamSlider::valueArea() const
    {
        auto b = getLocalBounds();

        switch (kind)
        {
            case Kind::big:  return b.withTrimmedBottom (16).withTrimmedTop (22).withWidth (juce::jmin (150, b.getWidth()));
            case Kind::wide: return b.removeFromRight (90).removeFromTop (getHeight() - 14);
            case Kind::row:  return b.removeFromRight (68);
            case Kind::cell: return b;
        }

        return b;
    }

    void ParamSlider::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
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
        const auto area = valueArea();
        if (kind == Kind::big)
            typeBox.setBounds (area);
        else
            typeBox.setBounds (area.withWidth (juce::jmin (area.getWidth(), 92)).withSizeKeepingCentre (juce::jmin (area.getWidth(), 92), juce::jmin (area.getHeight(), 26)));

        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void ParamSlider::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim().toLowerCase();
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

    void ParamSlider::paintTrack (juce::Graphics& g, juce::Rectangle<float> track, bool lit, bool showRange)
    {
        const auto at = [&] (double v) { return track.getX() + track.getWidth() * (float) valueToProportionOfLength (v); };

        g.setColour (tone::text.withAlpha (lit ? 0.2f : 0.13f));
        g.fillRoundedRectangle (track, track.getHeight() * 0.5f);

        const float rest = at (param.convertFrom0to1 (param.getDefaultValue()));
        const float x = at (getValue());
        const auto centre = track.getCentreY();

        if (showRange)
        {
            // The same gray as the band around the tract.
            const float from = at (0.8), to = at (1.2);
            g.setColour (tone::range.brighter (0.25f));
            g.fillRoundedRectangle (from, centre - 5.0f, to - from, 10.0f, 2.0f);
            g.setColour (tone::muted);
            g.fillRect (rest - 0.5f, centre - 5.0f, 1.0f, 10.0f);
        }

        // Fill grows from the rest position.
        g.setColour (showRange ? tone::oxideLine : tone::text.withAlpha (0.75f));
        g.fillRect (juce::jmin (rest, x), centre - 1.0f, std::abs (x - rest), 2.0f);

        const float thumb = lit ? 6.5f : 5.5f;
        g.setColour (tone::text);
        g.fillEllipse (x - thumb, centre - thumb, thumb * 2.0f, thumb * 2.0f);
    }

    void ParamSlider::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOverOrDragging() || forcedHover;
        auto b = getLocalBounds().toFloat();

        switch (kind)
        {
            case Kind::big:
            {
                auto track = b.removeFromBottom (14.0f).reduced (6.0f, 0.0f).withSizeKeepingCentre (b.getWidth() - 12.0f, 2.0f);
                paintTrack (g, track, lit, true);

                // The number grows with the room it is given, so the column can tighten up when Advanced opens.
                b.removeFromBottom (2.0f);
                auto line = b;
                g.setColour (lit ? tone::text : tone::muted);
                g.setFont (font (Weight::regular, 14.0f));
                g.drawText (label, line.removeFromTop (18.0f), juce::Justification::centredLeft, false);

                const float size = juce::jlimit (34.0f, 54.0f, line.getHeight() * 0.8f);
                g.setColour (tone::text);
                g.setFont (font (Weight::regular, size));
                g.drawText (valueText(), line, juce::Justification::centredLeft, false);

                if (describe != nullptr)
                {
                    g.setColour (tone::muted);
                    g.setFont (font (Weight::regular, 13.0f));
                    // On the number's baseline.
                    const float baseline = line.getCentreY() + size * 0.37f;
                    g.drawText (describe ((float) getValue()), juce::Rectangle<float> (line.getX(), baseline - 16.0f, line.getWidth(), 20.0f), juce::Justification::centredRight, false);
                }
                break;
            }

            case Kind::wide:
            {
                auto track = b.removeFromBottom (14.0f).withSizeKeepingCentre (b.getWidth() - 12.0f, 2.0f);
                paintTrack (g, track, lit, false);
                g.setColour (lit ? tone::text : tone::muted);
                g.setFont (font (Weight::regular, 14.0f));
                g.drawText (label, b, juce::Justification::centredLeft, false);
                g.setColour (tone::text);
                g.setFont (font (Weight::medium, 17.0f));
                g.drawText (valueText(), b, juce::Justification::centredRight, false);
                break;
            }

            case Kind::row:
            {
                g.setColour (lit ? tone::text : tone::muted);
                g.setFont (font (Weight::regular, 13.0f));
                g.drawText (label, b.removeFromLeft (rowLabelWidth), juce::Justification::centredLeft, false);
                g.setColour (tone::text);
                g.drawText (valueText(), b.removeFromRight (62.0f), juce::Justification::centredRight, false);
                paintTrack (g, b.reduced (6.0f, 0.0f).withSizeKeepingCentre (b.getWidth() - 12.0f, 2.0f), lit, false);
                break;
            }

            case Kind::cell:
            {
                g.setColour (lit ? tone::oxideLine : tone::text);
                g.setFont (font (Weight::medium, 15.0f));
                g.drawText (valueText(), b, juce::Justification::centredLeft, false);
                break;
            }
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    ParamToggle::ParamToggle (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind kindToUse)
        : juce::ToggleButton (labelText), kind (kindToUse)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        attachment = std::make_unique<juce::ButtonParameterAttachment> (*state.getParameter (paramId), *this);
    }

    void ParamToggle::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        auto b = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const bool lit = isOver || isDown;

        if (kind == Kind::button)
        {
            g.setColour (on ? tone::text : tone::text.withAlpha (lit ? 0.12f : 0.07f));
            g.fillRoundedRectangle (b, 7.0f);
            g.setColour (on ? tone::well : (lit ? tone::text : tone::muted));
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (getButtonText(), b, juce::Justification::centred, false);
            return;
        }

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (getButtonText(), b, juce::Justification::centredLeft, false);

        const auto pill = b.removeFromRight (40.0f).withSizeKeepingCentre (40.0f, 22.0f);
        g.setColour (on ? tone::text : tone::text.withAlpha (lit ? 0.18f : 0.12f));
        g.fillRoundedRectangle (pill, 11.0f);
        g.setColour (on ? tone::well : tone::muted);
        const float knob = 8.0f, cx = on ? pill.getRight() - 11.0f : pill.getX() + 11.0f;
        g.fillEllipse (cx - knob, pill.getCentreY() - knob, knob * 2.0f, knob * 2.0f);
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
        return getLocalBounds().toFloat().withTrimmedLeft (rowLabelWidth).withSizeKeepingCentre ((float) getWidth() - rowLabelWidth, 28.0f);
    }

    juce::Rectangle<float> ParamChoice::cellFor (int index) const
    {
        // They share the width by how long their names are, so a long name is not clipped. Spare room is shared evenly.
        const auto area = buttonsArea();
        const auto f = font (Weight::medium, 11.5f);
        float used = 0.0f;

        for (const auto& choice : choices)
            used += juce::GlyphArrangement::getStringWidth (f, choice);

        const float spare = juce::jmax (0.0f, area.getWidth() - used) / (float) choices.size();
        float x = area.getX();

        for (int i = 0; i < index; ++i)
            x += juce::GlyphArrangement::getStringWidth (f, choices[i]) + spare;

        return { x, area.getY(), juce::GlyphArrangement::getStringWidth (f, choices[index]) + spare, area.getHeight() };
    }

    int ParamChoice::segmentAt (juce::Point<float> p) const
    {
        for (int i = 0; i < choices.size(); ++i)
            if (cellFor (i).contains (p))
                return i;

        return -1;
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
        g.drawText (label, b.removeFromLeft (rowLabelWidth), juce::Justification::centredLeft, false);

        const auto area = buttonsArea();
        g.setColour (tone::text.withAlpha (0.06f));
        g.fillRoundedRectangle (area, 7.0f);

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto cell = cellFor (i);

            if (i == selected)
            {
                g.setColour (tone::text.withAlpha (0.15f));
                g.fillRoundedRectangle (cell.reduced (2.0f), 5.5f);
            }

            g.setColour (i == selected || i == hovered ? tone::text : tone::muted);
            g.setFont (font (Weight::medium, 11.5f));
            g.drawText (choices[i], cell, juce::Justification::centred, false);
        }
    }
}
