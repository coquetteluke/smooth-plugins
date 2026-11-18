#include "Controls.h"

namespace ui
{
    namespace
    {
        constexpr float labelSize = 12.5f;
    }

    void drawChevron (juce::Graphics& g, juce::Point<float> c, int direction, juce::Colour colour)
    {
        juce::Path p;
        if (direction == 0)
        {
            p.startNewSubPath (c.x - 4.0f, c.y - 2.0f);
            p.lineTo (c.x, c.y + 2.0f);
            p.lineTo (c.x + 4.0f, c.y - 2.0f);
        }
        else
        {
            const float s = direction == 1 ? 1.0f : -1.0f;
            p.startNewSubPath (c.x - 2.0f * s, c.y - 4.0f);
            p.lineTo (c.x + 2.0f * s, c.y);
            p.lineTo (c.x - 2.0f * s, c.y + 4.0f);
        }
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // ---- value cell -----------------------------------------------------------------------------------------

    juce::TextEditor* ValueCell::TypeBox::createEditorComponent()
    {
        auto* editor = juce::Label::createEditorComponent();
        editor->setJustification (juce::Justification::centredLeft);
        editor->setIndents (3, 1);
        editor->setBorder ({});
        return editor;
    }

    ValueCell::ValueCell (Apvts& s, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Formatter f, Parser p)
        : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox),
          state (s), label (labelText), formatter (std::move (f)), parser (std::move (p))
    {
        hint = hintText;
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (false);
        setMouseDragSensitivity (260);
        setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (false);

        typeBox.setFont (font (Weight::medium, 15.0f, true));
        typeBox.setJustificationType (juce::Justification::centredLeft);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);

        bind (paramId);
    }

    ValueCell::~ValueCell()
    {
        stopTimer();
    }

    void ValueCell::bind (const juce::String& paramId)
    {
        if (param != nullptr && getComponentID() == paramId)
            return;

        attachment.reset();
        param = state.getParameter (paramId);
        jassert (param != nullptr);
        setComponentID (paramId);
        setTitle (param->getName (64));
        restValue = param->convertFrom0to1 (param->getDefaultValue());
        attachment = std::make_unique<juce::SliderParameterAttachment> (*param, *this);
        setDoubleClickReturnValue (true, restValue);
        repaint();
    }

    void ValueCell::setAccent (juce::Colour c)
    {
        if (c != accent)
        {
            accent = c;
            repaint();
        }
    }

    juce::String ValueCell::fullValueText() const
    {
        return formatter ((float) getValue());
    }

    juce::String ValueCell::valueText() const
    {
        // A narrow cell drops the unit before it cuts a digit off. The label above says what the number is.
        const auto text = fullValueText();
        if (getWidth() <= 0 || juce::GlyphArrangement::getStringWidth (font (Weight::medium, valueSize, true), text) <= (float) getWidth())
            return text;
        const auto unit = text.fromLastOccurrenceOf (" ", false, false);
        if (unit == "dB" || unit == "st" || unit == "ms" || unit == "s" || unit == "BPM")
            return text.upToLastOccurrenceOf (" ", false, false);
        return text;
    }

    void ValueCell::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDown (e);
    }

    void ValueCell::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu())
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void ValueCell::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDoubleClick (e);
    }

    void ValueCell::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void ValueCell::startTyping()
    {
        typeBox.setBounds (getLocalBounds().withTrimmedTop (18).withHeight (23));
        typeBox.setText (param->getCurrentValueAsText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void ValueCell::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim().replace (juce::String::fromUTF8 ("\xe2\x88\x92"), "-");   // the minus sign values are shown with

        double value = 0.0;
        if (parser != nullptr)
        {
            bool understood = false;
            value = parser (text, understood);
            if (! understood)
                return;
        }
        else
        {
            // A word that means nothing here must not turn into zero.
            if (! text.containsAnyOf ("0123456789"))
                return;
            value = getValueFromText (text);
        }

        if (! std::isfinite (value))
            return;

        param->beginChangeGesture();
        setValue (juce::jlimit (getMinimum(), getMaximum(), value), juce::sendNotificationSync);
        param->endChangeGesture();
        repaint();
    }

    void ValueCell::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOverOrDragging();
        const auto b = getLocalBounds().toFloat();

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, labelSize));
        g.drawText (label, b.withHeight (18.0f), juce::Justification::centredLeft, false);

        if (! typeBox.isVisible())
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::medium, valueSize, true));
            g.drawText (valueText(), juce::Rectangle<float> (b.getX(), 18.0f, b.getWidth(), 23.0f), juce::Justification::centredLeft, false);
        }

        const auto track = juce::Rectangle<float> (b.getX(), b.getBottom() - 3.0f, b.getWidth(), 3.0f);
        g.setColour (tone::text.withAlpha (lit ? 0.2f : 0.12f));
        g.fillRoundedRectangle (track, 1.5f);

        const float proportion = juce::jlimit (0.0f, 1.0f, (float) valueToProportionOfLength (getValue()));
        const float x = track.getX() + track.getWidth() * proportion;

        // A control whose rest is in the middle fills out from the middle.
        const bool fromMiddle = getMinimum() < 0.0 && getMaximum() > 0.0;
        const float origin = track.getX() + (fromMiddle ? track.getWidth() * (float) valueToProportionOfLength (restValue) : 0.0f);
        const float left = juce::jmin (origin, x), width = juce::jmax (3.0f, std::abs (x - origin));
        g.setColour (accent);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (left, track.getRight() - width), track.getY(), width, track.getHeight()), 1.5f);
    }

    // ---- list -----------------------------------------------------------------------------------------------

    MenuField::MenuField (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, const juce::String& unitText)
        : param (*state.getParameter (paramId)),
          label (labelText),
          unit (unitText),
          choices (dynamic_cast<juce::AudioParameterChoice&> (param).choices),
          attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment.sendInitialUpdate();
    }

    MenuField::~MenuField()
    {
        stopTimer();
    }

    void MenuField::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOver (true);
        auto b = getLocalBounds().toFloat();

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, labelSize));
        g.drawText (label, b.removeFromTop (18.0f), juce::Justification::centredLeft, false);

        auto box = b;
        g.setColour (lit ? tone::hover : tone::raised);
        g.fillRoundedRectangle (box, 6.0f);

        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 14.0f));
        g.drawText (choices[selected] + unit, box.reduced (10.0f, 0.0f).withTrimmedRight (12.0f), juce::Justification::centredLeft, true);

        drawChevron (g, { box.getRight() - 14.0f, box.getCentreY() }, 0, lit ? tone::text : tone::muted);
    }

    void MenuField::mouseUp (const juce::MouseEvent& e)
    {
        // Waits out the second click of a double-click, which resets instead, as a value cell does.
        if (e.getNumberOfClicks() == 1 && getLocalBounds().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void MenuField::mouseDoubleClick (const juce::MouseEvent&)
    {
        stopTimer();
        choose (juce::roundToInt (param.convertFrom0to1 (param.getDefaultValue())));
    }

    void MenuField::timerCallback()
    {
        stopTimer();
        openMenu();
    }

    void MenuField::openMenu()
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());

        for (int i = 0; i < choices.size(); ++i)
            menu.addItem (i + 1, choices[i] + unit, true, i == selected);

        juce::Component::SafePointer<MenuField> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (26),
                            [safe] (int result)
                            {
                                if (safe != nullptr && result > 0)
                                    safe->choose (result - 1);
                            });
    }

    // ---- segments -------------------------------------------------------------------------------------------

    Segments::Segments (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
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

    juce::Rectangle<float> Segments::buttonsArea() const
    {
        return getLocalBounds().toFloat().withTrimmedTop (18.0f);
    }

    int Segments::segmentAt (juce::Point<float> p) const
    {
        const auto area = buttonsArea();

        if (! area.contains (p))
            return -1;

        return juce::jlimit (0, choices.size() - 1, (int) ((p.x - area.getX()) / area.getWidth() * (float) choices.size()));
    }

    juce::Point<float> Segments::centreOf (int index) const
    {
        const auto area = buttonsArea();
        return { area.getX() + area.getWidth() * ((float) index + 0.5f) / (float) choices.size(), area.getCentreY() };
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

        if (hit >= 0 && hit != selected && e.getNumberOfClicks() == 1)
            chooseByHand (hit);
    }

    void Segments::mouseDoubleClick (const juce::MouseEvent&)
    {
        chooseByHand (juce::roundToInt (param.convertFrom0to1 (param.getDefaultValue())));
    }

    void Segments::chooseByHand (int index)
    {
        if (onUserChoice != nullptr && index != selected)
            onUserChoice (index);
        choose (index);
    }

    void Segments::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat();

        g.setColour (hovered >= 0 ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, labelSize));
        g.drawText (label, b.removeFromTop (18.0f), juce::Justification::centredLeft, false);

        const auto area = buttonsArea();
        g.setColour (tone::raised);
        g.fillRoundedRectangle (area, 6.0f);

        const float width = area.getWidth() / (float) choices.size();
        g.setFont (font (Weight::medium, 13.0f));

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto cell = juce::Rectangle<float> (area.getX() + (float) i * width, area.getY(), width, area.getHeight());

            if (i == selected)
            {
                g.setColour (tone::text);
                g.fillRoundedRectangle (cell.reduced (2.0f), 4.5f);
            }
            else if (i == hovered)
            {
                g.setColour (tone::text.withAlpha (0.08f));
                g.fillRoundedRectangle (cell.reduced (2.0f), 4.5f);
            }

            g.setColour (i == selected ? tone::ink : tone::muted);
            g.drawText (choices[i], cell.reduced (2.0f, 0.0f), juce::Justification::centred, false);
        }
    }

    // ---- pill -----------------------------------------------------------------------------------------------

    Pill::Pill (Apvts& s, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : state (s), label (labelText)
    {
        hint = hintText;
        bind (paramId);
    }

    void Pill::bind (const juce::String& paramId)
    {
        if (param != nullptr && getComponentID() == paramId)
            return;

        attachment.reset();
        param = state.getParameter (paramId);
        jassert (param != nullptr);
        setComponentID (paramId);
        setTitle (param->getName (64));
        attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float v) { on = v > 0.5f; repaint(); }, nullptr);
        attachment->sendInitialUpdate();
    }

    void Pill::setAccent (juce::Colour c)
    {
        if (c != accent)
        {
            accent = c;
            repaint();
        }
    }

    void Pill::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOver (true);
        const auto b = getLocalBounds().toFloat();

        g.setColour (on ? accent : (lit ? tone::hover : tone::raised));
        g.fillRoundedRectangle (b, 6.0f);

        g.setColour (on ? tone::ink : (lit ? tone::text : tone::muted));
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (label, b, juce::Justification::centred, false);
    }

    void Pill::mouseDown (const juce::MouseEvent& e)
    {
        if (e.getNumberOfClicks() == 1)
            attachment->setValueAsCompleteGesture (on ? 0.0f : 1.0f);
    }

    void Pill::mouseDoubleClick (const juce::MouseEvent&)
    {
        attachment->setValueAsCompleteGesture (param->convertFrom0to1 (param->getDefaultValue()));
    }

    // ---- plain button ---------------------------------------------------------------------------------------

    ActionButton::ActionButton (const juce::String& labelText, const juce::String& hintText, bool isPrimary)
        : label (labelText), primary (isPrimary)
    {
        hint = hintText;
        setTitle (labelText);
    }

    void ActionButton::setLatched (bool shouldBe)
    {
        if (shouldBe != latched)
        {
            latched = shouldBe;
            repaint();
        }
    }

    void ActionButton::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOver (true);
        const auto b = getLocalBounds().toFloat();
        const bool filled = primary || latched;

        g.setColour (filled ? (lit ? juce::Colours::white : tone::text) : (lit ? tone::hover : tone::raised));
        g.fillRoundedRectangle (b, 6.0f);

        g.setColour (filled ? tone::ink : (lit ? tone::text : tone::muted));
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (label, b, juce::Justification::centred, false);
    }

    void ActionButton::mouseDown (const juce::MouseEvent&)
    {
        if (onClick != nullptr)
            onClick();
    }

    // ---- presets --------------------------------------------------------------------------------------------

    PresetBox::PresetBox (juce::AudioProcessor& p) : processor (p)
    {
        setTitle ("Preset");
        setComponentID ("preset");
        hint = "Presets set how slices are cut and played. They never touch the sample or markers you moved.";
        refresh();
    }

    void PresetBox::refresh()
    {
        const int current = processor.getCurrentProgram();
        const auto now = juce::isPositiveAndBelow (current, processor.getNumPrograms()) ? processor.getProgramName (current) : juce::String ("Choose a preset");

        if (now != shown)
        {
            shown = now;
            repaint();
        }
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

    void PresetBox::choose (int index)
    {
        const int count = processor.getNumPrograms();
        processor.setCurrentProgram ((index % count + count) % count);
        refresh();
    }

    void PresetBox::mouseDown (const juce::MouseEvent& e)
    {
        const auto z = zoneAt (e.position.x);

        const int current = processor.getCurrentProgram();   // -1 for a session that names no preset: the arrows go to the last or the first

        if (z == previous)
            choose (current < 0 ? processor.getNumPrograms() - 1 : current - 1);
        else if (z == next)
            choose (current < 0 ? 0 : current + 1);
        else
        {
            juce::PopupMenu menu;
            menu.setLookAndFeel (&getLookAndFeel());

            for (int i = 0; i < processor.getNumPrograms(); ++i)
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

    void PresetBox::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::raised);
        g.fillRoundedRectangle (b, 6.0f);

        if (zone != none)
        {
            g.setColour (tone::text.withAlpha (0.06f));
            const auto cell = zone == previous ? b.withWidth (arrowWidth)
                            : zone == next     ? b.withLeft (b.getRight() - arrowWidth)
                                               : b.withTrimmedLeft (arrowWidth).withTrimmedRight (arrowWidth);
            g.fillRoundedRectangle (cell, 6.0f);
        }

        drawChevron (g, { arrowWidth * 0.5f, b.getCentreY() }, 2, zone == previous ? tone::text : tone::muted);
        drawChevron (g, { b.getRight() - arrowWidth * 0.5f, b.getCentreY() }, 1, zone == next ? tone::text : tone::muted);

        g.setColour (tone::text);
        g.setFont (font (Weight::regular, 13.5f));
        g.drawText (shown, b.reduced (arrowWidth + 2.0f, 0.0f), juce::Justification::centred, true);
    }

    // ---- a reading ------------------------------------------------------------------------------------------

    ReadingCell::ReadingCell (const juce::String& id, const juce::String& labelText, const juce::String& hintText)
        : label (labelText)
    {
        hint = hintText;
        setComponentID (id);
        setTitle (labelText);
    }

    void ReadingCell::setValueText (const juce::String& t)
    {
        if (t != value)
        {
            value = t;
            repaint();
        }
    }

    void ReadingCell::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, labelSize));
        g.drawText (label, b.removeFromTop (18.0f), juce::Justification::centredLeft, false);
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 16.0f, true));
        g.drawText (value, b.withHeight (24.0f), juce::Justification::centredLeft, false);
    }
}
