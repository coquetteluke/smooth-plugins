#include "KeyViews.h"

#include "../KeyAnalyser.h"

namespace keyui
{
    namespace
    {
        constexpr auto twoPi = juce::MathConstants<float>::twoPi;

        // Sizes of the rings as shares of the circle's radius.
        constexpr float outerTop = 1.0f, outerBottom = 0.67f, innerTop = 0.65f, innerBottom = 0.36f;
        constexpr float dotStep = 1.0f; // dots grow in steps of this many points

        constexpr int whiteNotes[7] = { 0, 2, 4, 5, 7, 9, 11 };
        constexpr int blackNotes[5] = { 1, 3, 6, 8, 10 };
        constexpr float blackAfter[5] = { 1.0f, 2.0f, 4.0f, 5.0f, 6.0f };

        bool isBlack (int note) noexcept
        {
            for (int b : blackNotes)
                if (b == note) return true;
            return false;
        }

        juce::Image makeLayer (int width, int height, float scale)
        {
            return juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) width * scale)),
                                juce::jmax (1, juce::roundToInt ((float) height * scale)), false);
        }
    }

    // ------------------------------------------------------------------------------------------------------------

    Readout::Readout (int weightToUse, float sizeToUse, juce::Colour colourToUse, Align alignToUse)
        : weight (weightToUse), size (sizeToUse), colour (colourToUse), align (alignToUse)
    {
        setOpaque (true);
        setInterceptsMouseClicks (true, false);
    }

    bool Readout::set (const juce::String& newText, juce::Colour newColour)
    {
        if (newText == text && newColour == colour)
            return false;

        text = newText;
        colour = newColour;
        repaint();
        return true;
    }

    void Readout::setDot (bool shouldShow)
    {
        if (dot != shouldShow)
        {
            dot = shouldShow;
            repaint();
        }
    }

    void Readout::paint (juce::Graphics& g)
    {
        ++paintCount;
        g.fillAll (tone::paper);
        const auto b = getLocalBounds().toFloat();
        g.setColour (colour);
        drawTextIn (g, text, weight, size, b, align);

        if (dot)
        {
            const float width = textWidth (text, weight, size);
            const float x = align == Align::right ? b.getRight() - width - 16.0f : b.getX();
            g.fillEllipse (x, b.getCentreY() - 4.5f, 9.0f, 9.0f);
        }
    }

    // ------------------------------------------------------------------------------------------------------------

    HeroView::HeroView()
    {
        setOpaque (true);
        setComponentID ("hero");
        typeBox.setJustificationType (juce::Justification::centredLeft);
        typeBox.setEditable (false, false, true);
        typeBox.onTextChange = [this] { if (onTyped != nullptr) onTyped (typeBox.getText()); };
        typeBox.onEditorHide = [this] { typeBox.setVisible (false); };
        addChildComponent (typeBox);
    }

    HeroView::~HeroView()
    {
        stopTimer();
    }

    bool HeroView::set (const Content& c)
    {
        if (c.manual != content.manual)
        {
            hint = c.manual ? manualHint : listenHint;
            setMouseCursor (c.manual ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
        }
        else if (hint.isEmpty())
        {
            hint = c.manual ? manualHint : listenHint;
        }

        if (c.key == content.key && c.scale == content.scale && c.otherName == content.otherName && c.relative == content.relative
             && c.dimmed == content.dimmed && c.hasKey == content.hasKey && c.manual == content.manual)
            return false;

        content = c;
        repaint();
        return true;
    }

    void HeroView::resized()
    {
        // Every key gets the same size, set by the widest, so the reading does not jump about when the key changes.
        // The tallest letters come from their real outlines, because round and pointed ones rise above the rest.
        juce::SharedResourcePointer<Assets> assets;
        tallest = juce::jmax (0.7f, -assets->shape ("ACDEFGB", heavy).path.getBounds().getY() * 0.001f);
        const float byWidth = (float) getWidth() / (textWidth ("A#", heavy, 1000.0f) * 0.001f);
        const float byHeight = (float) getHeight() / (tallest + 0.34f);
        keySize = juce::jmin (byWidth, byHeight);
        scaleSize = keySize * 0.30f;
        keyBaseline = keySize * tallest;
        scaleBaseline = keyBaseline + keySize * 0.075f + scaleSize * 0.72f;
    }

    juce::Rectangle<int> HeroView::keyLetterBounds() const
    {
        return juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), keyBaseline).getSmallestIntegerContainer();
    }

    juce::Rectangle<int> HeroView::swapBounds() const
    {
        return { getWidth() - 74, juce::roundToInt (scaleBaseline - 30.0f), 74, 34 };
    }

    void HeroView::paint (juce::Graphics& g)
    {
        ++paintCount;
        g.fillAll (tone::paper);

        juce::SharedResourcePointer<Assets> assets;

        // Sit the letters' left edge on the margin, whichever letter it is.
        const auto& keyShape = assets->shape (content.key, heavy);
        const float inset = keyShape.path.isEmpty() ? 0.0f : keyShape.path.getBounds().getX() * keySize * 0.001f;
        const auto keyColour = ! content.hasKey ? tone::deep : (content.dimmed ? tone::faint : tone::ink);
        g.setColour (keyColour);
        drawText (g, content.key, heavy, keySize, -inset, keyBaseline);

        // With no key yet, the same line says why, sized to stop short of the swap button.
        const auto& scaleShape = assets->shape (content.scale, bold);
        const float scaleInset = scaleShape.path.isEmpty() ? 0.0f : scaleShape.path.getBounds().getX() * 0.001f;
        const float room = (float) getWidth() - 96.0f;
        const float wordSize = content.hasKey ? scaleSize : juce::jmin (scaleSize, room / juce::jmax (0.01f, scaleShape.width * 0.001f));
        g.setColour (content.hasKey ? keyColour : tone::faint);
        drawText (g, content.scale, bold, wordSize, -scaleInset * wordSize, scaleBaseline);

        if (content.hasKey)
        {
            // Beside the scale, right aligned with the column: the relative key above its swap button.
            const float right = (float) getWidth() - 86.0f;
            g.setColour (tone::soft);
            drawText (g, "Relative", book, 15.0f, right, scaleBaseline - 45.0f, Align::right);
            g.setColour (tone::ink);
            drawText (g, content.relative, semi, 25.0f, right, scaleBaseline - 13.0f, Align::right);

            if (content.otherName.isNotEmpty())
            {
                g.setColour (tone::soft);
                drawText (g, content.otherName, book, 15.0f, right, scaleBaseline + 12.0f, Align::right);
            }
        }
    }

    void HeroView::mouseUp (const juce::MouseEvent& e)
    {
        if (content.manual && ! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1)
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void HeroView::mouseDoubleClick (const juce::MouseEvent&)
    {
        stopTimer();

        if (content.manual && onReset != nullptr)
            onReset();
    }

    void HeroView::timerCallback()
    {
        stopTimer();
        startTyping();
    }

    void HeroView::startTyping()
    {
        typeBox.setFont (typingFont (keySize * 0.34f));
        typeBox.setBounds (0, 0, getWidth(), juce::roundToInt (keyBaseline + 6.0f));
        typeBox.setText (content.key + (content.scale.isNotEmpty() ? " " + content.scale : juce::String()), juce::dontSendNotification);
        typeBox.setVisible (true);
        typeBox.showEditor();
    }

    // ------------------------------------------------------------------------------------------------------------

    CircleView::CircleView()
    {
        setOpaque (true);
        setComponentID ("circle");
        dirty.fill (false);
    }

    int CircleView::spokeOf (int key) noexcept
    {
        const int major = key < 12 ? key : (key - 12 + 3) % 12;
        return (major * 7) % 12;
    }

    juce::Point<float> CircleView::centre() const noexcept
    {
        return getLocalBounds().toFloat().getCentre();
    }

    float CircleView::holeRadius() const noexcept
    {
        return (float) juce::jmin (getWidth(), getHeight()) * 0.5f * innerBottom;
    }

    void CircleView::resized()
    {
        const float radius = (float) juce::jmin (getWidth(), getHeight()) * 0.5f;
        const auto c = centre();
        const float gap = 3.0f;

        for (int key = 0; key < 24; ++key)
        {
            const bool minor = key >= 12;
            const float top = radius * (minor ? innerTop : outerTop), bottom = radius * (minor ? innerBottom : outerBottom);
            const float middle = (top + bottom) * 0.5f;
            const float spoke = (float) spokeOf (key) / 12.0f * twoPi;
            const float half = twoPi / 24.0f - 0.5f * gap / middle;

            juce::Path wedge;
            wedge.addPieSegment (c.x - top, c.y - top, top * 2.0f, top * 2.0f, spoke - half, spoke + half, bottom / top);
            auto& slot = wedges[(size_t) key];
            slot = wedge;
            wedgeBounds[(size_t) key] = wedge.getBounds().getSmallestIntegerContainer().expanded (1);

            // Slightly bigger, to clear the old edge before a new colour goes on.
            juce::Path clear;
            const float wide = twoPi / 24.0f - 0.15f * gap / middle;
            clear.addPieSegment (c.x - top - 0.7f, c.y - top - 0.7f, (top + 0.7f) * 2.0f, (top + 0.7f) * 2.0f, spoke - wide, spoke + wide, (bottom - 0.7f) / (top + 0.7f));
            clears[(size_t) key] = clear;

            labelSpots[(size_t) key] = { c.x + middle * std::sin (spoke), c.y - middle * std::cos (spoke) };
        }

        layerStale = true;
    }

    juce::Point<float> CircleView::pointForKey (int key) const noexcept
    {
        return labelSpots[(size_t) juce::jlimit (0, 23, key)];
    }

    int CircleView::keyAt (juce::Point<float> p) const noexcept
    {
        const float radius = (float) juce::jmin (getWidth(), getHeight()) * 0.5f;
        const auto c = centre();
        const float distance = p.getDistanceFrom (c) / radius;
        const bool outer = distance <= outerTop && distance >= outerBottom - 0.01f;
        const bool inner = distance < outerBottom - 0.01f && distance >= innerBottom;

        if (! outer && ! inner)
            return -1;

        float angle = std::atan2 (p.x - c.x, c.y - p.y);
        if (angle < 0.0f) angle += twoPi;
        const int spoke = juce::roundToInt (angle / twoPi * 12.0f) % 12;
        const int major = (spoke * 7) % 12;
        return outer ? major : 12 + (major + 9) % 12;
    }

    void CircleView::markDirty (int key)
    {
        dirty[(size_t) key] = true;
        repaint (wedgeBounds[(size_t) key]);
    }

    bool CircleView::set (const std::array<juce::uint8, 24>& level, int shown, bool shownDimmed, bool manual)
    {
        bool changed = false;

        if (manual != manualMode)
        {
            manualMode = manual;
            setMouseCursor (manual ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            if (! manual) setHovered (-1);
            changed = true;
        }

        hint = manual ? manualHint : listenHint;

        if (shown != redKey || shownDimmed != redDimmed)
        {
            if (redKey >= 0) markDirty (redKey);
            if (shown >= 0) markDirty (shown);
            redKey = shown;
            redDimmed = shownDimmed;
            changed = true;
        }

        for (int key = 0; key < 24; ++key)
        {
            if (level[(size_t) key] != shownLevels[(size_t) key])
            {
                shownLevels[(size_t) key] = level[(size_t) key];
                markDirty (key);
                changed = true;
            }
        }

        return changed;
    }

    void CircleView::drawKey (juce::Graphics& g, int key)
    {
        const bool minor = key >= 12;
        const bool red = key == redKey;
        const float level = (float) shownLevels[(size_t) key] / (float) levels;
        const float radius = (float) juce::jmin (getWidth(), getHeight()) * 0.5f;

        auto fill = tone::wash.interpolatedWith (tone::ink, level);

        if (key == hoveredKey && manualMode)
            fill = tone::deep;

        if (red)
            fill = redDimmed ? tone::red.interpolatedWith (tone::paper, 0.45f) : tone::red;

        g.setColour (tone::paper);
        g.fillPath (clears[(size_t) key]);
        g.setColour (fill);
        g.fillPath (wedges[(size_t) key]);

        const auto spot = labelSpots[(size_t) key];
        const float size = radius * (minor ? 0.084f : 0.112f);
        const juce::String name = juce::String (smoothkey::keyNames[key % 12]) + (minor ? "m" : "");

        // Black and white are equally clear at half ink, so the type turns there.
        g.setColour (red || level >= 0.5f ? tone::paper : (minor && level < 0.15f ? tone::soft : tone::ink));
        drawText (g, name, red ? heavy : (minor ? medium : semi), size, spot.x, spot.y + size * 0.35f, Align::centre);
    }

    void CircleView::paint (juce::Graphics& g)
    {
        ++paintCount;
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (layerStale || scale != layerScale || ! layer.isValid())
        {
            layer = makeLayer (getWidth(), getHeight(), scale);
            layerScale = scale;
            layerStale = false;
            dirty.fill (false);

            juce::Graphics pen (layer);
            pen.addTransform (juce::AffineTransform::scale (scale));
            pen.fillAll (tone::paper);

            for (int key = 0; key < 24; ++key)
                drawKey (pen, key);
        }
        else
        {
            juce::Graphics pen (layer);
            pen.addTransform (juce::AffineTransform::scale (scale));

            for (int key = 0; key < 24; ++key)
            {
                if (dirty[(size_t) key])
                {
                    dirty[(size_t) key] = false;
                    drawKey (pen, key);
                }
            }
        }

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (layer, juce::AffineTransform::scale (1.0f / scale));
    }

    void CircleView::setHovered (int key)
    {
        if (key == hoveredKey)
            return;

        if (hoveredKey >= 0) markDirty (hoveredKey);
        hoveredKey = key;
        if (hoveredKey >= 0) markDirty (hoveredKey);
    }

    void CircleView::mouseMove (const juce::MouseEvent& e)
    {
        if (manualMode)
            setHovered (keyAt (e.position));
    }

    void CircleView::mouseExit (const juce::MouseEvent&)
    {
        setHovered (-1);
    }

    void CircleView::mouseDown (const juce::MouseEvent& e)
    {
        if (! manualMode || onPick == nullptr)
            return;

        const int key = keyAt (e.position);

        if (key >= 0)
            onPick (key % 12, key >= 12);
    }

    void CircleView::mouseDoubleClick (const juce::MouseEvent&)
    {
        if (manualMode && onReset != nullptr)
            onReset();
    }

    // ------------------------------------------------------------------------------------------------------------

    KeyboardView::KeyboardView()
    {
        setOpaque (true);
        setComponentID ("keyboard");
    }

    bool KeyboardView::inScale (int note) const noexcept
    {
        if (rootKey < 0)
            return false;

        constexpr int majorSteps[7] = { 0, 2, 4, 5, 7, 9, 11 };
        constexpr int minorSteps[7] = { 0, 2, 3, 5, 7, 8, 10 };
        const int step = (note - rootKey + 12) % 12;

        for (int s : (minorScale ? minorSteps : majorSteps))
            if (s == step) return true;

        return false;
    }

    juce::Rectangle<float> KeyboardView::keyBounds (int note) const noexcept
    {
        const float whiteWidth = (float) getWidth() / 7.0f;

        for (int i = 0; i < 7; ++i)
            if (whiteNotes[i] == note)
                return { (float) i * whiteWidth + 1.5f, 0.0f, whiteWidth - 3.0f, (float) getHeight() };

        const float width = whiteWidth * 0.60f;

        for (int i = 0; i < 5; ++i)
            if (blackNotes[i] == note)
                return { blackAfter[i] * whiteWidth - width * 0.5f, 0.0f, width, (float) getHeight() * 0.60f };

        return {};
    }

    juce::Point<float> KeyboardView::dotSpot (int note) const noexcept
    {
        const auto b = keyBounds (note);

        if (isBlack (note))
            return { b.getCentreX(), b.getBottom() - 46.0f };

        return { b.getCentreX(), b.getBottom() - 42.0f };
    }

    int KeyboardView::noteAt (juce::Point<float> p) const noexcept
    {
        for (int note : blackNotes)
            if (keyBounds (note).expanded (2.0f, 0.0f).contains (p))
                return note;

        const float whiteWidth = (float) getWidth() / 7.0f;
        const int i = (int) (p.x / whiteWidth);
        return getLocalBounds().toFloat().contains (p) ? whiteNotes[juce::jlimit (0, 6, i)] : -1;
    }

    void KeyboardView::resized()
    {
        layerStale = true;
    }

    bool KeyboardView::setScale (int key, bool minor, bool dimmed, bool manual)
    {
        hint = manual ? manualHint : listenHint;

        if (manual != manualMode)
        {
            manualMode = manual;
            setMouseCursor (manual ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        }

        if (key == rootKey && minor == minorScale && dimmed == dim)
            return false;

        rootKey = key;
        minorScale = minor;
        dim = dimmed;
        layerStale = true;
        repaint();
        return true;
    }

    bool KeyboardView::setNotes (const std::array<float, 12>& levels)
    {
        bool changed = false;

        for (int note = 0; note < 12; ++note)
        {
            // The notes in a frame share out to about 1, so a single loud note is near a third of that once smoothed.
            const float amount = juce::jlimit (0.0f, 1.0f, levels[(size_t) note] * 3.2f);
            const float wanted = amount < 0.1f ? 0.0f : std::sqrt (amount) * 9.0f / dotStep;
            const auto steps = (juce::uint8) juce::roundToInt (wanted);

            // Only move for a real change, so a note hovering between two sizes does not redraw over and over.
            if (steps != dots[(size_t) note] && (steps == 0 || std::abs (wanted - (float) dots[(size_t) note]) > 0.7f))
            {
                const float reach = juce::jmax ((float) steps, (float) dots[(size_t) note]) * dotStep + 1.5f;
                const auto spot = dotSpot (note);
                dots[(size_t) note] = steps;
                repaint (juce::Rectangle<float> (spot.x - reach, spot.y - reach, reach * 2.0f, reach * 2.0f).getSmallestIntegerContainer());
                changed = true;
            }
        }

        return changed;
    }

    void KeyboardView::rebuild (float scale)
    {
        layer = makeLayer (getWidth(), getHeight(), scale);
        layerScale = scale;
        layerStale = false;

        juce::Graphics pen (layer);
        pen.addTransform (juce::AffineTransform::scale (scale));
        pen.fillAll (tone::paper);

        const bool known = rootKey >= 0;
        const auto red = dim ? tone::red.interpolatedWith (tone::paper, 0.45f) : tone::red;
        constexpr float labelSize = 17.0f;

        const auto drawOne = [&] (int note)
        {
            const auto b = keyBounds (note);
            const bool black = isBlack (note), member = inScale (note), home = note == rootKey;

            // With no key yet it is a plain keyboard. With one, the notes of the key are solid and the rest fall back, so the
            // shape of the scale reads from a distance. Keeping the scale's colour off the key's own colour is the point.
            auto body = black ? tone::ink : tone::wash;
            auto label = black ? tone::paper : tone::soft;

            if (known)
            {
                body = member ? (dim ? tone::faint : tone::ink) : tone::wash;
                label = member ? tone::paper : tone::faint.darker (0.15f);
            }

            if (home)
            {
                body = red;
                label = tone::paper;
            }

            if (black)
            {
                // A sliver of paper around the short keys, so they read as sitting in front.
                pen.setColour (tone::paper);
                pen.fillRoundedRectangle (b.expanded (3.0f).withY (0.0f).withBottom (b.getBottom() + 3.0f), 4.0f);
            }

            pen.setColour (body);
            pen.fillRoundedRectangle (b, 3.0f);

            pen.setColour (label);
            drawText (pen, smoothkey::keyNames[note], home ? heavy : (member ? semi : book), home ? labelSize + 2.0f : labelSize,
                      b.getCentreX(), b.getBottom() - 15.0f, Align::centre);
        };

        for (int note : whiteNotes) drawOne (note);
        for (int note : blackNotes) drawOne (note);
    }

    void KeyboardView::paint (juce::Graphics& g)
    {
        ++paintCount;
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (layerStale || scale != layerScale || ! layer.isValid())
            rebuild (scale);

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed (layer, juce::AffineTransform::scale (1.0f / scale));

        // The live part: one path per colour.
        juce::Path onLight, onDark;

        for (int note = 0; note < 12; ++note)
        {
            const float radius = (float) dots[(size_t) note] * dotStep;

            if (radius > 0.0f)
            {
                const auto spot = dotSpot (note);
                const bool darkBody = rootKey >= 0 ? inScale (note) : isBlack (note);
                auto& target = darkBody ? onDark : onLight;
                target.addEllipse (spot.x - radius, spot.y - radius, radius * 2.0f, radius * 2.0f);
            }
        }

        if (! onLight.isEmpty())
        {
            g.setColour (tone::ink);
            g.fillPath (onLight);
        }

        if (! onDark.isEmpty())
        {
            g.setColour (tone::paper);
            g.fillPath (onDark);
        }
    }

    void KeyboardView::mouseDown (const juce::MouseEvent& e)
    {
        if (! manualMode || onPick == nullptr)
            return;

        const int note = noteAt (e.position);

        if (note >= 0)
            onPick (note);
    }
}
