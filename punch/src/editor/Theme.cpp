#include "Theme.h"
#include "WindowAssets.h"

namespace ui
{
    Assets::Assets()
        : regular (juce::Typeface::createSystemTypefaceFor (WindowAssets::InterRegular_otf, (size_t) WindowAssets::InterRegular_otfSize)),
          medium (juce::Typeface::createSystemTypefaceFor (WindowAssets::InterMedium_otf, (size_t) WindowAssets::InterMedium_otfSize))
    {
    }

    juce::Font font (Weight weight, float size, bool tabular)
    {
        juce::SharedResourcePointer<Assets> assets;

        // Inter's line height is 1.21 of its type size. Display sizes are set tighter, as the typeface asks for.
        const float tracking = size >= 60.0f ? -0.035f : (size >= 26.0f ? -0.02f : (size >= 18.0f ? -0.01f : 0.0f));
        auto options = juce::FontOptions (weight == Weight::regular ? assets->regular : assets->medium)
                           .withHeight (size * 1.21f)
                           .withKerningFactor (tracking);
        if (tabular)
            options = options.withFeatureEnabled (juce::FontFeatureTag ("tnum"));
        return juce::Font (options);
    }

    LookAndFeel::LookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, tone::menu);
        setColour (juce::PopupMenu::textColourId, tone::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, tone::text.withAlpha (0.1f));
        setColour (juce::PopupMenu::highlightedTextColourId, tone::text);
        setColour (juce::Label::textColourId, tone::text);
        setColour (juce::Label::textWhenEditingColourId, tone::text);
        setColour (juce::Label::backgroundWhenEditingColourId, tone::well);
        setColour (juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::backgroundColourId, tone::well);
        setColour (juce::TextEditor::textColourId, tone::text);
        setColour (juce::TextEditor::highlightColourId, tone::hot.withAlpha (0.35f));
        setColour (juce::TextEditor::highlightedTextColourId, tone::text);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::CaretComponent::caretColourId, tone::hot);
    }

    juce::Font LookAndFeel::getPopupMenuFont() { return font (Weight::regular, 14.0f); }
    juce::Font LookAndFeel::getLabelFont (juce::Label& label) { return label.getFont(); }

    void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::well);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), 5.0f);
    }

    void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::hot.withAlpha (0.6f));
        g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), 5.0f, 1.0f);
    }

    void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int, int)
    {
        g.fillAll (tone::menu);
    }

    void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                         bool isTicked, bool, const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*)
    {
        if (isSeparator)
            return;

        if (isHighlighted && isActive)
        {
            g.setColour (tone::text.withAlpha (0.09f));
            g.fillRoundedRectangle (area.reduced (4, 1).toFloat(), 4.0f);
        }

        auto r = area.withTrimmedLeft (12).withTrimmedRight (14);
        auto mark = r.removeFromLeft (18);

        if (isTicked)
        {
            g.setColour (tone::text);
            g.fillRoundedRectangle (mark.withSizeKeepingCentre (8, 2).withX (mark.getX()).toFloat(), 1.0f);
        }

        g.setColour (isActive ? tone::text : tone::faint);
        g.setFont (getPopupMenuFont());
        g.drawText (text, r, juce::Justification::centredLeft, true);
    }

    void LookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        g.setColour (tone::text.withAlpha (isMouseOver || isMouseDragging ? 0.55f : 0.2f));

        for (int i = 0; i < 2; ++i)
        {
            const float inset = 4.0f + (float) i * 4.0f;
            g.drawLine ((float) w - inset, (float) h - 3.0f, (float) w - 3.0f, (float) h - inset, 1.0f);
        }
    }
}
