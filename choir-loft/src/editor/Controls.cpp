#include "Controls.h"

#include "../Scales.h"

namespace ui
{
    namespace
    {
        constexpr float corner = 7.0f;
        constexpr int barDragPixels = 240;

        void drawChevron (juce::Graphics& g, juce::Point<float> at, float direction, juce::Colour colour)
        {
            juce::Path p;
            p.startNewSubPath (at.x - 4.0f, at.y - 2.0f * direction);
            p.lineTo (at.x, at.y + 2.0f * direction);
            p.lineTo (at.x + 4.0f, at.y - 2.0f * direction);
            g.setColour (colour);
            g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Mode numbers from HarmonyBrain.h, kept apart so the controls do not need the whole brain.
        constexpr int modeScale = 0, modeMidi = 1, modeFixed = 2, modeChord = 3, modeChannel = 4;

        juce::String intervalWords (int semitones)
        {
            if (semitones == 0)
                return "Unison";

            return juce::String (smoothharmony::intervalName (semitones)) + (semitones > 0 ? " up" : " down");
        }

        juce::String semitoneText (int semitones)
        {
            if (semitones == 0)
                return "0 st";

            return (semitones > 0 ? "+" : "") + juce::String (semitones) + " st";
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    BarSlider::BarSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour accentColour)
        : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          accent (accentColour)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (false);
        setMouseDragSensitivity (barDragPixels);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));

        typeBox.setFont (font (Weight::medium, 13.0f));
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
        const auto v = (float) getValue();
        auto text = param.getText (param.convertTo0to1 (v), 16);
        const auto unit = param.getLabel();

        if (text == "Off" || text == "Centre")
            return text;

        if (showSign && v > 0.0f && ! text.startsWith ("+"))
            text = "+" + text;

        if (unit.isEmpty())
            return text;

        return unit == "%" ? text + unit : text + " " + unit;
    }

    float BarSlider::textOverflow() const
    {
        const float name = juce::GlyphArrangement::getStringWidth (font (Weight::regular, 13.0f), label);
        const float value = juce::GlyphArrangement::getStringWidth (font (Weight::medium, 13.0f), valueText());
        return name + value + 8.0f - (float) getWidth();
    }

    juce::Rectangle<int> BarSlider::valueArea() const
    {
        return getLocalBounds().removeFromRight (juce::jmin (84, getWidth() / 2));
    }

    void BarSlider::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        // Shift held as the drag starts gives a five times finer drag.
        setMouseDragSensitivity (barDragPixels * (e.mods.isShiftDown() ? 5 : 1));
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
        typeBox.setBounds (valueArea().reduced (2));
        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void BarSlider::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim();
        const auto lower = text.toLowerCase();

        // The parameter's own reader, so Off, Centre and 35 L all work.
        if (! text.containsAnyOf ("0123456789") && lower != "off" && lower != "centre" && lower != "center" && lower != "c")
            return;

        const double value = param.convertFrom0to1 (param.getValueForText (text));

        if (! std::isfinite (value))
            return;

        param.beginChangeGesture();
        setValue (value, juce::sendNotificationSync);
        param.endChangeGesture();
    }

    void BarSlider::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        const bool lit = isMouseOverOrDragging() || forcedHover;

        // Name and value on one line, a slim track under them. The whole row is the handle.
        constexpr float trackHeight = 3.0f;
        const auto track = juce::Rectangle<float> (b.getX(), b.getBottom() - trackHeight - 1.0f, b.getWidth(), trackHeight);

        g.setColour (tone::text.withAlpha (lit ? 0.2f : 0.11f));
        g.fillRoundedRectangle (track, 1.5f);

        // The fill grows from the middle when the range goes both ways, as Pan and Tone do.
        const float proportion = (float) valueToProportionOfLength (getValue());
        const bool fromMiddle = getMinimum() < 0.0 && getMaximum() > 0.0 && getMinimum() > -60.0;
        const float origin = fromMiddle ? (float) valueToProportionOfLength (0.0) : 0.0f;
        const float edge = track.getX() + track.getWidth() * proportion;
        const float start = track.getX() + track.getWidth() * origin;

        g.setColour (accent.withAlpha (lit ? 1.0f : 0.86f));

        if (std::abs (edge - start) > 0.25f)
            g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (start, edge), track.getY(), std::abs (edge - start), track.getHeight()), 1.5f);

        // The handle: a short upright bar, so it can be read from across the room and grabbed by eye.
        const float handleHeight = lit ? 13.0f : 11.0f;
        g.setColour (accent);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jlimit (b.getX(), b.getRight() - 3.0f, edge - 1.5f), track.getCentreY() - handleHeight * 0.5f, 3.0f, handleHeight), 1.5f);

        auto text = b.withTrimmedBottom (trackHeight + 7.0f);
        g.setFont (font (Weight::regular, 13.0f));
        g.setColour (lit ? tone::text : tone::muted);
        g.drawText (label, text, juce::Justification::centredLeft, false);
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (valueText(), text, juce::Justification::centredRight, false);
    }

    // ----------------------------------------------------------------------------------------------------------

    IntervalField::IntervalField (Apvts& state, const juce::String& paramId, int voiceIndex, juce::Colour accentColour)
        : juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          index (voiceIndex),
          accent (accentColour)
    {
        hint = "How far this voice sits from the lead. Drag up or down, use the arrows, or click to type semitones.";
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (false);
        setMouseDragSensitivity (320);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));

        typeBox.setFont (font (Weight::medium, 15.0f));
        typeBox.setJustificationType (juce::Justification::centred);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    IntervalField::~IntervalField()
    {
        stopTimer();
    }

    void IntervalField::setMode (int newMode)
    {
        if (mode == newMode)
            return;

        mode = newMode;

        if (mode == modeMidi || mode == modeChannel)
            hint = "MIDI decides what this voice sings in this Mode. The interval is kept for the other Modes.";
        else if (mode == modeFixed)
            hint = "Semitones from the lead, whatever the key. Drag up or down, use the arrows, or click to type.";
        else if (mode == modeChord)
            hint = "This voice sings the chord note nearest to this far from the lead. Drag, use the arrows, or click to type.";
        else
            hint = "How far this voice sits from the lead, counted in the key. Drag, use the arrows, or click to type semitones.";

        repaint();
    }

    int IntervalField::arrowAt (juce::Point<float> p) const
    {
        if (p.x < (float) getWidth() - (compact ? 18.0f : 26.0f))
            return 0;

        return p.y < (float) getHeight() * 0.5f ? 1 : -1;
    }

    void IntervalField::mouseMove (const juce::MouseEvent& e)
    {
        const int now = arrowAt (e.position);

        if (now != hoveredArrow)
        {
            hoveredArrow = now;
            repaint();
        }
    }

    void IntervalField::mouseExit (const juce::MouseEvent& e)
    {
        hoveredArrow = 0;
        juce::Slider::mouseExit (e);
    }

    void IntervalField::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        const int arrow = arrowAt (e.position);
        onArrow = arrow != 0;

        if (onArrow)
            step (arrow);
        else
            juce::Slider::mouseDown (e);
    }

    void IntervalField::mouseDrag (const juce::MouseEvent& e)
    {
        if (! onArrow)
            juce::Slider::mouseDrag (e);
    }

    void IntervalField::mouseUp (const juce::MouseEvent& e)
    {
        if (onArrow)
        {
            onArrow = false;
            return;
        }

        juce::Slider::mouseUp (e);

        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu())
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void IntervalField::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();

        if (arrowAt (e.position) != 0)
            step (arrowAt (e.position));
        else
            juce::Slider::mouseDoubleClick (e);
    }

    void IntervalField::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void IntervalField::step (int direction)
    {
        int next = juce::roundToInt (getValue());

        // Counted in the key, +3 and +4 both mean a third, so one press moves to the next name.
        const auto words = intervalWords (next);

        do
            next += direction;
        while ((mode == modeScale || mode == modeChord) && next >= -24 && next <= 24 && intervalWords (next) == words);

        next = juce::jlimit (-24, 24, next);
        param.beginChangeGesture();
        setValue (next, juce::sendNotificationSync);
        param.endChangeGesture();
    }

    void IntervalField::startTyping()
    {
        typeBox.setBounds (getLocalBounds().withWidth (96).withSizeKeepingCentre (96, juce::jmin (getHeight(), 30)).withX (0));
        typeBox.setText (juce::String (juce::roundToInt (getValue())), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void IntervalField::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim();

        if (! text.containsAnyOf ("0123456789"))
            return;

        param.beginChangeGesture();
        setValue (juce::jlimit (-24, 24, text.getIntValue()), juce::sendNotificationSync);
        param.endChangeGesture();
    }

    void IntervalField::words (juce::String& big, juce::String& small) const
    {
        const bool fromMidi = mode == modeMidi || mode == modeChannel;
        const int semitones = juce::roundToInt (getValue());

        if (fromMidi)
        {
            big = "MIDI";
            small = mode == modeChannel ? "channel " + juce::String (index + 1) : "held notes";
        }
        else if (mode == modeFixed)
        {
            big = semitoneText (semitones);
            small = intervalWords (semitones);
        }
        else
        {
            big = intervalWords (semitones);
            small = semitoneText (semitones);
        }
    }

    float IntervalField::bigScale() const
    {
        if (! compact)
            return 1.0f;

        // With eight voices and the lead in the row, "Octave down" is a hair wider than its card. Narrow the letters by up to 5% before cutting any off.
        juce::String big, small;
        words (big, small);
        const float width = juce::GlyphArrangement::getStringWidth (font (Weight::regular, 17.0f), big);
        return width > 0.0f ? juce::jlimit (0.95f, 1.0f, ((float) getWidth() - textTrim()) / width) : 1.0f;
    }

    float IntervalField::wordsOverflow() const
    {
        juce::String big, small;
        words (big, small);
        const float room = (float) getWidth() - textTrim();

        if (compact)
            return juce::jmax (juce::GlyphArrangement::getStringWidth (font (Weight::regular, 17.0f), big) * bigScale(),
                               juce::GlyphArrangement::getStringWidth (font (Weight::regular, 12.0f), small)) - room;

        // The big words, then the small ones 8 to their right. See paint.
        return juce::GlyphArrangement::getStringWidth (font (Weight::regular, 27.0f), big) + 2.0f + 8.0f
               + juce::GlyphArrangement::getStringWidth (font (Weight::regular, 13.0f), small) - room;
    }

    void IntervalField::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        const bool lit = isMouseOverOrDragging() || forcedHover;
        const bool fromMidi = mode == modeMidi || mode == modeChannel;

        juce::String big, small;
        words (big, small);

        auto text = b.withTrimmedRight (textTrim());

        if (compact)
        {
            g.setFont (font (Weight::regular, 17.0f).withHorizontalScale (bigScale()));
            g.setColour (fromMidi ? tone::muted : tone::text);
            g.drawText (big, text.removeFromTop (b.getHeight() * 0.56f).withTrimmedTop (2.0f), juce::Justification::bottomLeft, false);
            g.setFont (font (Weight::regular, 12.0f));
            g.setColour (lit ? tone::text : tone::muted);
            g.drawText (small, text, juce::Justification::topLeft, false);
        }
        else
        {
            g.setFont (font (Weight::regular, 27.0f));
            const float bigWidth = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), big);
            g.setColour (fromMidi ? tone::muted : tone::text);
            g.drawText (big, text.removeFromLeft (bigWidth + 2.0f), juce::Justification::centredLeft, false);

            g.setFont (font (Weight::regular, 13.0f));
            g.setColour (lit ? tone::text : tone::muted);
            g.drawText (small, text.withTrimmedLeft (8.0f).withTrimmedTop (5.0f), juce::Justification::centredLeft, false);
        }

        const float x = b.getRight() - (compact ? 8.0f : 13.0f);
        const auto arrowColour = [this, lit, fromMidi] (int which)
        {
            if (hoveredArrow == which)
                return tone::text;

            return (lit ? tone::muted : tone::faint).withMultipliedAlpha (fromMidi ? 0.6f : 1.0f);
        };
        drawChevron (g, { x, b.getCentreY() - 8.0f }, -1.0f, arrowColour (1));
        drawChevron (g, { x, b.getCentreY() + 8.0f }, 1.0f, arrowColour (-1));
    }

    // ----------------------------------------------------------------------------------------------------------

    ChoiceField::ChoiceField (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
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

    void ChoiceField::setOverride (const juce::String& text)
    {
        if (overrideText != text)
        {
            overrideText = text;
            repaint();
        }
    }

    void ChoiceField::paint (juce::Graphics& g)
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (tone::text.withAlpha (hovered ? 0.095f : 0.055f));
        g.fillRoundedRectangle (b, corner);

        drawChevron (g, { b.getRight() - 14.0f, b.getCentreY() }, 1.0f, hovered ? tone::text : tone::muted);

        auto text = b.reduced (10.0f, 0.0f).withTrimmedRight (16.0f);
        g.setFont (font (Weight::regular, 13.0f));
        g.setColour (hovered ? tone::text : tone::muted);
        const float labelWidth = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label);
        g.drawText (label, text.removeFromLeft (labelWidth + 1.0f), juce::Justification::centredLeft, false);

        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (overrideText.isNotEmpty() ? overrideText : choices[selected], text, juce::Justification::centredRight, true);
    }

    void ChoiceField::mouseEnter (const juce::MouseEvent&)  { hovered = true; repaint(); }
    void ChoiceField::mouseExit (const juce::MouseEvent&)   { hovered = false; repaint(); }

    void ChoiceField::mouseDown (const juce::MouseEvent& e)
    {
        if (e.getNumberOfClicks() > 1)
            return;

        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());

        for (int i = 0; i < choices.size(); ++i)
            menu.addItem (i + 1, choices[i], true, i == selected);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (26),
                            [safe = juce::Component::SafePointer<ChoiceField> (this)] (int result)
                            {
                                if (safe != nullptr && result > 0)
                                    safe->pick (result - 1);
                            });
    }

    void ChoiceField::pick (int index)
    {
        attachment.setValueAsCompleteGesture ((float) juce::jlimit (0, choices.size() - 1, index));
        if (onUserPick)
            onUserPick();
    }

    void ChoiceField::mouseDoubleClick (const juce::MouseEvent&)
    {
        juce::PopupMenu::dismissAllActiveMenus();
        pick (juce::roundToInt (param.convertFrom0to1 (param.getDefaultValue())));
    }

    // ----------------------------------------------------------------------------------------------------------

    Segments::Segments (Apvts& state, const juce::String& paramId, juce::StringArray labelsToUse, int firstValue, const juce::String& hintText)
        : param (*state.getParameter (paramId)),
          labels (std::move (labelsToUse)),
          mainHint (hintText),
          first (firstValue),
          attachment (param, [this] (float v) { selected = juce::roundToInt (v) - first; repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));

        // Wider words get wider cells, so the row reads evenly.
        const auto measure = font (Weight::medium, 13.0f);

        for (const auto& l : labels)
            weights.push_back (juce::GlyphArrangement::getStringWidth (measure, l) + 22.0f);

        attachment.sendInitialUpdate();
    }

    juce::Rectangle<float> Segments::cellBounds (int cell) const
    {
        float total = 0.0f, before = 0.0f;

        for (size_t i = 0; i < weights.size(); ++i)
        {
            if ((int) i < cell)
                before += weights[i];

            total += weights[i];
        }

        const float w = (float) getWidth() / total;
        return { before * w, 0.0f, weights[(size_t) cell] * w, (float) getHeight() };
    }

    int Segments::cellAt (juce::Point<float> p) const
    {
        for (int i = 0; i < labels.size(); ++i)
            if (cellBounds (i).contains (p))
                return i;

        return -1;
    }

    void Segments::mouseMove (const juce::MouseEvent& e)
    {
        const int now = cellAt (e.position);

        if (now != hovered)
        {
            hovered = now;
            hint = juce::isPositiveAndBelow (now, cellHints.size()) ? cellHints[now] : mainHint;
            repaint();
        }
    }

    void Segments::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        hint = mainHint;
        repaint();
    }

    void Segments::mouseDown (const juce::MouseEvent& e)
    {
        const int hit = cellAt (e.position);

        if (hit >= 0)
            choose (hit);
    }

    void Segments::mouseDoubleClick (const juce::MouseEvent&)
    {
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

    void Segments::choose (int cell)
    {
        if (cell != selected)
            attachment.setValueAsCompleteGesture ((float) (first + cell));
    }

    void Segments::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::text.withAlpha (0.055f));
        g.fillRoundedRectangle (b, corner);

        for (int i = 0; i < labels.size(); ++i)
        {
            const auto cell = cellBounds (i);

            if (i == selected)
            {
                g.setColour (tone::text.withAlpha (0.92f));
                g.fillRoundedRectangle (cell.reduced (2.0f), corner - 2.0f);
            }
            else if (i == hovered)
            {
                g.setColour (tone::text.withAlpha (0.07f));
                g.fillRoundedRectangle (cell.reduced (2.0f), corner - 2.0f);
            }

            g.setColour (i == selected ? tone::ink : (i == hovered ? tone::text : tone::muted));
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (labels[i], cell, juce::Justification::centred, false);
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    SingerSwitch::SingerSwitch (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour colourToUse)
        : label (labelText),
          colour (colourToUse),
          rightColour (colourToUse),
          attachment (*state.getParameter (paramId), [this] (float v) { heard = v >= 0.5f; repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        attachment.sendInitialUpdate();
    }

    void SingerSwitch::setRight (const juce::String& text, juce::Colour newColour, float size)
    {
        if (text != rightText || newColour != rightColour || size != rightSize)
        {
            rightText = text;
            rightColour = newColour;
            rightSize = size;
            repaint();
        }
    }

    void SingerSwitch::toggle()
    {
        attachment.setValueAsCompleteGesture (heard ? 0.0f : 1.0f);
    }

    void SingerSwitch::mouseEnter (const juce::MouseEvent&)  { hovered = true; repaint(); }
    void SingerSwitch::mouseExit (const juce::MouseEvent&)   { hovered = false; repaint(); }

    void SingerSwitch::mouseDown (const juce::MouseEvent&)
    {
        if (onClickFirst == nullptr || ! onClickFirst())
            toggle();
    }

    void SingerSwitch::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        const auto dot = juce::Rectangle<float> (b.getX(), b.getCentreY() - 5.0f, 10.0f, 10.0f);

        if (heard)
        {
            g.setColour (colour);
            g.fillEllipse (hovered ? dot.expanded (1.0f) : dot);
        }
        else
        {
            g.setColour (colour);
            g.drawEllipse (dot.reduced (0.75f), 1.5f);
        }

        if (label.isNotEmpty())
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::medium, compact ? 13.0f : 14.0f));
            g.drawText (label, b.withTrimmedLeft (compact ? 15.0f : 18.0f), juce::Justification::centredLeft, false);
        }

        if (rightText.isNotEmpty())
        {
            g.setColour (rightColour);
            g.setFont (font (rightSize >= 16.0f ? Weight::medium : Weight::regular, rightSize));
            g.drawText (rightText, b, juce::Justification::centredRight, false);
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    VoicePicker::VoicePicker (Apvts& state, const juce::String& hintText)
        : firstParam (*state.getParameter ("voices")),
          moreParam (*state.getParameter ("moreVoices")),
          firstAttachment (firstParam, [this] (float v) { first = juce::roundToInt (v); repaint(); }, nullptr),
          moreAttachment (moreParam, [this] (float v) { more = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID ("voices");
        setTitle ("Voices");
        firstAttachment.sendInitialUpdate();
        moreAttachment.sendInitialUpdate();
    }

    int VoicePicker::cellAt (juce::Point<float> p) const
    {
        if (! getLocalBounds().toFloat().contains (p))
            return -1;

        return juce::jlimit (0, 7, (int) (p.x / (float) getWidth() * 8.0f));
    }

    void VoicePicker::choose (int count)
    {
        count = juce::jlimit (1, 8, count);

        if (count == getCount() && first + more == count && (count <= 4 ? more == 0 : first == 4))
            return;

        const float newFirst = (float) juce::jmin (count, 4), newMore = (float) juce::jmax (0, count - 4);
        firstAttachment.beginGesture();
        moreAttachment.beginGesture();
        firstAttachment.setValueAsPartOfGesture (newFirst);
        moreAttachment.setValueAsPartOfGesture (newMore);
        firstAttachment.endGesture();
        moreAttachment.endGesture();
    }

    void VoicePicker::mouseMove (const juce::MouseEvent& e)
    {
        const int now = cellAt (e.position);

        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void VoicePicker::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    void VoicePicker::mouseDown (const juce::MouseEvent& e)
    {
        const int hit = cellAt (e.position);

        if (hit >= 0)
            choose (hit + 1);
    }

    void VoicePicker::mouseDoubleClick (const juce::MouseEvent&)
    {
        choose (juce::roundToInt (firstParam.convertFrom0to1 (firstParam.getDefaultValue())));
    }

    void VoicePicker::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::text.withAlpha (0.055f));
        g.fillRoundedRectangle (b, corner);

        const int count = juce::jlimit (1, 8, getCount());
        const float w = b.getWidth() / 8.0f;

        for (int i = 0; i < 8; ++i)
        {
            const auto cell = juce::Rectangle<float> ((float) i * w, 0.0f, w, b.getHeight());

            if (i + 1 == count)
            {
                g.setColour (tone::text.withAlpha (0.92f));
                g.fillRoundedRectangle (cell.reduced (2.0f), corner - 2.0f);
            }
            else if (i == hovered)
            {
                g.setColour (tone::text.withAlpha (0.07f));
                g.fillRoundedRectangle (cell.reduced (2.0f), corner - 2.0f);
            }

            g.setColour (i + 1 == count ? tone::ink : (i == hovered ? tone::text : tone::muted));
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (juce::String (i + 1), cell, juce::Justification::centred, false);
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    PillToggle::PillToggle (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : juce::ToggleButton (labelText)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        attachment = std::make_unique<juce::ButtonParameterAttachment> (*state.getParameter (paramId), *this);
    }

    void PillToggle::paintButton (juce::Graphics& g, bool isOver, bool isDown)
    {
        const auto b = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const bool lit = isOver || isDown || forcedHover;

        g.setColour (on ? tone::text.withAlpha (0.92f) : tone::text.withAlpha (lit ? 0.095f : 0.055f));
        g.fillRoundedRectangle (b, corner);
        g.setColour (on ? tone::ink : (lit ? tone::text : tone::muted));
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (getButtonText(), b, juce::Justification::centred, false);
    }
}
