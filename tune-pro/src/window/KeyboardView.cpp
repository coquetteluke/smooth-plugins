#include "KeyboardView.h"

using namespace smoothtune;
using namespace tuneui;

namespace
{
    // Key layout across one octave, in white key widths.
    constexpr bool isBlack[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    constexpr float whiteIndex[12] = { 0, 0.68f, 1, 1.68f, 2, 3, 3.68f, 4, 4.68f, 5, 5.68f, 6 };
}

KeyboardView::KeyboardView (PluginProcessor& p) : processor (p)
{
    setOpaque (true);
    setBufferedToImage (true);
    setTitle ("Notes");
    for (int i = 0; i < 12; ++i)
    {
        noteParams[(size_t) i] = processor.apvts.getRawParameterValue ("note" + juce::String (i));
        modesShown[i] = modeOf (i);
    }
    scaleShown = processor.getInScaleMask();
    bypassParam = processor.apvts.getRawParameterValue ("bypass");
}

int KeyboardView::modeOf (int pitchClass) const noexcept
{
    return juce::roundToInt (noteParams[(size_t) pitchClass]->load (std::memory_order_relaxed));
}

void KeyboardView::resized() {}

juce::Rectangle<float> KeyboardView::keyBounds (int pc) const noexcept
{
    const auto area = getLocalBounds().toFloat();
    const float white = area.getWidth() / 7.0f;
    if (isBlack[pc])
        return { area.getX() + whiteIndex[pc] * white, area.getY(), white * 0.64f, area.getHeight() * 0.58f };
    return { area.getX() + whiteIndex[pc] * white, area.getY(), white, area.getHeight() };
}

int KeyboardView::noteAt (juce::Point<float> p) const noexcept
{
    for (int pc = 0; pc < 12; ++pc)
        if (isBlack[pc] && keyBounds (pc).contains (p))
            return pc;
    for (int pc = 0; pc < 12; ++pc)
        if (! isBlack[pc] && keyBounds (pc).contains (p))
            return pc;
    return -1;
}

void KeyboardView::setNote (int pc, int mode)
{
    if (! juce::isPositiveAndBelow (pc, 12))
        return;
    auto* parameter = processor.apvts.getParameter ("note" + juce::String (pc));
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) mode));
    parameter->endChangeGesture();
    advance();
    // The line under the mouse says what the key does now (up to 1.8.1 it kept describing the mode before the click).
    if (pc == hovered)
        sayHint();
}

void KeyboardView::cycleNote (int pc)
{
    if (juce::isPositiveAndBelow (pc, 12))
        setNote (pc, (modeOf (pc) + 1) % 3);
}

void KeyboardView::openMenu (int pc)
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());
    const int mode = modeOf (pc);
    menu.addItem (1, "Tune", true, mode == noteTune);
    menu.addItem (2, "Remove", true, mode == noteRemove);
    menu.addItem (3, "Bypass", true, mode == noteBypass);
    juce::Component::SafePointer<KeyboardView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (keyBounds (pc).toNearestInt())), [safe, pc] (int result)
    {
        if (result > 0 && safe != nullptr)
            safe->setNote (pc, result - 1);
    });
}

void KeyboardView::mouseDown (const juce::MouseEvent& e)
{
    const int pc = noteAt (e.position);
    if (pc < 0)
        return;

    if (e.mods.isPopupMenu())
        openMenu (pc);
    else if (e.getNumberOfClicks() >= 2)
        setNote (pc, noteTune);     // double-click goes back to the default
    else
        cycleNote (pc);
}

void KeyboardView::setHovered (int pc)
{
    if (pc == hovered)
        return;
    if (juce::isPositiveAndBelow (hovered, 12))
        repaint (keyBounds (hovered).getSmallestIntegerContainer());
    hovered = pc;
    if (juce::isPositiveAndBelow (hovered, 12))
        repaint (keyBounds (hovered).getSmallestIntegerContainer());
    sayHint();
}

void KeyboardView::sayHint()
{
    if (! onHint)
        return;
    if (hovered < 0)
    {
        onHint ({});
        return;
    }
    const auto mode = modeOf (hovered);
    const char* now = mode == noteRemove ? " is removed, the tuner avoids it. Click to bypass it."
                    : mode == noteBypass ? " is bypassed, it passes untouched. Click to tune it."
                                         : " is tuned. Click to remove it, click again to bypass it.";
    onHint (juce::String (noteNames[hovered]) + now);
}

void KeyboardView::mouseMove (const juce::MouseEvent& e) { setHovered (noteAt (e.position)); }
void KeyboardView::mouseExit (const juce::MouseEvent&)   { setHovered (-1); }

void KeyboardView::advance()
{
    const auto r = processor.getReadings();
    const bool bypassed = bypassParam->load (std::memory_order_relaxed) > 0.5f;
    const int lit = r.voiced && ! stale && ! bypassed && r.targetNote >= 0 ? r.targetNote % 12 : -1;

    if (lit != litShown)
    {
        if (juce::isPositiveAndBelow (litShown, 12)) repaint (keyBounds (litShown).getSmallestIntegerContainer());
        if (juce::isPositiveAndBelow (lit, 12))      repaint (keyBounds (lit).getSmallestIntegerContainer());
        litShown = lit;
    }

    for (int pc = 0; pc < 12; ++pc)
    {
        const int mode = modeOf (pc);
        if (mode != modesShown[pc])
        {
            modesShown[pc] = mode;
            repaint (keyBounds (pc).getSmallestIntegerContainer());
        }
    }

    const auto scale = processor.getInScaleMask();
    if (scale != scaleShown)
    {
        scaleShown = scale;
        repaint();
    }
}

void KeyboardView::paintKey (juce::Graphics& g, int pc)
{
    const float s = typeScale;
    const bool black = isBlack[pc], in = ((scaleShown >> pc) & 1) != 0, lit = pc == litShown, over = pc == hovered;
    const int mode = modesShown[pc];
    auto box = keyBounds (pc);

    // White keys are held apart by a thin line of ink, and a black key cuts a clean edge out of them.
    if (black)
    {
        g.setColour (tone::ink);
        g.fillRect (box.expanded (2.0f * s, 0.0f).withTrimmedBottom (-2.0f * s));
    }
    box = black ? box : box.withTrimmedRight (2.0f * s);

    juce::Colour fill, inkColour;
    const char* word = nullptr;

    if (lit)
    {
        fill = tone::coral;
        inkColour = tone::ink;
    }
    else if (mode == noteRemove)
    {
        // A removed note is simply gone from the keyboard, with only its name left.
        fill = tone::inkDeep;
        inkColour = tone::muted;
        word = "Remove";
    }
    else if (mode == noteBypass)
    {
        fill = black ? tone::keyWhiteOff : tone::muted;
        inkColour = black ? tone::white : tone::ink;
        word = "Bypass";
    }
    else if (in)
    {
        fill = black ? tone::keyBlack : tone::keyWhite;
        inkColour = black ? tone::white : tone::ink;
    }
    else
    {
        fill = black ? tone::keyBlackOff : tone::keyWhiteOff;
        inkColour = black ? tone::faint : tone::muted;
    }

    if (over && ! lit)
        fill = black || mode == noteRemove || ! in ? fill.brighter (0.22f) : fill.darker (0.10f);

    g.setColour (fill);
    g.fillRect (box);

    const float padX = (black ? 8.0f : 12.0f) * s, padY = (black ? 6.0f : 8.0f) * s;
    auto text = box.reduced (padX, padY);
    g.setColour (inkColour);
    g.setFont (font (Weight::medium, (black ? 12.5f : 14.0f) * s));

    if (black)
    {
        auto bottom = text.removeFromBottom (16.0f * s);
        g.drawText (noteNames[pc], bottom, juce::Justification::centredLeft, false);
        if (word != nullptr)
        {
            g.setFont (font (Weight::regular, 11.5f * s));
            g.drawText (word, bottom, juce::Justification::centredRight, false);
        }
    }
    else
    {
        auto bottom = text.removeFromBottom (18.0f * s);
        g.drawText (noteNames[pc], bottom, juce::Justification::centredLeft, false);
        if (word != nullptr)
        {
            g.setFont (font (Weight::regular, 12.5f * s));
            g.drawText (word, bottom, juce::Justification::centredRight, false);
        }
    }
}

void KeyboardView::paint (juce::Graphics& g)
{
    g.fillAll (tone::ink);
    for (int pc = 0; pc < 12; ++pc) if (! isBlack[pc] && g.clipRegionIntersects (keyBounds (pc).getSmallestIntegerContainer())) paintKey (g, pc);
    for (int pc = 0; pc < 12; ++pc) if (isBlack[pc] && g.clipRegionIntersects (keyBounds (pc).expanded (3.0f).getSmallestIntegerContainer())) paintKey (g, pc);
}
