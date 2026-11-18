#include "Controls.h"

namespace ui
{
    namespace
    {
        constexpr int menuDelayMs = 200;   // long enough to see the second half of a double-click, short enough not to feel late
    }

    // ---- bar ------------------------------------------------------------------------------------------------

    juce::TextEditor* BarSlider::TypeBox::createEditorComponent()
    {
        auto* editor = juce::Label::createEditorComponent();
        editor->setJustification (juce::Justification::centred);
        editor->setIndents (0, 3);
        editor->setBorder ({});
        return editor;
    }

    BarSlider::BarSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour, Formatter f)
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

    BarSlider::~BarSlider()
    {
        stopTimer();
    }

    juce::String BarSlider::valueText() const
    {
        return formatter ((float) getValue());
    }

    juce::Rectangle<int> BarSlider::valueArea() const
    {
        return getLocalBounds().removeFromTop (28).removeFromRight (juce::jmin (valueSize < 16.0f ? 72 : 96, getWidth() / 2));
    }

    void BarSlider::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDown (e);
    }

    void BarSlider::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && valueArea().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void BarSlider::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDoubleClick (e);
    }

    void BarSlider::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void BarSlider::startTyping()
    {
        typeBox.setBounds (valueArea().withTrimmedTop (2).withTrimmedBottom (2));
        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void BarSlider::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim().replace (juce::String::fromUTF8 ("\xe2\x88\x92"), "-");   // the minus sign the value is shown with

        // A word that means nothing here must not turn into zero. "Instant" is the one word a parameter understands.
        if (! text.containsAnyOf ("0123456789") && ! text.equalsIgnoreCase ("instant"))
            return;

        const double value = getValueFromText (text);

        if (! std::isfinite (value))
            return;

        param.beginChangeGesture();
        setValue (juce::jlimit (getMinimum(), getMaximum(), value), juce::sendNotificationSync);
        param.endChangeGesture();
        repaint();
    }

    void BarSlider::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOverOrDragging() || forcedHover;
        auto b = getLocalBounds().toFloat();
        auto top = b.removeFromTop (28.0f);

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, top, juce::Justification::centredLeft, false);

        if (! typeBox.isVisible())
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::medium, valueSize, true));
            g.drawText (valueText(), top, juce::Justification::centredRight, false);
        }

        const auto track = b.withHeight (5.0f).withCentre ({ b.getCentreX(), b.getY() + 9.0f });
        g.setColour (tone::text.withAlpha (lit ? 0.2f : 0.13f));
        g.fillRoundedRectangle (track, 2.5f);

        const float proportion = (float) valueToProportionOfLength (getValue());
        const float x = track.getX() + track.getWidth() * proportion;

        // A control whose rest is in the middle fills out from the middle.
        const float origin = track.getX() + (getMinimum() < 0.0 && getMaximum() > 0.0 ? track.getWidth() * (float) valueToProportionOfLength (restValue) : 0.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (origin, x), track.getY(), std::abs (x - origin), track.getHeight()), 2.5f);

        const float thumbWidth = lit ? 5.0f : 4.0f;
        const auto thumb = juce::Rectangle<float> (thumbWidth, 15.0f).withCentre ({ juce::jlimit (track.getX() + thumbWidth * 0.5f, track.getRight() - thumbWidth * 0.5f, x), track.getCentreY() });
        g.setColour (accent == tone::lime ? tone::lime : tone::text);
        g.fillRoundedRectangle (thumb, thumbWidth * 0.5f);
    }

    // ---- list -----------------------------------------------------------------------------------------------

    MenuField::MenuField (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
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

    MenuField::~MenuField()
    {
        stopTimer();
    }

    void MenuField::showInstead (const juce::String& text)
    {
        if (text != insteadText)
        {
            insteadText = text;
            repaint();
        }
    }

    void MenuField::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOver (true);
        auto b = getLocalBounds().toFloat();

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, b.removeFromTop (20.0f), juce::Justification::centredLeft, false);

        auto box = b.withTrimmedTop (2.0f);
        g.setColour (tone::text.withAlpha (lit ? 0.12f : 0.07f));
        g.fillRoundedRectangle (box, 7.0f);

        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 15.0f));
        g.drawText (insteadText.isNotEmpty() ? insteadText : choices[selected], box.reduced (11.0f, 0.0f).withTrimmedRight (14.0f), juce::Justification::centredLeft, true);

        juce::Path chevron;
        const float cx = box.getRight() - 15.0f, cy = box.getCentreY();
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (lit ? tone::text : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void MenuField::mouseUp (const juce::MouseEvent& e)
    {
        if (e.getNumberOfClicks() == 1 && getLocalBounds().contains (e.getPosition()))
            startTimer (menuDelayMs);
    }

    void MenuField::mouseDoubleClick (const juce::MouseEvent&)
    {
        stopTimer();
        resetToDefault();
    }

    void MenuField::resetToDefault()
    {
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
            menu.addItem (i + 1, choices[i], true, i == selected);

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
        return getLocalBounds().toFloat().withTrimmedTop (22.0f);
    }

    int Segments::segmentAt (juce::Point<float> p) const
    {
        const auto area = buttonsArea();

        if (! area.contains (p))
            return -1;

        return juce::jlimit (0, choices.size() - 1, (int) ((p.x - area.getX()) / area.getWidth() * (float) choices.size()));
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
            choose (hit);
    }

    void Segments::mouseDoubleClick (const juce::MouseEvent&)
    {
        choose (juce::roundToInt (param.convertFrom0to1 (param.getDefaultValue())));
    }

    void Segments::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat();

        g.setColour (hovered >= 0 ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, b.removeFromTop (20.0f), juce::Justification::centredLeft, false);

        const auto area = buttonsArea();
        g.setColour (tone::text.withAlpha (0.07f));
        g.fillRoundedRectangle (area, 7.0f);

        const float width = area.getWidth() / (float) choices.size();

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto cell = juce::Rectangle<float> (area.getX() + (float) i * width, area.getY(), width, area.getHeight());

            if (i == selected)
            {
                g.setColour (tone::text);
                g.fillRoundedRectangle (cell.reduced (2.0f), 5.5f);
            }
            else if (i == hovered)
            {
                g.setColour (tone::text.withAlpha (0.08f));
                g.fillRoundedRectangle (cell.reduced (2.0f), 5.5f);
            }

            g.setColour (i == selected ? tone::well : (i == hovered ? tone::text : tone::muted));
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (choices[i], cell, juce::Justification::centred, false);
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

        g.setColour (on ? tone::text : tone::text.withAlpha (lit ? 0.12f : 0.07f));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (on ? tone::well : (lit ? tone::text : tone::muted));
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
        hint = "Starting points for how tuning behaves. Click the name for the list.";
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
        g.setColour (tone::text.withAlpha (zone != none ? 0.12f : 0.07f));
        g.fillRoundedRectangle (b, 7.0f);

        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 14.0f));
        g.drawText (shown, b.reduced (arrowWidth, 0.0f), juce::Justification::centred, true);

        const auto chevron = [&] (float cx, float direction, bool lit)
        {
            juce::Path p;
            p.startNewSubPath (cx - 2.5f * direction, b.getCentreY() - 5.0f);
            p.lineTo (cx + 2.5f * direction, b.getCentreY());
            p.lineTo (cx - 2.5f * direction, b.getCentreY() + 5.0f);
            g.setColour (lit ? tone::text : tone::muted);
            g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        chevron (b.getX() + arrowWidth * 0.5f, -1.0f, zone == previous);
        chevron (b.getRight() - arrowWidth * 0.5f, 1.0f, zone == next);
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

        // With no preset known (a session saved before the preset was), the arrows go to the last or the first.
        const int current = processor.getCurrentProgram();
        if (hit == previous)
        {
            choose (current < 0 ? count - 1 : (current + count - 1) % count);
        }
        else if (hit == next)
        {
            choose (current < 0 ? 0 : (current + 1) % count);
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
