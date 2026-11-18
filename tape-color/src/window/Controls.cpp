#include "Controls.h"

namespace ui
{
    namespace
    {
        constexpr float tileRadius = 9.0f;

        juce::String minusSign() { return juce::String::fromUTF8 ("\xe2\x88\x92"); }

        // Ribbon thickness at a place along it, 0 to 1.
        float ribbonThickness (float position, float height)
        {
            return juce::jmap (position, height, juce::jmax (5.0f, height * 0.16f));
        }

        juce::Path ribbonShape (float width, float height)
        {
            juce::Path p;
            const float left = ribbonThickness (0.0f, height), right = ribbonThickness (1.0f, height), middle = height * 0.5f;
            p.startNewSubPath (0.0f, middle - left * 0.5f);
            p.lineTo (width, middle - right * 0.5f);
            p.lineTo (width, middle + right * 0.5f);
            p.lineTo (0.0f, middle + left * 0.5f);
            p.closeSubPath();
            return p.createPathWithRoundedCorners (3.0f);
        }
    }

    // ---- block ------------------------------------------------------------------------------------------------

    juce::TextEditor* Tile::TypeBox::createEditorComponent()
    {
        auto* editor = juce::Label::createEditorComponent();
        editor->setJustification (juce::Justification::centredLeft);
        editor->setIndents (6, 2);
        editor->setBorder ({});
        return editor;
    }

    Tile::Tile (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, Formatter f)
        : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox),
          param (*state.getParameter (paramId)),
          label (labelText),
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
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);

        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, restValue);

        typeBox.setFont (font (Weight::medium, 16.0f));
        typeBox.setJustificationType (juce::Justification::centredLeft);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { applyTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    Tile::~Tile()
    {
        stopTimer();
    }

    juce::String Tile::valueText() const
    {
        return formatter ((float) getValue());
    }

    void Tile::setResting (bool shouldRest)
    {
        if (resting != shouldRest)
        {
            resting = shouldRest;
            repaint();
        }
    }

    juce::Rectangle<int> Tile::valueArea() const
    {
        auto b = getLocalBounds().reduced (compact ? 7 : 8, 0);
        return b.removeFromBottom (compact ? 30 : 36).withTrimmedBottom (4);
    }

    void Tile::resized()
    {
        juce::Slider::resized();
    }

    void Tile::mouseDown (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDown (e);
    }

    void Tile::mouseUp (const juce::MouseEvent& e)
    {
        juce::Slider::mouseUp (e);

        // Wait to see whether this is the first half of a double-click, which resets instead.
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && valueArea().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void Tile::mouseDoubleClick (const juce::MouseEvent& e)
    {
        stopTimer();
        juce::Slider::mouseDoubleClick (e);
    }

    void Tile::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void Tile::startTyping()
    {
        typeBox.setBounds (valueArea());
        typeBox.setFont (font (Weight::medium, compact ? 15.0f : 18.0f));
        typeBox.setText (valueText().replace (minusSign(), "-"), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    void Tile::applyTyped (const juce::String& typed)
    {
        auto text = typed.trim().replace (minusSign(), "-");

        // A word that means nothing here must not turn into zero. "Off" is the one word the cuts understand.
        const bool isOffWord = offWord.isNotEmpty() && text.equalsIgnoreCase (offWord);
        if (! text.containsAnyOf ("0123456789") && ! isOffWord)
            return;

        double value = 0.0;
        if (isOffWord)
            value = getValueFromText (text);
        else
        {
            value = text.retainCharacters ("0123456789.-").getDoubleValue();
            if (acceptsKilo && text.containsIgnoreCase ("k"))
                value *= 1000.0;
        }

        if (! std::isfinite (value))
            return;

        param.beginChangeGesture();
        setValue (juce::jlimit (getMinimum(), getMaximum(), value), juce::sendNotificationSync);
        param.endChangeGesture();
        repaint();
    }

    void Tile::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOverOrDragging() || forcedHover;
        const auto b = getLocalBounds().toFloat();
        const float dim = resting ? 0.55f : 1.0f;

        g.setColour (tone::text.withAlpha (lit ? 0.11f : 0.065f));
        g.fillRoundedRectangle (b, tileRadius);

        const float proportion = (float) valueToProportionOfLength (getValue());
        const float x = b.getWidth() * proportion;
        const float origin = getMinimum() < 0.0 && getMaximum() > 0.0 ? b.getWidth() * (float) valueToProportionOfLength (restValue) : 0.0f;

        const auto span = juce::Rectangle<float> (juce::jmin (origin, x), 0.0f, std::abs (x - origin), b.getHeight()).getSmallestIntegerContainer();

        if (std::abs (x - origin) > 0.25f)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (span);
            g.setColour (tone::amber.withAlpha ((lit ? 0.24f : 0.17f) * dim));
            g.fillRoundedRectangle (b, tileRadius);
        }

        // The exact value runs along the bottom edge, clear of the text. A control sitting at its middle rest shows a short mark.
        {
            juce::Graphics::ScopedSaveState save (g);
            auto strip = span.withTrimmedTop (getHeight() - 3);
            if (strip.getWidth() < 3)
                strip = strip.withSizeKeepingCentre (3, 3);
            g.reduceClipRegion (strip);
            g.setColour (tone::amber.withAlpha (dim));
            g.fillRoundedRectangle (b, tileRadius);
        }

        const int inset = compact ? 9 : 11;
        g.setColour ((lit ? tone::text : tone::muted).withAlpha (resting ? 0.7f : 1.0f));
        g.setFont (font (Weight::regular, compact ? 12.0f : 13.0f));
        g.drawText (label, getLocalBounds().reduced (inset, 0).withTrimmedTop (compact ? 6 : 8).withHeight (17), juce::Justification::centredLeft, true);

        if (! typeBox.isVisible())
        {
            g.setColour (tone::text.withAlpha (resting ? 0.6f : 1.0f));
            g.setFont (font (Weight::medium, compact ? 16.0f : 20.0f, true));
            g.drawText (valueText(), getLocalBounds().reduced (inset, 0).removeFromBottom (compact ? 31 : 36).withTrimmedBottom (compact ? 6 : 8), juce::Justification::centredLeft, false);
        }
    }

    // ---- chips ------------------------------------------------------------------------------------------------

    Segments::Segments (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText, juce::StringArray shortNames)
        : param (*state.getParameter (paramId)),
          label (labelText),
          names (std::move (shortNames)),
          attachment (param, [this] (float v) { selected = juce::roundToInt (v); repaint(); }, nullptr)
    {
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        attachment.sendInitialUpdate();
    }

    void Segments::setReading (const juce::String& text)
    {
        if (text != reading)
        {
            reading = text;
            repaint (getLocalBounds().withHeight (28));
        }
    }

    juce::Rectangle<float> Segments::chipArea (int index) const
    {
        auto row = getLocalBounds().toFloat().reduced (5.0f, 0.0f).removeFromBottom ((float) juce::jmin (32, getHeight() - 26)).withTrimmedBottom (5.0f);
        const float gap = 2.0f, w = (row.getWidth() - gap * (float) (names.size() - 1)) / (float) names.size();
        return { row.getX() + (float) index * (w + gap), row.getY(), w, row.getHeight() };
    }

    int Segments::chipAt (juce::Point<float> p) const
    {
        for (int i = 0; i < names.size(); ++i)
            if (chipArea (i).expanded (1.0f, 6.0f).contains (p))
                return i;
        return -1;
    }

    void Segments::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::text.withAlpha (isMouseOver (true) ? 0.11f : 0.065f));
        g.fillRoundedRectangle (b, tileRadius);

        const auto top = getLocalBounds().reduced (11, 0).withTrimmedTop (8).withHeight (17);
        g.setColour (isMouseOver (true) ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (label, top, juce::Justification::centredLeft, false);

        if (reading.isNotEmpty())
        {
            g.setColour (tone::amber);
            g.setFont (font (Weight::medium, 13.0f, true));
            g.drawText (reading, top, juce::Justification::centredRight, false);
        }

        for (int i = 0; i < names.size(); ++i)
        {
            const auto chip = chipArea (i);

            if (i == selected)
            {
                g.setColour (tone::text);
                g.fillRoundedRectangle (chip, 6.0f);
            }
            else if (i == hovered)
            {
                g.setColour (tone::text.withAlpha (0.1f));
                g.fillRoundedRectangle (chip, 6.0f);
            }

            g.setColour (i == selected ? tone::base : (i == hovered ? tone::text : tone::muted));
            g.setFont (font (i == selected ? Weight::medium : Weight::regular, 14.0f, true));
            g.drawText (names[i], chip, juce::Justification::centred, false);
        }
    }

    void Segments::mouseMove (const juce::MouseEvent& e)
    {
        const int now = chipAt (e.position);
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
        const int hit = chipAt (e.position);
        if (hit >= 0 && e.getNumberOfClicks() == 1)
            choose (hit);
    }

    void Segments::mouseDoubleClick (const juce::MouseEvent&)
    {
        choose (juce::roundToInt (param.convertFrom0to1 (param.getDefaultValue())));
    }

    // ---- list -------------------------------------------------------------------------------------------------

    ChoiceTile::ChoiceTile (Apvts& state, const juce::String& paramId, const juce::String& labelText, const juce::String& hintText)
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

    ChoiceTile::~ChoiceTile()
    {
        stopTimer();
    }

    void ChoiceTile::paint (juce::Graphics& g)
    {
        const bool lit = isMouseOver (true);
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::text.withAlpha (lit ? 0.11f : 0.065f));
        g.fillRoundedRectangle (b, tileRadius);

        g.setColour (lit ? tone::text : tone::muted);
        g.setFont (font (Weight::regular, 12.0f));
        g.drawText (label, getLocalBounds().reduced (9, 0).withTrimmedTop (6).withHeight (17), juce::Justification::centredLeft, false);

        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 15.0f));
        g.drawText (choices[selected], getLocalBounds().reduced (9, 0).removeFromBottom (31).withTrimmedBottom (6).withTrimmedRight (16), juce::Justification::centredLeft, true);

        juce::Path chevron;
        const float cx = b.getRight() - 15.0f, cy = b.getCentreY();
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (lit ? tone::text : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void ChoiceTile::mouseDown (const juce::MouseEvent&)
    {
        stopTimer();   // the second press of a double-click: the reset comes on its release, and no list opens
    }

    void ChoiceTile::mouseUp (const juce::MouseEvent& e)
    {
        // Wait to see whether this is the first half of a double-click, as Tile does. 200 ms was shorter than many double-clicks, so the list
        // opened under the second click and the reset never came.
        if (e.getNumberOfClicks() == 1 && getLocalBounds().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void ChoiceTile::mouseDoubleClick (const juce::MouseEvent&)
    {
        stopTimer();
        choose (juce::roundToInt (param.convertFrom0to1 (param.getDefaultValue())));
    }

    void ChoiceTile::timerCallback()
    {
        stopTimer();

        juce::PopupMenu menu;
        menu.setLookAndFeel (&getLookAndFeel());
        for (int i = 0; i < choices.size(); ++i)
            menu.addItem (i + 1, choices[i], true, i == selected);

        juce::Component::SafePointer<ChoiceTile> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (26),
                            [safe] (int result)
                            {
                                if (safe != nullptr && result > 0)
                                    safe->choose (result - 1);
                            });
    }

    // ---- button -----------------------------------------------------------------------------------------------

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
        g.setColour (on ? tone::text : tone::text.withAlpha (lit ? 0.14f : 0.08f));
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (on ? tone::base : (lit ? tone::text : tone::muted));
        g.setFont (font (Weight::medium, 14.0f));
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

    // ---- presets ----------------------------------------------------------------------------------------------

    PresetBox::PresetBox (juce::AudioProcessor& p) : processor (p)
    {
        hint = "Starting points for the whole tape. Click the name for the list.";
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
        g.fillRoundedRectangle (b, 8.0f);

        g.setColour (shown == noneName() ? tone::muted : tone::text);
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
        const int now = indexNow != nullptr ? indexNow() : processor.getCurrentProgram();

        // With no preset known the arrows go to the first or the last one.
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
                menu.addItem (i + 1, processor.getProgramName (i), true, i == now);

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

    // ---- the ribbon -------------------------------------------------------------------------------------------

    AgeRibbon::AgeRibbon (Apvts& state, const juce::String& paramId, const juce::String& hintText)
        : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox)
    {
        auto& param = *state.getParameter (paramId);
        hint = hintText;
        setComponentID (paramId);
        setTitle (param.getName (64));
        setRepaintsOnMouseActivity (true);
        setScrollWheelEnabled (true);
        setSliderSnapsToMousePosition (true);
        setMouseClickGrabsKeyboardFocus (false);
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        setOpaque (true);
        attachment = std::make_unique<juce::SliderParameterAttachment> (param, *this);
        setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));
    }

    void AgeRibbon::resized()
    {
        juce::Slider::resized();
        filled = {};
        empty = {};
        picture = {};
    }

    void AgeRibbon::buildImages (float scale)
    {
        imageScale = scale;
        const int w = juce::roundToInt ((float) getWidth() * scale), h = juce::roundToInt ((float) getHeight() * scale);
        const auto shape = ribbonShape ((float) getWidth(), (float) getHeight());

        filled = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        {
            juce::Graphics g (filled);
            g.addTransform (juce::AffineTransform::scale (scale));
            juce::ColourGradient wear (tone::text, 0.0f, 0.0f, tone::rust, (float) getWidth(), 0.0f, false);
            wear.addColour (0.42, tone::amber);
            g.setGradientFill (wear);
            g.fillPath (shape);
        }

        empty = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        {
            juce::Graphics g (empty);
            g.addTransform (juce::AffineTransform::scale (scale));
            g.setColour (tone::text.withAlpha (0.11f));
            g.fillPath (shape);
        }
    }

    void AgeRibbon::paint (juce::Graphics& g)
    {
        const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (! filled.isValid() || std::abs (scale - imageScale) > 0.001f)
        {
            buildImages (scale);
            picture = {};
        }

        const bool lit = isMouseOverOrDragging();
        const float width = (float) getWidth(), height = (float) getHeight();
        const float proportion = (float) valueToProportionOfLength (getValue());

        // One picture, redrawn only when it changes. The reel's repaints can stretch the area being drawn over this, so most paints only show it.
        if (! picture.isValid() || proportion != pictureProportion || lit != pictureLit)
        {
            pictureProportion = proportion;
            pictureLit = lit;
            picture = juce::Image (juce::Image::RGB, filled.getWidth(), filled.getHeight(), true);
            juce::Graphics p (picture);
            p.fillAll (tone::base);
            p.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            p.setColour (juce::Colours::black);
            p.drawImageAt (empty, 0, 0);
            {
                juce::Graphics::ScopedSaveState save (p);
                p.reduceClipRegion (0, 0, juce::roundToInt (width * proportion * scale), picture.getHeight());
                p.drawImageAt (filled, 0, 0);
            }

            // The handle stands a little taller than the ribbon, so it is easy to find on the thin end.
            p.addTransform (juce::AffineTransform::scale (scale));
            const float x = width * proportion;
            const float thickness = juce::jmax (ribbonThickness (proportion, height), height * 0.55f);
            const float handleWidth = lit ? 5.0f : 4.0f;
            const auto handle = juce::Rectangle<float> (handleWidth, juce::jmin (height, thickness + 10.0f))
                                    .withCentre ({ juce::jlimit (handleWidth * 0.5f, width - handleWidth * 0.5f, x), height * 0.5f });
            p.setColour (tone::base);
            p.fillRoundedRectangle (handle.expanded (2.0f, 0.0f), 3.0f);
            p.setColour (tone::text);
            p.fillRoundedRectangle (handle, handleWidth * 0.5f);
        }

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.setColour (juce::Colours::black);
        g.drawImageTransformed (picture, juce::AffineTransform::scale (1.0f / scale));
    }
}
