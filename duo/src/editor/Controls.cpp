#include "Controls.h"

namespace ui
{
    namespace
    {
        void drawChevronDown (juce::Graphics& g, juce::Point<float> at, juce::Colour colour)
        {
            juce::Path p;
            p.startNewSubPath (at.x - 4.0f, at.y - 2.0f);
            p.lineTo (at.x, at.y + 2.0f);
            p.lineTo (at.x + 4.0f, at.y - 2.0f);
            g.setColour (colour);
            g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        juce::String oneDecimalIfNeeded (float v)
        {
            const float rounded = std::round (v);
            return std::abs (v - rounded) < 0.05f ? juce::String ((int) rounded) : juce::String (v, 1);
        }
    }

    // ---- what every slider shares ------------------------------------------------------------------------------------

    ParamControl::ParamControl (Apvts& state, const juce::String& paramId, const juce::String& hintText)
        : param (*state.getParameter (paramId)),
          attachment (param, [this] (float v) { value = v; repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setWantsKeyboardFocus (false);
        typeFont = font (Weight::medium, 14.0f);
        typeBox.setFont (typeFont);
        typeBox.setJustificationType (juce::Justification::centred);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
        attachment.sendInitialUpdate();
    }

    ParamControl::~ParamControl()
    {
        stopTimer();
    }

    juce::String ParamControl::valueText() const
    {
        return param.getText (param.convertTo0to1 (value), 16);
    }

    void ParamControl::mouseEnter (const juce::MouseEvent&)  { hovered = true; repaint(); }
    void ParamControl::mouseExit (const juce::MouseEvent&)   { hovered = false; repaint(); }

    void ParamControl::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        if (e.mods.isPopupMenu())
            return;

        startedOnValue = valueArea().contains (e.getPosition());
        dragStartValue = value;
        dragging = true;
        attachment.beginGesture();

        // A click on the value waits to see if it is a click or a drag. Anywhere else, the control answers at once.
        if (! startedOnValue)
            attachment.setValueAsPartOfGesture (valueFromDrag (e, dragStartValue));

        repaint();
    }

    void ParamControl::mouseDrag (const juce::MouseEvent& e)
    {
        if (dragging && e.mouseWasDraggedSinceMouseDown())
            attachment.setValueAsPartOfGesture (valueFromDrag (e, dragStartValue));
    }

    void ParamControl::mouseUp (const juce::MouseEvent& e)
    {
        if (! dragging)
            return;

        dragging = false;
        attachment.endGesture();
        repaint();

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (startedOnValue && ! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu())
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void ParamControl::mouseDoubleClick (const juce::MouseEvent&)
    {
        stopTimer();
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

    void ParamControl::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
    {
        const float next = juce::jlimit (0.0f, 1.0f, param.convertTo0to1 (value) + (wheel.deltaY + wheel.deltaX) * 0.08f);
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (next));
    }

    void ParamControl::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void ParamControl::startTyping()
    {
        typeBox.setBounds (valueArea().reduced (1));
        typeBox.setText (valueText(), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    float ParamControl::parseTyped (const juce::String& text) const
    {
        return param.convertFrom0to1 (param.getValueForText (text));
    }

    void ParamControl::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim();

        if (! text.containsAnyOf ("0123456789") && ! text.containsIgnoreCase ("off") && ! text.containsIgnoreCase ("centre") && ! text.containsIgnoreCase ("center"))
            return;

        const float parsed = parseTyped (text);

        if (std::isfinite (parsed))
            attachment.setValueAsCompleteGesture (param.getNormalisableRange().snapToLegalValue (juce::jlimit (param.getNormalisableRange().start, param.getNormalisableRange().end, parsed)));
    }

    // ---- Width -------------------------------------------------------------------------------------------------------

    WidthControl::WidthControl (Apvts& state, const juce::String& hintText)
        : ParamControl (state, "width", hintText)
    {
    }

    juce::String WidthControl::valueText() const
    {
        return oneDecimalIfNeeded (getValue()) + "%";
    }

    juce::Rectangle<int> WidthControl::valueArea() const
    {
        return juce::Rectangle<int> (juce::roundToInt ((spanLeft + spanRight) * 0.5f) - 34, trackY + 13, 96, getHeight() - trackY - 13);
    }

    float WidthControl::valueFromDrag (const juce::MouseEvent& e, float) const
    {
        const float middle = (spanLeft + spanRight) * 0.5f, half = (spanRight - spanLeft) * 0.5f;
        return juce::jlimit (0.0f, 100.0f, std::abs (e.position.x - middle) / half * 100.0f);
    }

    void WidthControl::paint (juce::Graphics& g)
    {
        const bool lit = isLit();
        const float middle = (spanLeft + spanRight) * 0.5f, half = (spanRight - spanLeft) * 0.5f, reach = half * getValue() * 0.01f, y = (float) trackY;

        g.setColour (tone::well);
        g.fillRoundedRectangle (spanLeft - 11.0f, y - 2.5f, spanRight - spanLeft + 22.0f, 5.0f, 2.5f);
        g.setColour (tone::sky.withAlpha (lit ? 1.0f : 0.85f));
        g.fillRoundedRectangle (middle - reach, y - 2.5f, reach * 2.0f, 5.0f, 2.5f);

        // One handle under each singer. At Width 0 they meet in the middle.
        const float d = lit ? 22.0f : 20.0f;
        g.setColour (tone::text);
        g.fillEllipse (middle - reach - d * 0.5f, y - d * 0.5f, d, d);
        g.fillEllipse (middle + reach - d * 0.5f, y - d * 0.5f, d, d);

        // Name and value, side by side in the middle.
        const float baseY = y + 13.0f, h = (float) getHeight() - baseY;
        g.setFont (font (Weight::regular, 14.0f));
        g.setColour (lit ? tone::text : tone::muted);
        g.drawText ("Width", juce::Rectangle<float> (middle - 110.0f, baseY, 100.0f, h), juce::Justification::centredRight, false);
        g.setFont (font (Weight::medium, 21.0f));
        g.setColour (tone::text);
        g.drawText (valueText(), juce::Rectangle<float> (middle - 4.0f, baseY, 110.0f, h), juce::Justification::centredLeft, false);
    }

    // ---- Tightness ---------------------------------------------------------------------------------------------------

    TightnessControl::TightnessControl (Apvts& state, const juce::String& hintText)
        : ParamControl (state, "tightness", hintText)
    {
    }

    juce::String TightnessControl::valueText() const
    {
        return oneDecimalIfNeeded (getValue()) + "%";
    }

    juce::Rectangle<float> TightnessControl::track() const
    {
        return getLocalBounds().toFloat().withTrimmedTop (84.0f).withTrimmedBottom (30.0f);
    }

    juce::Rectangle<int> TightnessControl::valueArea() const
    {
        return getLocalBounds().withTrimmedTop (20).withHeight (34);
    }

    float TightnessControl::valueFromDrag (const juce::MouseEvent& e, float) const
    {
        const auto t = track();
        return juce::jlimit (0.0f, 100.0f, (t.getBottom() - e.position.y) / t.getHeight() * 100.0f);
    }

    void TightnessControl::paint (juce::Graphics& g)
    {
        const bool lit = isLit();
        const auto b = getLocalBounds().toFloat();
        const auto t = track();
        const float cx = b.getCentreX(), y = t.getBottom() - t.getHeight() * getValue() * 0.01f;

        g.setFont (font (Weight::regular, 14.0f));
        g.setColour (lit ? tone::text : tone::muted);
        g.drawText ("Tightness", b.withHeight (20.0f), juce::Justification::centred, false);
        g.setFont (font (Weight::medium, 21.0f));
        g.setColour (tone::text);
        g.drawText (valueText(), b.withTrimmedTop (20.0f).withHeight (34.0f), juce::Justification::centred, false);

        g.setFont (font (Weight::regular, 12.5f));
        g.setColour (tone::faint);
        g.drawText ("Loose", b.withTrimmedTop (58.0f).withHeight (20.0f), juce::Justification::centred, false);
        g.drawText ("Tight", b.withTop (t.getBottom() + 8.0f).withHeight (20.0f), juce::Justification::centred, false);

        g.setColour (tone::well);
        g.fillRoundedRectangle (cx - 2.5f, t.getY(), 5.0f, t.getHeight(), 2.5f);
        g.setColour (tone::sky.withAlpha (lit ? 1.0f : 0.85f));
        g.fillRoundedRectangle (cx - 2.5f, y, 5.0f, t.getBottom() - y, 2.5f);

        const float d = lit ? 22.0f : 20.0f;
        g.setColour (tone::text);
        g.fillEllipse (cx - d * 0.5f, y - d * 0.5f, d, d);
    }

    // ---- a level -----------------------------------------------------------------------------------------------------

    LevelFader::LevelFader (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::Colour fill)
        : ParamControl (state, paramId, hintText),
          label (labelText),
          colour (fill)
    {
    }

    juce::Rectangle<float> LevelFader::column() const
    {
        return getLocalBounds().toFloat().withTrimmedTop (28.0f).withTrimmedBottom (28.0f);
    }

    juce::Rectangle<int> LevelFader::valueArea() const
    {
        return getLocalBounds().removeFromBottom (24);
    }

    juce::String LevelFader::valueText() const
    {
        const float v = getValue();
        if (v <= param.getNormalisableRange().start)
            return "Off";
        return (v > 0.04f ? "+" : "") + juce::String (v, 1) + " dB";
    }

    void LevelFader::setSinging (bool isSinging)
    {
        if (singing != isSinging)
        {
            singing = isSinging;
            repaint();
        }
    }

    float LevelFader::valueFromDrag (const juce::MouseEvent& e, float startValue) const
    {
        const auto& range = param.getNormalisableRange();
        const float dbPerPixel = (range.end - range.start) / column().getHeight() / (e.mods.isShiftDown() ? 5.0f : 1.0f);
        return juce::jlimit (range.start, range.end, startValue - (float) e.getDistanceFromDragStartY() * dbPerPixel);
    }

    void LevelFader::paint (juce::Graphics& g)
    {
        const bool lit = isLit();
        const auto b = getLocalBounds().toFloat();
        const auto c = column();
        const float dim = singing ? 1.0f : 0.42f;
        const auto& range = param.getNormalisableRange();
        const float proportion = (getValue() - range.start) / (range.end - range.start);

        g.setFont (font (Weight::regular, 13.0f));
        g.setColour ((lit ? tone::text : tone::muted).withMultipliedAlpha (dim));
        g.drawText (label, b.withHeight (20.0f), juce::Justification::centred, false);

        // A slim column. The body of the fill stays quiet so the stage is what you look at first, and a bright cap marks the value.
        const auto bar = juce::Rectangle<float> (c.getCentreX() - 14.0f, c.getY(), 28.0f, c.getHeight());
        const float radius = 7.0f;
        g.setColour (tone::well);
        g.fillRoundedRectangle (bar, radius);

        const float zeroProportion = (0.0f - range.start) / (range.end - range.start);
        const float fillHeight = bar.getHeight() * proportion;

        if (fillHeight > 0.5f)
        {
            const auto fill = bar.withTop (bar.getBottom() - fillHeight);
            g.setColour (colour.withMultipliedAlpha (dim * (lit ? 0.62f : 0.46f)));
            g.fillRoundedRectangle (fill, juce::jmin (radius, fillHeight * 0.5f));
            g.setColour (colour.withMultipliedAlpha (dim));
            g.fillRoundedRectangle (fill.withHeight (juce::jmin (4.0f, fillHeight)), 2.0f);
        }

        // Where 0 dB is, the setting that changes nothing: a small tick beside the column.
        const float zeroY = bar.getBottom() - bar.getHeight() * zeroProportion;
        g.setColour (tone::faint.withMultipliedAlpha (dim));
        g.fillRect (juce::Rectangle<float> (bar.getRight() + 3.0f, zeroY - 0.5f, 5.0f, 1.0f));

        g.setFont (font (Weight::medium, 13.0f));
        g.setColour (tone::text.withMultipliedAlpha (dim));
        g.drawText (valueText(), b.withTop (b.getBottom() - 22.0f), juce::Justification::centred, false);
    }

    // ---- a setting in Advanced ---------------------------------------------------------------------------------------

    ValueSlider::ValueSlider (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Format f)
        : ParamControl (state, paramId, hintText),
          label (labelText),
          format (f)
    {
    }

    juce::String ValueSlider::valueText() const
    {
        const float v = getValue();

        if (format == Format::pan)
        {
            if (std::abs (v) < 0.05f)
                return "Centre";
            return oneDecimalIfNeeded (std::abs (v)) + (v < 0.0f ? " L" : " R");
        }

        return oneDecimalIfNeeded (v) + (format == Format::percent ? "%" : " ms");
    }

    float ValueSlider::parseTyped (const juce::String& text) const
    {
        const auto lower = text.toLowerCase().trim();

        if (format == Format::pan)
        {
            if (lower.startsWith ("c"))
                return 0.0f;

            const float amount = lower.retainCharacters ("0123456789.-").getFloatValue();
            return lower.containsChar ('l') ? -std::abs (amount) : amount;
        }

        return lower.retainCharacters ("0123456789.-").getFloatValue();
    }

    juce::Rectangle<int> ValueSlider::valueArea() const
    {
        return getLocalBounds().removeFromRight (juce::jmin (92, getWidth() / 2)).withTrimmedBottom (14);
    }

    float ValueSlider::valueFromDrag (const juce::MouseEvent& e, float startValue) const
    {
        const auto& range = param.getNormalisableRange();
        const float perPixel = (range.end - range.start) / juce::jmax (1.0f, (float) getWidth()) / (e.mods.isShiftDown() ? 5.0f : 1.0f);
        return juce::jlimit (range.start, range.end, startValue + (float) e.getDistanceFromDragStartX() * perPixel);
    }

    void ValueSlider::paint (juce::Graphics& g)
    {
        const bool lit = isLit();
        const auto b = getLocalBounds().toFloat();
        const auto& range = param.getNormalisableRange();
        const float proportion = (getValue() - range.start) / (range.end - range.start);
        const float origin = range.start < 0.0f ? 0.5f : 0.0f;

        const auto textArea = b.withTrimmedBottom (14.0f);
        g.setFont (font (Weight::regular, 13.0f));
        g.setColour (lit ? tone::text : tone::muted);
        g.drawText (label, textArea, juce::Justification::centredLeft, false);
        g.setFont (font (Weight::medium, 17.0f));
        g.setColour (tone::text);
        g.drawText (valueText(), textArea, juce::Justification::centredRight, false);

        const float y = b.getBottom() - 6.0f, x0 = b.getX() + 5.0f, w = b.getWidth() - 10.0f;
        g.setColour (tone::well);
        g.fillRoundedRectangle (b.getX(), y - 1.5f, b.getWidth(), 3.0f, 1.5f);
        const float at = x0 + w * proportion, from = x0 + w * origin;
        g.setColour (tone::sky.withAlpha (lit ? 1.0f : 0.8f));
        g.fillRoundedRectangle (juce::jmin (at, from), y - 1.5f, std::abs (at - from), 3.0f, 1.5f);
        const float d = lit ? 12.0f : 10.0f;
        g.setColour (tone::text);
        g.fillEllipse (at - d * 0.5f, y - d * 0.5f, d, d);
    }

    // ---- a list ------------------------------------------------------------------------------------------------------

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

    void ChoiceField::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setFont (font (Weight::regular, 13.0f));
        g.setColour (hovered ? tone::text : tone::muted);
        g.drawText (label, b, juce::Justification::centredLeft, false);
        g.setFont (font (Weight::medium, 15.0f));
        g.setColour (tone::text);
        g.drawText (choices[selected], b.withTrimmedRight (18.0f), juce::Justification::centredRight, false);
        drawChevronDown (g, { b.getRight() - 6.0f, b.getCentreY() }, hovered ? tone::text : tone::muted);
    }

    void ChoiceField::mouseEnter (const juce::MouseEvent&)  { hovered = true; repaint(); }
    void ChoiceField::mouseExit (const juce::MouseEvent&)   { hovered = false; repaint(); }

    void ChoiceField::mouseDown (const juce::MouseEvent&)
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());

        for (int i = 0; i < choices.size(); ++i)
            menu.addItem (i + 1, choices[i], true, i == selected);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (28),
                            [safe = juce::Component::SafePointer<ChoiceField> (this)] (int result)
                            {
                                if (safe != nullptr && result > 0)
                                    safe->attachment.setValueAsCompleteGesture ((float) (result - 1));
                            });
    }

    void ChoiceField::mouseDoubleClick (const juce::MouseEvent&)
    {
        juce::PopupMenu::dismissAllActiveMenus();
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

    // ---- how many doubles --------------------------------------------------------------------------------------------

    CountPicker::CountPicker (Apvts& state, const juce::String& paramId, const juce::String& hintText)
        : param (*state.getParameter (paramId)),
          attachment (param, [this] (float v) { count = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment.sendInitialUpdate();
    }

    juce::Rectangle<float> CountPicker::cell (int number) const
    {
        const float size = (float) getHeight(), gap = 6.0f;
        const float x = (float) getWidth() - (float) (4 - number) * size - (float) (3 - number) * gap;
        return { x, 0.0f, size, size };
    }

    int CountPicker::cellAt (juce::Point<float> p) const
    {
        for (int n = 1; n <= 3; ++n)
            if (cell (n).contains (p))
                return n;
        return 0;
    }

    void CountPicker::mouseMove (const juce::MouseEvent& e)
    {
        const int now = cellAt (e.position);
        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void CountPicker::mouseExit (const juce::MouseEvent&)
    {
        hovered = 0;
        repaint();
    }

    void CountPicker::mouseDown (const juce::MouseEvent& e)
    {
        if (const int n = cellAt (e.position))
            choose (n);
    }

    void CountPicker::mouseDoubleClick (const juce::MouseEvent&)
    {
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

    void CountPicker::choose (int newCount)
    {
        attachment.setValueAsCompleteGesture ((float) juce::jlimit (1, 3, newCount));
    }

    void CountPicker::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setFont (font (Weight::regular, 13.0f));
        g.setColour (tone::muted);
        g.drawText ("Doubles", b.withRight (cell (1).getX() - 10.0f), juce::Justification::centredRight, false);

        for (int n = 1; n <= 3; ++n)
        {
            const auto c = cell (n);
            const bool on = n == count;
            g.setColour (on ? tone::sky : tone::text.withAlpha (hovered == n ? 0.16f : 0.08f));
            g.fillEllipse (c);
            g.setFont (font (Weight::medium, 15.0f));
            g.setColour (on ? tone::ink : tone::text);
            g.drawText (juce::String (n), c, juce::Justification::centred, false);
        }
    }

    // ---- a switch ----------------------------------------------------------------------------------------------------

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
        g.setColour (on ? tone::text : tone::text.withAlpha (isDown ? 0.2f : (isOver ? 0.14f : 0.08f)));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        g.setFont (font (Weight::medium, 13.0f));
        g.setColour (on ? tone::ink : tone::text);
        g.drawText (getButtonText(), b, juce::Justification::centred, false);
    }

    // ---- a small live number -----------------------------------------------------------------------------------------

    Readout::Readout()
    {
        setOpaque (true);
        setInterceptsMouseClicks (false, false);
    }

    void Readout::setText (const juce::String& newText, bool isLit)
    {
        if (newText != text || isLit != lit)
        {
            text = newText;
            lit = isLit;
            repaint();
        }
    }

    void Readout::paint (juce::Graphics& g)
    {
        g.fillAll (tone::base);
        g.setFont (font (Weight::medium, 15.0f));
        g.setColour (lit ? tone::text : tone::faint);
        g.drawText (text, getLocalBounds().toFloat(), juce::Justification::centred, false);
    }
}
