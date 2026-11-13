#include "Controls.h"

namespace ui
{
    // ---- bar ------------------------------------------------------------------------------------------------

    juce::TextEditor* BarSlider::TypeBox::createEditorComponent()
    {
        auto* editor = juce::Label::createEditorComponent();
        editor->setJustification (juce::Justification::centredRight);
        editor->setIndents (0, 3);
        editor->setBorder ({});
        return editor;
    }

    BarSlider::BarSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Size sizeToUse, Formatter f)
        : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          size (sizeToUse),
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

        typeBox.setFont (font (Weight::medium, size == Size::large ? 26.0f : 15.0f));
        typeBox.setJustificationType (juce::Justification::centredRight);
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
        return getLocalBounds().removeFromTop (topHeight()).removeFromRight (juce::jmin (size == Size::large ? 170 : 100, getWidth() * 2 / 3));
    }

    void BarSlider::setDimmed (bool shouldBe)
    {
        if (shouldBe != dimmed)
        {
            dimmed = shouldBe;
            repaint();
        }
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
        typeBox.setBounds (valueArea().withTrimmedTop (size == Size::large ? 4 : 1).withTrimmedBottom (size == Size::large ? 4 : 1));
        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void BarSlider::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim().replace (juce::String::fromUTF8 ("\xe2\x88\x92"), "-");   // the minus sign the value is shown with

        // A word that means nothing here must not turn into zero.
        if (! text.containsAnyOf ("0123456789"))
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
        const bool large = size == Size::large;
        auto b = getLocalBounds().toFloat();
        auto top = b.removeFromTop ((float) topHeight());

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (large ? Weight::medium : Weight::regular, large ? 16.0f : 13.0f));
        g.drawText (label, top, large ? juce::Justification::bottomLeft : juce::Justification::centredLeft, false);

        if (! typeBox.isVisible())
        {
            g.setColour (dimmed ? tone::muted : tone::text);
            g.setFont (font (Weight::medium, large ? 34.0f : 17.0f, true));
            g.drawText (valueText(), top.withTrimmedBottom (large ? -5.0f : 0.0f), large ? juce::Justification::bottomRight : juce::Justification::centredRight, false);
        }

        const float thickness = large ? 10.0f : 5.0f;
        const auto track = b.withHeight (thickness).withCentre ({ b.getCentreX(), b.getY() + (large ? 13.0f : 9.0f) });
        const float corner = large ? 2.0f : 2.5f;
        g.setColour (lit ? tone::track2 : tone::track);
        g.fillRoundedRectangle (track, corner);

        const float proportion = (float) valueToProportionOfLength (getValue());
        const float x = track.getX() + track.getWidth() * proportion;

        // A control whose rest is in the middle fills out from the middle.
        const bool centred = getMinimum() < 0.0 && getMaximum() > 0.0 && std::abs (restValue) < 1.0e-3;
        const float origin = track.getX() + (centred ? track.getWidth() * (float) valueToProportionOfLength (restValue) : 0.0f);
        g.setColour (large ? tone::hot : (dimmed ? tone::faint : tone::muted));
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (origin, x), track.getY(), std::abs (x - origin), track.getHeight()), corner);

        if (centred && large)
        {
            g.setColour (tone::black);
            g.fillRect (juce::Rectangle<float> (2.0f, track.getHeight()).withCentre ({ origin, track.getCentreY() }));
        }

        const float thumbWidth = large ? (lit ? 7.0f : 6.0f) : (lit ? 5.0f : 4.0f);
        const auto thumb = juce::Rectangle<float> (thumbWidth, large ? 22.0f : 15.0f).withCentre ({ juce::jlimit (track.getX() + thumbWidth * 0.5f, track.getRight() - thumbWidth * 0.5f, x), track.getCentreY() });
        g.setColour (dimmed ? tone::muted : tone::text);
        g.fillRoundedRectangle (thumb, large ? 2.0f : thumbWidth * 0.5f);

        if (large && lowWord.isNotEmpty())
        {
            auto words = getLocalBounds().toFloat().removeFromBottom (15.0f);
            g.setColour (tone::faint);
            g.setFont (font (Weight::regular, 12.0f));
            g.drawText (lowWord, words, juce::Justification::centredLeft, false);
            g.drawText (highWord, words, juce::Justification::centredRight, false);
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

        g.setColour (on ? tone::text : (lit ? tone::well2 : tone::well));
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (on ? tone::black : (lit ? tone::text : tone::muted));
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
        hint = "Starting points. Click the name for the list.";
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
        g.setColour (zone != none ? tone::well2 : tone::well);
        g.fillRoundedRectangle (b, 3.0f);

        g.setColour (processor.getCurrentProgram() < 0 ? tone::muted : tone::text);   // no preset known: a session saved before 0.6.2
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
        const int current = processor.getCurrentProgram();   // -1: none known, so the arrows go to the first or the last

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
