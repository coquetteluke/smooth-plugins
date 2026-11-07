#include "Controls.h"

namespace ui
{
    namespace
    {
        constexpr int typingTimer = 1, textTimer = 2;
        constexpr double textIntervalTouched = 100.0, textIntervalAutomated = 250.0;

        juce::Slider::SliderStyle cellStyle() { return juce::Slider::RotaryHorizontalVerticalDrag; }

        juce::String signedText (float v, int decimals)
        {
            const auto text = juce::String (std::abs (v), decimals);
            const bool zero = text.getDoubleValue() == 0.0;
            return (zero ? juce::String() : (v > 0.0f ? juce::String ("+") : juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")))) + text;
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    ParamCell::ParamCell (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Size sizeToUse, Unit unitToUse)
        : juce::Slider (cellStyle(), juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
          size (sizeToUse),
          unit (unitToUse),
          twoSided (param.convertFrom0to1 (0.0f) < 0.0f && param.convertFrom0to1 (1.0f) > 0.0f)
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

        typeBox.setFont (font (Weight::medium, size == Size::main ? 17.0f : 13.0f));
        typeBox.setJustificationType (juce::Justification::centredLeft);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    ParamCell::~ParamCell()
    {
        stopTimer (typingTimer);
        stopTimer (textTimer);
    }

    void ParamCell::valueChanged()
    {
        // The bar moves every time. The number at most 10 times a second under the mouse, and 4 times when something else is moving
        // it, like automation or a hand on the field, where the big number on top is the one being read.
        const auto time = juce::Time::getMillisecondCounterHiRes();
        const double textInterval = isMouseOverOrDragging() ? textIntervalTouched : textIntervalAutomated;

        if (time - textBuilt >= textInterval)
        {
            stopTimer (textTimer);
            textStale = true;
            repaint();
        }
        else
        {
            repaint (barArea());
            if (! isTimerRunning (textTimer))
                startTimer (textTimer, juce::jmax (1, (int) (textInterval - (time - textBuilt))));
        }
    }

    void ParamCell::valueParts (juce::String& number, juce::String& unitText) const
    {
        const auto v = (float) getValue();
        unitText = {};

        switch (unit)
        {
            case Unit::hz:
                number = v < 1.0f ? juce::String (v, 3) : (v < 10.0f ? juce::String (v, 2) : juce::String (v, 1));
                unitText = "Hz";
                break;
            case Unit::ms:
                number = v < 10.0f ? juce::String (v, 2) : (v < 100.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v)));
                unitText = "ms";
                break;
            case Unit::flangeMs:     number = signedText (v, 2); unitText = "ms"; break;
            case Unit::percent:      number = juce::String (juce::roundToInt (v)); unitText = "%"; break;
            case Unit::signedPercent:number = signedText ((float) juce::roundToInt (v), 0); unitText = "%"; break;
            case Unit::signedDb:     number = signedText (v, 1); unitText = "dB"; break;
            case Unit::degrees:      number = juce::String (juce::roundToInt (v)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0")); break;
            case Unit::frequencyOrOff:
            {
                const auto t = param.getText (param.convertTo0to1 (v), 32);
                if (t.containsIgnoreCase ("off"))
                {
                    number = "Off";
                }
                else if (v >= 1000.0f)
                {
                    number = juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2);
                    unitText = "kHz";
                }
                else
                {
                    number = juce::String (juce::roundToInt (v));
                    unitText = "Hz";
                }
                break;
            }
            case Unit::words:
                number = param.getText (param.convertTo0to1 (v), 32);
                break;
        }
    }

    juce::Rectangle<int> ParamCell::valueArea() const
    {
        return size == Size::main ? juce::Rectangle<int> (0, 20, getWidth(), 34)
                                  : juce::Rectangle<int> (0, 16, getWidth(), 20);
    }

    juce::Rectangle<int> ParamCell::barArea() const
    {
        return getLocalBounds().removeFromBottom (size == Size::main ? 9 : 7);
    }

    void ParamCell::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer (typingTimer);
        // Shift held as the drag starts gives a five times finer drag.
        setMouseDragSensitivity (e.mods.isShiftDown() ? 1100 : 220);
        juce::Slider::mouseDown (e);
    }

    void ParamCell::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && valueArea().contains (e.getPosition()))
            startTimer (typingTimer, juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void ParamCell::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer (typingTimer);
        juce::Slider::mouseDoubleClick (e);
    }

    void ParamCell::timerCallback (int id)
    {
        stopTimer (id);

        if (id == typingTimer)
        {
            startTyping();
        }
        else
        {
            textStale = true;
            repaint();
        }
    }

    void ParamCell::startTyping()
    {
        typeBox.setBounds (valueArea().withWidth (juce::jmin (getWidth(), 110)));
        juce::String n, u;
        valueParts (n, u);
        typeBox.setText (n.replace (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")), "-"), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void ParamCell::applyTyped (const juce::String& typed)
    {
        const auto text = typed.trim().toLowerCase();

        if (text.isEmpty())
            return;

        double value = 0.0;

        if (unit == Unit::words || (unit == Unit::frequencyOrOff && text.startsWith ("off")))
        {
            // The parameter's own reader knows Triangle, Sine, Square, Off and the sync divisions.
            value = param.convertFrom0to1 (param.getValueForText (typed.trim()));
        }
        else
        {
            if (! text.containsAnyOf ("0123456789"))
                return;

            value = text.getDoubleValue();

            // "3k" and "3.2 kHz" are the natural ways to type a frequency.
            if ((unit == Unit::hz || unit == Unit::frequencyOrOff) && text.containsChar ('k'))
                value *= 1000.0;
        }

        if (! std::isfinite (value))
            return;

        param.beginChangeGesture();
        setValue (value, juce::sendNotificationSync);
        param.endChangeGesture();
    }

    void ParamCell::buildText (float scale, bool lit)
    {
        const auto b = getLocalBounds().toFloat();
        const bool main = size == Size::main;
        const bool narrow = main && getWidth() < 96;   // while Advanced is open
        const float labelSize = main ? 13.0f : 12.0f, unitSize = main ? (narrow ? 12.0f : 13.0f) : 11.0f;
        float numberSize = main ? (narrow ? 19.0f : 23.0f) : 15.0f;

        const int w = juce::jmax (1, juce::roundToInt (b.getWidth() * scale)), h = juce::jmax (1, juce::roundToInt (b.getHeight() * scale));
        if (! textImage.isValid() || textImage.getWidth() != w || textImage.getHeight() != h)
            textImage = juce::Image (juce::Image::ARGB, w, h, true, juce::NativeImageType());
        else
            textImage.clear (textImage.getBounds());

        juce::Graphics g (textImage);
        g.addTransform (juce::AffineTransform::scale ((float) w / b.getWidth(), (float) h / b.getHeight()));

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, labelSize));
        g.drawText (label, b.withHeight (main ? 18.0f : 15.0f).withTrimmedRight ((float) headerInset), juce::Justification::centredLeft, false);

        juce::String number, unitText;
        valueParts (number, unitText);

        // A long value, like a sync division in a narrow cell, gets smaller rather than cut off.
        const float unitRoom = unitText.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (font (Weight::regular, unitSize), unitText) + 4.0f;
        const float wanted = juce::GlyphArrangement::getStringWidth (font (Weight::medium, numberSize), number) + unitRoom;
        if (wanted > b.getWidth())
            numberSize = juce::jmax (11.0f, numberSize * (b.getWidth() - unitRoom) / (wanted - unitRoom));

        const auto valueFont = font (Weight::medium, numberSize);
        const auto va = valueArea().toFloat();
        const float numberWidth = juce::GlyphArrangement::getStringWidth (valueFont, number);
        g.setColour (tone::text);
        g.setFont (valueFont);
        g.drawText (number, va.withWidth (numberWidth + 2.0f), juce::Justification::centredLeft, false);

        if (unitText.isNotEmpty())
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, unitSize));
            g.drawText (unitText, va.withTrimmedLeft (numberWidth + 4.0f).withTrimmedBottom (main ? 3.0f : 1.0f), juce::Justification::bottomLeft, false);
        }

        textScale = scale;
        textLit = lit;
        textStale = false;
        textBuilt = juce::Time::getMillisecondCounterHiRes();
    }

    void ParamCell::paint (juce::Graphics& g)
    {
        const bool lit = isEnabled() && (isMouseOverOrDragging() || forcedHover);
        const auto b = getLocalBounds().toFloat();
        const bool main = size == Size::main;
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (textStale || ! textImage.isValid() || lit != textLit || std::abs (scale - textScale) > 0.01f
            || textImage.getWidth() != juce::roundToInt (b.getWidth() * scale))
            buildText (scale, lit);

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (textImage, b, juce::RectanglePlacement::stretchToFit);

        // The bar. Square ended, like everything else that shows a position.
        const float barHeight = main ? 3.0f : 2.0f;
        const auto bar = b.withTop (b.getBottom() - barHeight - (main ? 2.0f : 1.0f)).withHeight (barHeight);
        const float p = (float) valueToProportionOfLength (getValue());
        g.setColour (tone::track);
        g.fillRect (bar);

        const float from = twoSided ? (float) valueToProportionOfLength (0.0) : 0.0f;
        const float x1 = bar.getX() + bar.getWidth() * juce::jmin (from, p), x2 = bar.getX() + bar.getWidth() * juce::jmax (from, p);
        g.setColour (lit ? tone::violet : tone::steel);
        g.fillRect (juce::Rectangle<float> (x1, bar.getY(), juce::jmax (x2 - x1, 2.0f), barHeight));

        if (twoSided)
        {
            g.setColour (tone::muted);
            g.fillRect (juce::Rectangle<float> (bar.getX() + bar.getWidth() * from - 0.5f, bar.getY() - 2.0f, 1.0f, barHeight + 4.0f));
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    ParamToggle::ParamToggle (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : label (labelText)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        attachment = std::make_unique<juce::ButtonParameterAttachment> (*state.getParameter (paramId), *this);
    }

    void ParamToggle::paintButton (juce::Graphics& g, bool isOver, bool)
    {
        const auto b = getLocalBounds().toFloat();
        const bool on = getToggleState();
        g.setColour (on ? tone::steel : (isOver ? tone::raisedLit : tone::raised));
        g.fillRect (b);
        g.setColour (on ? tone::well : (isOver ? tone::text : tone::muted));
        g.setFont (font (Weight::medium, getHeight() < 24 ? 12.0f : 13.0f));
        g.drawText (label, b, juce::Justification::centred, false);
    }

    // ----------------------------------------------------------------------------------------------------------

    ParamSwitch::ParamSwitch (Apvts& state, const juce::String& paramId, const juce::String& hintText)
        : choices (dynamic_cast<juce::AudioParameterChoice&> (*state.getParameter (paramId)).choices),
          attachment (*state.getParameter (paramId),
                      [this] (float value)
                      {
                          selected = juce::roundToInt (value);
                          repaint();
                      })
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (state.getParameter (paramId)->getName (64));
        attachment.sendInitialUpdate();
    }

    int ParamSwitch::cellAt (juce::Point<float> p) const
    {
        if (! getLocalBounds().toFloat().contains (p))
            return -1;

        return juce::jlimit (0, choices.size() - 1, (int) (p.x / (float) getWidth() * (float) choices.size()));
    }

    void ParamSwitch::mouseMove (const juce::MouseEvent& e)
    {
        const int now = cellAt (e.position);
        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void ParamSwitch::mouseExit (const juce::MouseEvent&)
    {
        hovered = -1;
        repaint();
    }

    void ParamSwitch::mouseDown (const juce::MouseEvent& e)
    {
        const int hit = cellAt (e.position);
        if (hit >= 0 && hit != selected)
            attachment.setValueAsCompleteGesture ((float) hit);
    }

    void ParamSwitch::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::raised);
        g.fillRect (b);
        const float w = b.getWidth() / (float) choices.size();

        for (int i = 0; i < choices.size(); ++i)
        {
            const auto cell = juce::Rectangle<float> (b.getX() + w * (float) i, b.getY(), w, b.getHeight());

            if (i == selected)
            {
                g.setColour (tone::steel);
                g.fillRect (cell);
            }

            g.setColour (i == selected ? tone::well : (i == hovered ? tone::text : tone::muted));
            g.setFont (font (Weight::medium, 14.0f));
            g.drawText (choices[i], cell, juce::Justification::centred, false);
        }
    }

    // ----------------------------------------------------------------------------------------------------------

    GainStrip::GainStrip (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
        : juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setOpaque (true);
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (false);
        setMouseDragSensitivity (300);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));

        typeBox.setFont (font (Weight::medium, 13.0f));
        typeBox.setJustificationType (juce::Justification::centred);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this]
        {
            const auto text = typeBox.getText().trim();
            if (text.containsAnyOf ("0123456789"))
            {
                param.beginChangeGesture();
                setValue (text.getDoubleValue(), juce::sendNotificationSync);
                param.endChangeGesture();
            }
        };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    GainStrip::~GainStrip()
    {
        stopTimer();
    }

    juce::Rectangle<int> GainStrip::valueArea() const { return getLocalBounds().removeFromBottom (24); }
    juce::Rectangle<int> GainStrip::barsArea() const  { return getLocalBounds().withTrimmedTop (22).withTrimmedBottom (28); }

    juce::String GainStrip::valueText() const
    {
        const auto v = (float) getValue();
        return signedText (v, 1);
    }

    void GainStrip::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        setMouseDragSensitivity (e.mods.isShiftDown() ? 1500 : 300);
        juce::Slider::mouseDown (e);
    }

    void GainStrip::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && valueArea().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void GainStrip::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDoubleClick (e);
    }

    void GainStrip::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void GainStrip::startTyping()
    {
        typeBox.setBounds (valueArea());
        typeBox.setText (juce::String ((float) getValue(), 1), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void GainStrip::setLevels (float left, float right, bool mono, double dtSeconds)
    {
        const float fall = (float) std::exp (-dtSeconds / 0.25);
        const auto toScale = [] (float peak)
        {
            const float db = juce::Decibels::gainToDecibels (peak, -100.0f);
            return juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 54.0f);   // -48 dB to +6 dB
        };

        const float now[2] { toScale (left), toScale (right) };
        bool moved = mono != isMono;
        isMono = mono;
        const auto bars = barsArea();

        for (int ch = 0; ch < 2; ++ch)
        {
            shown[ch] = juce::jmax (now[ch], shown[ch] * fall + (1.0f - fall) * now[ch]);
            const int pixels = juce::roundToInt (shown[ch] * (float) bars.getHeight() * 2.0f);   // half a point
            moved = moved || pixels != drawn[ch];
            drawn[ch] = pixels;
        }

        if (moved)
            repaint (bars);
    }

    void GainStrip::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOverOrDragging();
        g.fillAll (tone::base);

        if (g.clipRegionIntersects (getLocalBounds().withBottom (22)))
        {
            g.setColour (lit ? tone::text : tone::muted);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (label, getLocalBounds().withHeight (18), juce::Justification::centred, false);
        }

        if (g.clipRegionIntersects (valueArea()))
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::medium, 14.0f));
            g.drawText (valueText(), valueArea(), juce::Justification::centredBottom, false);
        }

        const auto bars = barsArea().toFloat();
        const float cx = bars.getCentreX(), barW = 7.0f, gap = 2.0f;
        const int count = isMono ? 1 : 2;

        for (int ch = 0; ch < count; ++ch)
        {
            const float x = isMono ? cx - barW * 0.5f : (ch == 0 ? cx - gap * 0.5f - barW : cx + gap * 0.5f);
            const auto bar = juce::Rectangle<float> (x, bars.getY(), barW, bars.getHeight());
            g.setColour (tone::well);
            g.fillRect (bar);
            const float h = shown[ch] * bars.getHeight();
            g.setColour (tone::steel);
            g.fillRect (bar.withTop (bar.getBottom() - h));
        }

        // The gain's own handle.
        const float y = bars.getBottom() - bars.getHeight() * (float) valueToProportionOfLength (getValue());
        g.setColour (lit ? tone::violet : tone::text);
        g.fillRect (juce::Rectangle<float> (cx - 16.0f, y - 1.5f, 32.0f, 3.0f));
    }
}
