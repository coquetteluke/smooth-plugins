#include "Theme.h"
#include "WindowAssets.h"

namespace fxui
{
    const std::array<Module, numModules>& modules()
    {
        static const std::array<Module, numModules> list { {
            { "Pitch",      "pitchOn",   juce::Colour (0xffff8fc7), "Moves the whole voice up or down. Turn on Moves Formants for a chipmunk or a monster." },
            { "Motion",     "motionOn",  juce::Colour (0xffffb020), "Walks each note through a pattern in key, in time with the song." },
            { "Throat",     "throatOn",  juce::Colour (0xffff7a45), "Makes the throat longer or shorter. Longer sounds bigger and darker." },
            { "Harmony",    "harmonyOn", juce::Colour (0xffd98cff), "Adds one or two voices at a set distance from the lead." },
            { "Doubler",    "doublerOn", juce::Colour (0xff8fdcff), "Adds two copies, a little late and a little out of tune, for a wider sound." },
            { "Vocoder",    "vocoderOn", juce::Colour (0xff6be05a), "Adds a synth tone at the voice's pitch, shaped by the words." },
            { "Distortion", "distOn",    juce::Colour (0xffff4b3e), "Pushes the voice until it breaks up." },
            { "Filter",     "filterOn",  juce::Colour (0xff14d8c4), "Keeps only part of the voice's range, with a sweep in time if you want one." },
            { "Ring",       "ringOn",    juce::Colour (0xffe6f04a), "Multiplies the voice by a tone, for a metallic robot sound." },
            { "Delay",      "delayOn",   juce::Colour (0xff4a9dff), "Repeats the voice in time with the song." },
            { "Reverb",     "reverbOn",  juce::Colour (0xff8b7dff), "Puts the voice in a space." },
        } };
        return list;
    }

    juce::Colour panelTint (juce::Colour moduleColour, bool on)
    {
        return tone::black.interpolatedWith (moduleColour, on ? 0.15f : 0.07f);
    }

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
        setColour (juce::PopupMenu::backgroundColourId, tone::lifted);
        setColour (juce::PopupMenu::textColourId, tone::white);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, tone::white.withAlpha (0.1f));
        setColour (juce::PopupMenu::highlightedTextColourId, tone::white);
        setColour (juce::Label::textColourId, tone::white);
        setColour (juce::Label::textWhenEditingColourId, tone::white);
        setColour (juce::Label::backgroundWhenEditingColourId, tone::black);
        setColour (juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::backgroundColourId, tone::black);
        setColour (juce::TextEditor::textColourId, tone::white);
        setColour (juce::TextEditor::highlightColourId, tone::magenta.withAlpha (0.45f));
        setColour (juce::TextEditor::highlightedTextColourId, tone::white);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::CaretComponent::caretColourId, tone::magenta);
    }

    juce::Font LookAndFeel::getPopupMenuFont()               { return font (Weight::regular, 14.0f); }
    juce::Font LookAndFeel::getLabelFont (juce::Label& label) { return label.getFont(); }

    void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::black);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), 5.0f);
    }

    void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::magenta);
        g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), 5.0f, 1.0f);
    }

    void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int, int)
    {
        g.fillAll (tone::lifted);
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
            g.setColour (tone::magenta);
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
