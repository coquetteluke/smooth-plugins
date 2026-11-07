#include "Controls.h"

namespace fxui
{
    // ------------------------------------------------------------------------------------------------------- slider

    juce::TextEditor* ParamSlider::TypeBox::createEditorComponent()
    {
        auto* editor = juce::Label::createEditorComponent();
        editor->setJustification (juce::Justification::centredLeft);
        editor->setIndents (6, 3);
        editor->setBorder ({});
        return editor;
    }

    ParamSlider::ParamSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Kind kindToUse, juce::Colour accentColour)
        : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          kind (kindToUse),
          accent (accentColour)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (false);
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));

        typeBox.setFont (font (Weight::medium, kind == Kind::lead ? 20.0f : 15.0f));
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
        // One to one: a drag across the bar moves the value across the bar.
        setMouseDragSensitivity (juce::jmax (40, (int) trackArea().getWidth()));
    }

    juce::Rectangle<float> ParamSlider::trackArea() const
    {
        auto b = getLocalBounds().toFloat();

        switch (kind)
        {
            case Kind::lead:      return b.removeFromBottom (14.0f);
            case Kind::main:      return b.removeFromBottom (10.0f);
            case Kind::inlineBar: return b.withTrimmedLeft (88.0f).withTrimmedRight (72.0f).withSizeKeepingCentre (b.getWidth() - 160.0f, 8.0f);
        }

        return b;
    }

    juce::Rectangle<int> ParamSlider::valueArea() const
    {
        auto b = getLocalBounds();

        switch (kind)
        {
            case Kind::lead:      return b.withTrimmedTop (18).withTrimmedBottom (20).withWidth (juce::jmin (getWidth(), 170));
            case Kind::main:      return b.withTrimmedTop (18).withTrimmedBottom (14);
            case Kind::inlineBar: return b.removeFromRight (66);
        }

        return b;
    }

    juce::String ParamSlider::valueText() const
    {
        const auto v = (float) getValue();
        auto text = param.getText (param.convertTo0to1 (v), 0);

        // A control that rests in the middle shows its sign.
        if (getMinimum() < 0.0 && text.getFloatValue() > 0.0f && ! text.startsWithChar ('+'))
            text = "+" + text;

        return text;
    }

    juce::String ParamSlider::unitText() const
    {
        if (valueText() == "Instant")
            return {};

        const auto unit = param.getLabel();
        const bool one = std::abs (getValue() - 1.0) < 0.001 || std::abs (getValue() + 1.0) < 0.001;
        if (unit == "st")  return one ? "semitone" : "semitones";
        if (unit == "oct") return one ? "octave" : "octaves";
        return unit;
    }

    bool ParamSlider::isChanged() const
    {
        return std::abs (param.getValue() - param.getDefaultValue()) > 0.0005f;
    }

    // In words, for the one control that decides the character of the whole thing.
    juce::String ParamSlider::description() const
    {
        if (kind != Kind::lead || param.getParameterID() != "retune")
            return {};

        const auto v = getValue();
        if (v < 0.05) return "Hard tuned";
        if (v < 15.0) return "Tight";
        if (v < 50.0) return "Natural";
        if (v < 150.0) return "Gentle";
        return "Barely there";
    }

    void ParamSlider::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        // Shift held as the drag starts gives a five times finer drag.
        setMouseDragSensitivity (juce::jmax (40, (int) trackArea().getWidth()) * (e.mods.isShiftDown() ? 5 : 1));
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
        typeBox.setBounds (valueArea().withWidth (juce::jmin (valueArea().getWidth(), kind == Kind::lead ? 150 : 96)));
        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void ParamSlider::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim().toLowerCase();
        double value = text.getDoubleValue();

        if (text.startsWith ("inst"))
            value = 0.0;
        else if (! text.containsAnyOf ("0123456789"))
            return;
        else if (text.endsWith ("k") || text.endsWith ("khz"))
            value *= 1000.0;

        if (! std::isfinite (value))
            return;

        param.beginChangeGesture();
        setValue (juce::jlimit (getMinimum(), getMaximum(), value), juce::sendNotificationSync);
        param.endChangeGesture();
    }

    void ParamSlider::paint (juce::Graphics& g)
    {
        const bool lit = isEnabled() && (isMouseOverOrDragging() || forcedHover);
        auto b = getLocalBounds().toFloat();
        const auto track = trackArea();
        const auto value = valueText();

        if (kind == Kind::inlineBar)
        {
            g.setColour (lit ? tone::white : tone::muted);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (label, b.withWidth (84.0f), juce::Justification::centredLeft, false);

            if (! typeBox.isVisible())
            {
                g.setColour (tone::white);
                g.setFont (font (Weight::medium, 13.5f));
                g.drawText (value + " " + unitText(), valueArea().toFloat(), juce::Justification::centredRight, false);
            }
        }
        else
        {
            const float valueSize = kind == Kind::lead ? 42.0f : 25.0f;
            const float unitSize  = kind == Kind::lead ? 15.0f : 13.0f;

            // The name, then the value set big, then the bar.
            g.setColour (lit ? tone::white : tone::muted);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (label, b.removeFromTop (18.0f), juce::Justification::centredLeft, false);

            const auto valueRow = valueArea().toFloat();
            const auto valueFont = font (Weight::regular, valueSize);
            const float valueWidth = juce::GlyphArrangement::getStringWidth (valueFont, value);

            if (! typeBox.isVisible())
            {
                g.setColour (tone::white);
                g.setFont (valueFont);
                g.drawText (value, valueRow.withWidth (valueWidth + 4.0f), juce::Justification::centredLeft, false);

                g.setColour (tone::muted);
                g.setFont (font (Weight::regular, unitSize));
                const auto after = juce::Rectangle<float> (valueRow.getX() + valueWidth + 6.0f, valueRow.getY(), 110.0f, valueRow.getHeight());
                g.drawText (unitText(), after.translated (0.0f, kind == Kind::lead ? 6.0f : 3.0f), juce::Justification::centredLeft, false);
            }

            if (kind == Kind::lead)
            {
                g.setColour (tone::muted);
                g.setFont (font (Weight::regular, 13.0f));
                g.drawText (description(), juce::Rectangle<float> (0.0f, valueRow.getY(), (float) getWidth(), valueRow.getHeight()).translated (0.0f, 6.0f), juce::Justification::centredRight, false);
            }
        }

        // The bar: a solid block that fills, with a white tick for the value. A control that rests in the middle fills from the middle.
        const float corner = 3.0f;
        g.setColour (tone::white.withAlpha (lit ? 0.2f : 0.13f));
        g.fillRoundedRectangle (track, corner);

        const float tick = 2.0f + (lit ? 1.0f : 0.0f);
        const float travel = track.getWidth() - tick;
        const float x = track.getX() + tick * 0.5f + travel * (float) valueToProportionOfLength (getValue());
        const bool fromMiddle = getMinimum() < 0.0 && getMaximum() > 0.0;
        const float origin = fromMiddle ? track.getX() + tick * 0.5f + travel * (float) valueToProportionOfLength (0.0) : track.getX();
        g.setColour (accent);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (origin, x), track.getY(), std::abs (x - origin), track.getHeight()), corner);

        g.setColour (tone::white);
        g.fillRect (juce::Rectangle<float> (x - tick * 0.5f, track.getY() - 2.0f, tick, track.getHeight() + 4.0f));
    }

    // ------------------------------------------------------------------------------------------------------- toggle

    ParamToggle::ParamToggle (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour, const juce::String& offText)
        : juce::ToggleButton (labelText), param (*state.getParameter (paramId)), offWord (offText), accent (accentColour)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment = std::make_unique<juce::ButtonParameterAttachment> (param, *this);
    }

    bool ParamToggle::isChanged() const
    {
        return std::abs (param.getValue() - param.getDefaultValue()) > 0.5f;
    }

    void ParamToggle::mouseDoubleClick (const juce::MouseEvent&)
    {
        setToggleState (param.getDefaultValue() >= 0.5f, juce::sendNotificationSync);
    }

    void ParamToggle::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        const auto b = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const bool lit = isOver || isDown || forcedHover;

        g.setColour (on ? accent : (lit ? tone::lifted.brighter (0.2f) : tone::lifted));
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (on ? tone::black : (lit ? tone::white : tone::muted));
        g.setFont (font (Weight::medium, 13.5f));
        g.drawText (on || offWord.isEmpty() ? getButtonText() : offWord, b, juce::Justification::centred, false);
    }

    // ----------------------------------------------------------------------------------------------------- segments

    ParamSegments::ParamSegments (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour, int rows)
        : param (*state.getParameter (paramId)),
          label (labelText),
          choices (dynamic_cast<juce::AudioParameterChoice&> (param).choices),
          accent (accentColour),
          numRows (rows),
          attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment.sendInitialUpdate();
    }

    bool ParamSegments::isChanged() const
    {
        return std::abs (param.getValue() - param.getDefaultValue()) > 0.0005f;
    }

    juce::Rectangle<float> ParamSegments::cellArea() const
    {
        auto b = getLocalBounds().toFloat();
        if (label.isNotEmpty())
            b.removeFromTop (22.0f);
        return b;
    }

    juce::Rectangle<float> ParamSegments::cellBounds (int i) const
    {
        const auto b = cellArea();
        const int perRow = (choices.size() + numRows - 1) / numRows;
        const float w = b.getWidth() / (float) perRow, h = b.getHeight() / (float) numRows;
        return { b.getX() + w * (float) (i % perRow), b.getY() + h * (float) (i / perRow), w, h };
    }

    int ParamSegments::segmentAt (juce::Point<float> p) const
    {
        for (int i = 0; i < choices.size(); ++i)
            if (cellBounds (i).contains (p))
                return i;
        return -1;
    }

    void ParamSegments::mouseMove (const juce::MouseEvent& e)
    {
        const int now = segmentAt (e.position);
        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void ParamSegments::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    void ParamSegments::mouseDown (const juce::MouseEvent& e)
    {
        const int hit = segmentAt (e.position);
        if (hit >= 0 && hit != selected)
            attachment.setValueAsCompleteGesture ((float) hit);
    }

    void ParamSegments::mouseDoubleClick (const juce::MouseEvent&)
    {
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

    void ParamSegments::paint (juce::Graphics& g)
    {
        if (label.isNotEmpty())
        {
            g.setColour (hovered >= 0 ? tone::white : tone::muted);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (label, getLocalBounds().toFloat().removeFromTop (18.0f), juce::Justification::centredLeft, false);
        }

        g.setColour (tone::lifted);
        g.fillRoundedRectangle (cellArea(), 5.0f);

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto cell = cellBounds (i);
            if (i == selected)
            {
                g.setColour (accent);
                g.fillRoundedRectangle (cell.reduced (2.0f), 3.5f);
            }
            g.setColour (i == selected ? tone::black : (i == hovered ? tone::white : tone::muted));
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (choices[i], cell, juce::Justification::centred, false);
        }
    }

    // --------------------------------------------------------------------------------------------------------- menu

    ParamMenu::ParamMenu (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : param (*state.getParameter (paramId)),
          label (labelText),
          choices (dynamic_cast<juce::AudioParameterChoice&> (param).choices),
          attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment.sendInitialUpdate();
    }

    bool ParamMenu::isChanged() const
    {
        return std::abs (param.getValue() - param.getDefaultValue()) > 0.0005f;
    }

    void ParamMenu::mouseDown (const juce::MouseEvent&)
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        for (int i = 0; i < choices.size(); ++i)
            menu.addItem (i + 1, choices[i], true, i == selected);

        openedAt = juce::Time::getMillisecondCounter();
        juce::Component::SafePointer<ParamMenu> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()), [safe] (int result)
        {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;
            if (result > 0)
                self->attachment.setValueAsCompleteGesture ((float) (result - 1));
            else if (juce::Time::getMillisecondCounter() - self->openedAt < (juce::uint32) juce::MouseEvent::getDoubleClickTimeout() && self->isMouseOver (true))
                self->attachment.setValueAsCompleteGesture (self->param.convertFrom0to1 (self->param.getDefaultValue()));
        });
    }

    void ParamMenu::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat();
        const bool lit = isMouseOver (true);

        if (label.isNotEmpty())
        {
            g.setColour (lit ? tone::white : tone::muted);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (label, b.removeFromTop (22.0f).withTrimmedBottom (4.0f), juce::Justification::centredLeft, false);
        }

        g.setColour (lit ? tone::lifted.brighter (0.2f) : tone::lifted);
        g.fillRoundedRectangle (b, 5.0f);

        g.setColour (tone::white);
        g.setFont (font (Weight::medium, 13.5f));
        g.drawText (choices[selected], b.withTrimmedLeft (10.0f).withTrimmedRight (22.0f), juce::Justification::centredLeft, true);

        juce::Path chevron;
        const float cx = b.getRight() - 13.0f, cy = b.getCentreY();
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (lit ? tone::white : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // ------------------------------------------------------------------------------------------------------- preset

    PresetBox::PresetBox (juce::AudioProcessor& p) : processor (p)
    {
        hint = "Starting points. Click the name for the full list.";
        setComponentID ("presets");
        setTitle ("Presets");
    }

    void PresetBox::refresh()
    {
        if (processor.getCurrentProgram() != shown)
        {
            shown = processor.getCurrentProgram();
            repaint();
        }
    }

    PresetBox::Zone PresetBox::zoneAt (float x) const
    {
        if (x < arrowWidth)                      return previous;
        if (x > (float) getWidth() - arrowWidth) return next;
        return name;
    }

    void PresetBox::setZone (Zone z)
    {
        if (z != zone)
        {
            zone = z;
            repaint();
        }
    }

    void PresetBox::choose (int index)
    {
        processor.setCurrentProgram (index);
        refresh();
    }

    void PresetBox::mouseDown (const juce::MouseEvent& e)
    {
        const auto hit = zoneAt (e.position.x);
        const int count = processor.getNumPrograms();

        if (hit == previous)
            choose ((processor.getCurrentProgram() + count - 1) % count);
        else if (hit == next)
            choose ((processor.getCurrentProgram() + 1) % count);
        else
        {
            juce::PopupMenu menu;
            menu.setLookAndFeel (&getLookAndFeel());
            for (int i = 0; i < count; ++i)
                menu.addItem (i + 1, processor.getProgramName (i), true, i == processor.getCurrentProgram());

            juce::Component::SafePointer<PresetBox> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()), [safe] (int result)
            {
                if (result > 0 && safe != nullptr)
                    safe->choose (result - 1);
            });
        }
    }

    void PresetBox::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (zone == name ? tone::lifted.brighter (0.2f) : tone::lifted);
        g.fillRoundedRectangle (b, 5.0f);

        g.setColour (tone::white);
        g.setFont (font (Weight::regular, 13.5f));
        g.drawText (processor.getProgramName (processor.getCurrentProgram()), b.reduced (arrowWidth, 0.0f), juce::Justification::centred, true);

        const auto chevron = [&] (float cx, float direction, bool lit)
        {
            juce::Path p;
            p.startNewSubPath (cx - 2.5f * direction, b.getCentreY() - 5.0f);
            p.lineTo (cx + 2.5f * direction, b.getCentreY());
            p.lineTo (cx - 2.5f * direction, b.getCentreY() + 5.0f);
            g.setColour (lit ? tone::white : tone::muted);
            g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        chevron (b.getX() + arrowWidth * 0.5f, -1.0f, zone == previous);
        chevron (b.getRight() - arrowWidth * 0.5f, 1.0f, zone == next);
    }
}
