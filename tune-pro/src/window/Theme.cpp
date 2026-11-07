#include "Theme.h"
#include "WindowAssets.h"

namespace tuneui
{
    Assets::Assets()
        : regular (juce::Typeface::createSystemTypefaceFor (WindowAssets::InterRegular_otf, (size_t) WindowAssets::InterRegular_otfSize)),
          medium (juce::Typeface::createSystemTypefaceFor (WindowAssets::InterMedium_otf, (size_t) WindowAssets::InterMedium_otfSize))
    {
    }

    juce::Font font (Weight weight, float size)
    {
        juce::SharedResourcePointer<Assets> assets;
        // Inter's line height is 1.21 of its type size. Display sizes are set tighter, as the typeface asks for.
        const float tracking = size >= 26.0f ? -0.022f : (size >= 18.0f ? -0.011f : 0.0f);
        return juce::Font (juce::FontOptions (weight == Weight::regular ? assets->regular : assets->medium)
                               .withHeight (size * 1.21f)
                               .withKerningFactor (tracking));
    }

    LookAndFeel::LookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, tone::raised);
        setColour (juce::PopupMenu::textColourId, tone::white);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, tone::white.withAlpha (0.1f));
        setColour (juce::PopupMenu::highlightedTextColourId, tone::white);
        setColour (juce::Label::textColourId, tone::white);
        setColour (juce::Label::textWhenEditingColourId, tone::white);
        setColour (juce::Label::backgroundWhenEditingColourId, tone::inkDeep);
        setColour (juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::backgroundColourId, tone::inkDeep);
        setColour (juce::TextEditor::textColourId, tone::white);
        setColour (juce::TextEditor::highlightColourId, tone::coral.withAlpha (0.45f));
        setColour (juce::TextEditor::highlightedTextColourId, tone::white);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::CaretComponent::caretColourId, tone::coral);
    }

    juce::Font LookAndFeel::getPopupMenuFont()               { return font (Weight::regular, 14.0f); }
    juce::Font LookAndFeel::getLabelFont (juce::Label& label) { return label.getFont(); }

    void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::inkDeep);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), 5.0f);
    }

    void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::coral);
        g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), 5.0f, 1.0f);
    }

    void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int, int)
    {
        g.fillAll (tone::raised);
    }

    void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                         bool isTicked, bool, const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*)
    {
        if (isSeparator)
            return;

        if (isHighlighted && isActive)
        {
            g.setColour (tone::white.withAlpha (0.1f));
            g.fillRoundedRectangle (area.reduced (4, 1).toFloat(), 4.0f);
        }

        auto r = area.withTrimmedLeft (14).withTrimmedRight (14);
        auto dot = r.removeFromLeft (14);

        if (isTicked)
        {
            g.setColour (tone::coral);
            g.fillEllipse (dot.withSizeKeepingCentre (6, 6).toFloat());
        }

        g.setColour (isActive ? tone::white : tone::faint);
        g.setFont (getPopupMenuFont());
        g.drawText (text, r, juce::Justification::centredLeft, true);
    }

    void LookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        g.setColour (tone::white.withAlpha (isMouseOver || isMouseDragging ? 0.5f : 0.18f));

        for (int i = 0; i < 2; ++i)
        {
            const float inset = 4.0f + (float) i * 4.0f;
            g.drawLine ((float) w - inset, (float) h - 3.0f, (float) w - 3.0f, (float) h - inset, 1.0f);
        }
    }
}
