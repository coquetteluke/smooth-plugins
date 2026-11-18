#include "Controls.h"

namespace ui
{
    juce::String cleanTyped (const juce::String& typed)
    {
        const auto text = typed.trim().replace (juce::String::fromUTF8 ("\xe2\x88\x92"), "-");   // the minus sign the value is shown with
        return text.containsAnyOf ("0123456789") ? text : juce::String();
    }

    // ---- slider ---------------------------------------------------------------------------------------------

    juce::TextEditor* DotSlider::TypeBox::createEditorComponent()
    {
        auto* editor = juce::Label::createEditorComponent();
        editor->setJustification (juce::Justification::centred);
        editor->setIndents (0, 3);
        editor->setBorder ({});
        return editor;
    }

    DotSlider::DotSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour, Formatter f)
        : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          accent (accentColour),
          formatter (std::move (f)),
          restValue (param.convertFrom0to1 (param.getDefaultValue()))
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (false);
        setMouseClickGrabsKeyboardFocus (false);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, restValue);

        typeBox.setFont (font (Weight::medium, 16.0f));
        typeBox.setJustificationType (juce::Justification::centred);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    DotSlider::~DotSlider()
    {
        stopTimer();
    }

    void DotSlider::setResting (bool shouldRest)
    {
        if (shouldRest != resting)
        {
            resting = shouldRest;
            repaint();
        }
    }

    juce::String DotSlider::valueText() const
    {
        return formatter ((float) getValue());
    }

    juce::Rectangle<int> DotSlider::valueArea() const
    {
        return getLocalBounds().removeFromTop (labelRow).removeFromRight (juce::jmin (96, getWidth() / 2));
    }

    void DotSlider::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDown (e);
    }

    void DotSlider::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && valueArea().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void DotSlider::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDoubleClick (e);
    }

    void DotSlider::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void DotSlider::startTyping()
    {
        typeBox.setBounds (valueArea().withTrimmedTop (2).withTrimmedBottom (2));
        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void DotSlider::applyTyped (const juce::String& typed)
    {
        const auto text = cleanTyped (typed);
        if (text.isEmpty())
            return;

        // A note name is a word with a digit in it, and the parameter knows how to read those.
        const double value = param.convertFrom0to1 (param.getValueForText (text));
        if (! std::isfinite (value))
            return;

        param.beginChangeGesture();
        setValue (juce::jlimit (getMinimum(), getMaximum(), value), juce::sendNotificationSync);
        param.endChangeGesture();
        repaint();
    }

    void DotSlider::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOverOrDragging() || forcedHover;
        auto b = getLocalBounds().toFloat();
        auto top = b.removeFromTop ((float) labelRow);

        const auto value = valueText();
        const auto row = rowFor (value);
        const float valueWidth = juce::GlyphArrangement::getStringWidth (font (Weight::medium, row.valueSize, true), value);
        g.setColour (lit ? tone::text : (resting ? tone::faint : tone::muted));
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (row.name, top.withTrimmedRight (typeBox.isVisible() ? 0.0f : valueWidth + 6.0f), juce::Justification::centredLeft, true);

        if (! typeBox.isVisible())
        {
            g.setColour (resting && ! lit ? tone::muted : tone::text);
            g.setFont (font (Weight::medium, row.valueSize, true));
            g.drawText (value, top, juce::Justification::centredRight, false);
        }

        constexpr float thumbRadius = 7.0f;
        const auto line = b.withHeight (4.0f).withCentre (b.getCentre()).reduced (thumbRadius + 1.0f, 0.0f);
        g.setColour (tone::text.withAlpha (lit ? 0.26f : 0.17f));
        g.fillRoundedRectangle (line.expanded (thumbRadius - 2.0f, 0.0f), 2.0f);

        const float proportion = (float) valueToProportionOfLength (getValue());
        const float x = line.getX() + line.getWidth() * proportion;

        // A control whose rest is in the middle fills out from the middle.
        const float origin = line.getX() + (getMinimum() < 0.0 && getMaximum() > 0.0 ? line.getWidth() * (float) valueToProportionOfLength (restValue) : 0.0f);
        // Acid means the voice is being changed, so at rest even an acid control is plain.
        const bool atRest = std::abs (getValue() - restValue) < 1.0e-3 * (getMaximum() - getMinimum());
        const auto colour = (resting && ! lit) ? tone::muted : (accent == tone::acid && atRest ? tone::text : accent);
        g.setColour (colour);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (origin, x), line.getY(), std::abs (x - origin), line.getHeight()), 2.0f);

        const float radius = lit ? thumbRadius + 1.0f : thumbRadius;
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre ({ x, line.getCentreY() }));
    }

    DotSlider::Row DotSlider::rowFor (const juce::String& value) const
    {
        const auto need = [&value] (const juce::String& name, float size)
        {
            return juce::GlyphArrangement::getStringWidth (font (Weight::regular, 13.0f), name) + 6.0f
                   + juce::GlyphArrangement::getStringWidth (font (Weight::medium, size, true), value);
        };
        const float room = (float) getWidth();
        for (const auto& name : { label, shortLabel })
        {
            if (name.isEmpty())
                continue;
            for (float size = 18.0f; size >= 13.0f; size -= 0.5f)
                if (need (name, size) <= room)
                    return { name, size, need (name, size) - room };
        }
        // Nothing fits whole: the smallest value, and the name cut short with an ellipsis in what is left, so they still do not overlap.
        return { shortLabel.isNotEmpty() ? shortLabel : label, 13.0f, 0.0f };
    }

    float DotSlider::labelValueOverlap() const
    {
        return rowFor (valueText()).over;
    }

    // ---- the big value --------------------------------------------------------------------------------------

    ValueField::ValueField (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : param (*state.getParameter (paramId)),
          label (labelText),
          attachment (param, [this] (float v) { value = v; repaint(); }, nullptr)
    {
        hint = hintText;
        setTitle (labelText);
        setComponentID (paramId + "Value");
        attachment.sendInitialUpdate();

        typeBox.setFont (font (Weight::medium, 20.0f));
        typeBox.setJustificationType (juce::Justification::centredLeft);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); repaint(); };
        addChildComponent (typeBox);
    }

    ValueField::~ValueField()
    {
        stopTimer();
    }

    juce::String ValueField::valueText() const
    {
        return signedText (value, 2, " st");
    }

    void ValueField::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOver (true);
        auto b = getLocalBounds().toFloat();
        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, b.removeFromTop (18.0f), juce::Justification::centredLeft, false);

        if (! typeBox.isVisible())
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::medium, 26.0f, true));
            g.drawText (valueText(), b, juce::Justification::centredLeft, false);
        }
    }

    void ValueField::mouseUp (const juce::MouseEvent& e)
    {
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && getLocalBounds().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void ValueField::mouseDoubleClick (const juce::MouseEvent&)
    {
        stopTimer();
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

    void ValueField::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void ValueField::startTyping()
    {
        typeBox.setBounds (getLocalBounds().withTrimmedTop (20).withWidth (juce::jmin (getWidth(), 110)));
        typeBox.setText (juce::String (std::abs (value) < 0.005f ? 0.0f : value, 2), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
        repaint();
    }

    void ValueField::applyTyped (const juce::String& typed)
    {
        const auto text = cleanTyped (typed);
        if (text.isEmpty())
            return;

        const auto range = param.getNormalisableRange();
        attachment.setValueAsCompleteGesture (juce::jlimit (range.start, range.end, text.getFloatValue()));
    }

    // ---- chips ----------------------------------------------------------------------------------------------

    Chips::Chips (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
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

    juce::Rectangle<float> Chips::chipBounds (int index) const
    {
        constexpr float gap = 6.0f;
        const auto area = getLocalBounds().toFloat().withTrimmedTop (22.0f);
        const float width = (area.getWidth() - gap * (float) (choices.size() - 1)) / (float) choices.size();
        return { area.getX() + (float) index * (width + gap), area.getY(), width, area.getHeight() };
    }

    int Chips::chipAt (juce::Point<float> p) const
    {
        for (int i = 0; i < choices.size(); ++i)
            if (chipBounds (i).contains (p))
                return i;
        return -1;
    }

    void Chips::mouseMove (const juce::MouseEvent& e)
    {
        const int now = chipAt (e.position);

        if (now != hovered)
        {
            hovered = now;
            repaint();
            if (onHover != nullptr)
                onHover (hovered);
        }
    }

    void Chips::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
        if (onHover != nullptr)
            onHover (-1);
    }

    void Chips::mouseDown (const juce::MouseEvent& e)
    {
        const int hit = chipAt (e.position);

        if (hit >= 0 && hit != selected && e.getNumberOfClicks() == 1)
            choose (hit);
    }

    void Chips::mouseDoubleClick (const juce::MouseEvent&)
    {
        choose (juce::roundToInt (param.convertFrom0to1 (param.getDefaultValue())));
    }

    void Chips::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat();

        g.setColour (hovered >= 0 ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, b.removeFromTop (20.0f), juce::Justification::centredLeft, false);

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto chip = chipBounds (i);
            const float radius = chip.getHeight() * 0.5f;

            g.setColour (i == selected ? tone::acid : tone::text.withAlpha (i == hovered ? 0.2f : 0.11f));
            g.fillRoundedRectangle (chip, radius);

            g.setColour (i == selected ? tone::well : tone::text);
            g.setFont (font (Weight::medium, 14.0f));
            g.drawText (choices[i], chip, juce::Justification::centred, false);
        }
    }

    // ---- pill -----------------------------------------------------------------------------------------------

    Pill::Pill (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : param (*state.getParameter (paramId)),
          label (labelText),
          attachment (param, [this] (float v) { on = v >= 0.5f; repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment.sendInitialUpdate();
    }

    void Pill::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        const bool lit = isMouseOver (true);

        g.setColour (on ? tone::text : tone::text.withAlpha (lit ? 0.2f : 0.11f));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        g.setColour (on ? tone::well : tone::text);
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (label, b, juce::Justification::centred, false);
    }

    void Pill::mouseDown (const juce::MouseEvent& e)
    {
        if (e.getNumberOfClicks() == 1)
            setOn (! on);
    }

    void Pill::mouseDoubleClick (const juce::MouseEvent&)
    {
        setOn (param.getDefaultValue() >= 0.5f);
    }

    // ---- presets --------------------------------------------------------------------------------------------

    PresetBox::PresetBox (juce::AudioProcessor& p) : processor (p)
    {
        hint = "Ready-made voices. Click the name for the list.";
        setComponentID ("presets");
        setTitle ("Presets");
    }

    void PresetBox::refresh()
    {
        const auto now = nameNow != nullptr ? nameNow() : processor.getProgramName (processor.getCurrentProgram());

        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    void PresetBox::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::text.withAlpha (zone != none ? 0.2f : 0.11f));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);

        g.setColour (processor.getCurrentProgram() < 0 ? tone::muted : tone::text);   // "Choose a preset" is quieter than a name
        g.setFont (font (Weight::medium, 14.0f));
        g.drawText (shown, b.reduced (arrowWidth, 0.0f), juce::Justification::centred, true);

        const auto chevron = [&] (float cx, float direction, bool lit)
        {
            juce::Path p;
            p.startNewSubPath (cx - 2.5f * direction, b.getCentreY() - 5.0f);
            p.lineTo (cx + 2.5f * direction, b.getCentreY());
            p.lineTo (cx - 2.5f * direction, b.getCentreY() + 5.0f);
            g.setColour (lit ? tone::text : tone::muted);
            g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        chevron (b.getX() + arrowWidth * 0.5f + 2.0f, -1.0f, zone == previous);
        chevron (b.getRight() - arrowWidth * 0.5f - 2.0f, 1.0f, zone == next);
    }

    PresetBox::Zone PresetBox::zoneAt (float x) const
    {
        if (x < arrowWidth)                       return previous;
        if (x > (float) getWidth() - arrowWidth)  return next;
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

    void PresetBox::mouseMove (const juce::MouseEvent& e)  { setZone (zoneAt (e.position.x)); }
    void PresetBox::mouseExit (const juce::MouseEvent&)    { setZone (none); }

    void PresetBox::mouseDown (const juce::MouseEvent& e)
    {
        const auto hit = zoneAt (e.position.x);
        const int count = processor.getNumPrograms();

        const int now = processor.getCurrentProgram();   // -1 when the session names no preset: the arrows go to the last or the first
        if (hit == previous)
        {
            choose (now < 0 ? count - 1 : (now + count - 1) % count);
        }
        else if (hit == next)
        {
            choose (now < 0 ? 0 : (now + 1) % count);
        }
        else
        {
            juce::PopupMenu menu;
            menu.setLookAndFeel (&getLookAndFeel());

            for (int i = 0; i < count; ++i)
                menu.addItem (i + 1, processor.getProgramName (i), true, i == processor.getCurrentProgram());

            juce::Component::SafePointer<PresetBox> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (26),
                                [safe] (int result)
                                {
                                    if (safe != nullptr && result > 0)
                                        safe->choose (result - 1);
                                });
        }
    }

    void PresetBox::choose (int index)
    {
        processor.setCurrentProgram (index);
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withProgramChanged (true));
        refresh();
    }
}
