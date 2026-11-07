#include "Theme.h"

#include "WindowAssets.h"

namespace ui
{
    Assets::Assets()
        : light (juce::Typeface::createSystemTypefaceFor (WindowAssets::WorkSansLight_ttf, (size_t) WindowAssets::WorkSansLight_ttfSize)),
          medium (juce::Typeface::createSystemTypefaceFor (WindowAssets::WorkSansMedium_ttf, (size_t) WindowAssets::WorkSansMedium_ttfSize))
    {
    }

    juce::Font font (Weight weight, float size)
    {
        juce::SharedResourcePointer<Assets> assets;

        // Work Sans' line height is 1.17 of its type size. Large numbers sit a little tighter.
        const float tracking = size >= 24.0f ? -0.025f : (size >= 17.0f ? -0.01f : 0.0f);
        return juce::Font (juce::FontOptions (weight == Weight::light ? assets->light : assets->medium)
                               .withHeight (size * 1.17f)
                               .withKerningFactor (tracking));
    }

    juce::String minusSign()
    {
        return juce::String::fromUTF8 ("\xe2\x88\x92");
    }

    LookAndFeel::LookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, tone::deep);
        setColour (juce::PopupMenu::textColourId, tone::mint);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, tone::mint.withAlpha (0.1f));
        setColour (juce::PopupMenu::highlightedTextColourId, tone::mint);
        setColour (juce::Label::textColourId, tone::mint);
        setColour (juce::Label::textWhenEditingColourId, tone::mint);
        setColour (juce::Label::backgroundWhenEditingColourId, tone::deep);
        setColour (juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::backgroundColourId, tone::deep);
        setColour (juce::TextEditor::textColourId, tone::mint);
        setColour (juce::TextEditor::highlightColourId, tone::aqua.withAlpha (0.45f));
        setColour (juce::TextEditor::highlightedTextColourId, tone::mint);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::CaretComponent::caretColourId, tone::mint);
    }

    juce::Font LookAndFeel::getPopupMenuFont()
    {
        return font (Weight::medium, 14.0f);
    }

    juce::Font LookAndFeel::getLabelFont (juce::Label& label)
    {
        return label.getFont();
    }

    void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::deep);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), 4.0f);
    }

    void LookAndFeel::drawTextEditorOutline (juce::Graphics&, int, int, juce::TextEditor&) {}

    void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int, int)
    {
        g.fillAll (tone::deep);
    }

    void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                         bool isTicked, bool, const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*)
    {
        if (isSeparator)
            return;

        if (isHighlighted && isActive)
        {
            g.setColour (tone::mint.withAlpha (0.1f));
            g.fillRoundedRectangle (area.reduced (4, 1).toFloat(), 4.0f);
        }

        auto r = area.withTrimmedLeft (14).withTrimmedRight (14);

        if (isTicked)
        {
            g.setColour (tone::aqua);
            g.fillEllipse (r.removeFromLeft (6).withSizeKeepingCentre (6, 6).toFloat());
            r.removeFromLeft (8);
        }
        else
        {
            r.removeFromLeft (14);
        }

        g.setColour (isActive ? tone::mint : tone::faint);
        g.setFont (getPopupMenuFont());
        g.drawText (text, r, juce::Justification::centredLeft, true);
    }

    void LookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        g.setColour (tone::mint.withAlpha (isMouseOver || isMouseDragging ? 0.55f : 0.2f));

        for (int i = 0; i < 2; ++i)
        {
            const float inset = 4.0f + (float) i * 4.0f;
            g.drawLine ((float) w - inset, (float) h - 3.0f, (float) w - 3.0f, (float) h - inset, 1.0f);
        }
    }
}
