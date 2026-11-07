#include "Theme.h"

#include "WindowAssets.h"

namespace ui
{
    namespace
    {
        // Smooth value noise, so the glaze has soft pooled runs instead of pixel grain.
        struct Noise
        {
            explicit Noise (juce::int64 seed)
            {
                juce::Random r (seed);
                for (auto& v : table)
                    v = r.nextFloat();
            }

            float at (float x, float y) const noexcept
            {
                const int xi = (int) std::floor (x), yi = (int) std::floor (y);
                float fx = x - (float) xi, fy = y - (float) yi;
                fx = fx * fx * (3.0f - 2.0f * fx);
                fy = fy * fy * (3.0f - 2.0f * fy);
                const float a = lookup (xi, yi), b = lookup (xi + 1, yi), c = lookup (xi, yi + 1), d = lookup (xi + 1, yi + 1);
                return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
            }

            float lookup (int x, int y) const noexcept
            {
                return table[(size_t) (((x & 255) + (y & 255) * 57) & 1023)];
            }

            std::array<float, 1024> table {};
        };

        juce::Image makeGlaze (int w, int h)
        {
            const Noise noise (20261107);
            juce::Image image (juce::Image::RGB, w, h, false);
            juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);
            juce::Random grain (7);

            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    // Stretched vertically: a glaze runs down as it melts.
                    const float fx = (float) x, fy = (float) y;
                    const float n = 0.55f * noise.at (fx / 190.0f, fy / 340.0f)
                                  + 0.30f * noise.at (fx / 70.0f + 11.0f, fy / 130.0f + 4.0f)
                                  + 0.15f * noise.at (fx / 24.0f + 3.0f, fy / 46.0f + 19.0f);
                    const float t = juce::jlimit (0.0f, 1.0f, 0.5f + (n - 0.5f) * 1.25f);

                    auto c = t < 0.6f ? tone::lapisDark.interpolatedWith (tone::lapis, t / 0.6f)
                                      : tone::lapis.interpolatedWith (tone::lapisLight, (t - 0.6f) / 0.4f);

                    const float fine = (grain.nextFloat() - 0.5f) * 0.012f;
                    c = fine > 0.0f ? c.brighter (fine * 2.0f) : c.darker (-fine * 2.0f);
                    pixels.setPixelColour (x, y, c);
                }
            }

            return image;
        }

        juce::Path makeFlecks (int w, int h)
        {
            // Lapis carries tiny specks of pyrite. They only show up close to.
            juce::Path p;
            juce::Random r (1117);

            for (int i = 0; i < 150; ++i)
            {
                const float size = 0.5f + 0.8f * r.nextFloat() * r.nextFloat();
                p.addEllipse ((float) r.nextInt (w), (float) r.nextInt (h), size, size);
            }

            return p;
        }
    }

    Assets::Assets()
        : regular (juce::Typeface::createSystemTypefaceFor (WindowAssets::SourceSansProRegular_otf, (size_t) WindowAssets::SourceSansProRegular_otfSize)),
          semibold (juce::Typeface::createSystemTypefaceFor (WindowAssets::SourceSansProSemibold_otf, (size_t) WindowAssets::SourceSansProSemibold_otfSize)),
          bold (juce::Typeface::createSystemTypefaceFor (WindowAssets::SourceSansProBold_otf, (size_t) WindowAssets::SourceSansProBold_otfSize)),
          glaze (makeGlaze (1160, 760)),
          flecks (makeFlecks (1160, 760))
    {
    }

    juce::Font font (Weight weight, float height)
    {
        juce::SharedResourcePointer<Assets> assets;
        const auto& face = weight == Weight::regular ? assets->regular : (weight == Weight::semibold ? assets->semibold : assets->bold);
        return juce::Font (juce::FontOptions (face).withHeight (height));
    }

    juce::String hertzText (float hz)
    {
        if (hz >= 10000.0f)
            return juce::String (hz / 1000.0f, 1) + " kHz";

        if (hz >= 1000.0f)
            return juce::String (hz / 1000.0f, 2) + " kHz";

        return juce::String (juce::roundToInt (hz)) + " Hz";
    }

    LookAndFeel::LookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, tone::lapisDark);
        setColour (juce::PopupMenu::textColourId, tone::bone);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, tone::raised);
        setColour (juce::PopupMenu::highlightedTextColourId, tone::bone);
        setColour (juce::Label::textColourId, tone::bone);
        setColour (juce::Label::textWhenEditingColourId, tone::bone);
        setColour (juce::Label::backgroundWhenEditingColourId, tone::well);
        setColour (juce::Label::outlineWhenEditingColourId, tone::bone.withAlpha (0.5f));
        setColour (juce::TextEditor::backgroundColourId, tone::well);
        setColour (juce::TextEditor::textColourId, tone::bone);
        setColour (juce::TextEditor::highlightColourId, tone::raised);
        setColour (juce::TextEditor::highlightedTextColourId, tone::bone);
        setColour (juce::TextEditor::outlineColourId, tone::bone.withAlpha (0.4f));
        setColour (juce::TextEditor::focusedOutlineColourId, tone::bone.withAlpha (0.7f));
        setColour (juce::CaretComponent::caretColourId, tone::bone);
    }

    juce::Font LookAndFeel::getPopupMenuFont()
    {
        return font (Weight::regular, 18.0f);
    }

    juce::Font LookAndFeel::getLabelFont (juce::Label& label)
    {
        return label.getFont();
    }

    void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::well);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), 5.0f);
    }

    void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::bone.withAlpha (0.75f));
        g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), 5.0f, 1.0f);
    }

    void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
    {
        g.fillAll (tone::lapisDark);
        g.setColour (tone::bone.withAlpha (0.18f));
        g.drawRect (0, 0, width, height);
    }

    void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                         bool isTicked, bool, const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*)
    {
        if (isSeparator)
        {
            g.setColour (tone::bone.withAlpha (0.15f));
            g.fillRect (area.getX() + 12, area.getCentreY(), area.getWidth() - 24, 1);
            return;
        }

        if (isHighlighted && isActive)
        {
            g.setColour (tone::raised);
            g.fillRoundedRectangle (area.reduced (4, 1).toFloat(), 4.0f);
        }

        auto r = area.withTrimmedLeft (14).withTrimmedRight (14);

        if (isTicked)
        {
            g.setColour (tone::bone);
            g.fillEllipse (r.removeFromLeft (8).withSizeKeepingCentre (6, 6).toFloat());
            r.removeFromLeft (8);
        }
        else
        {
            r.removeFromLeft (16);
        }

        g.setColour (isActive ? tone::bone : tone::boneFaint);
        g.setFont (getPopupMenuFont());
        g.drawText (text, r, juce::Justification::centredLeft, true);
    }

    void LookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        g.setColour (tone::bone.withAlpha (isMouseOver || isMouseDragging ? 0.7f : 0.3f));

        for (int i = 0; i < 3; ++i)
        {
            const float inset = 3.0f + (float) i * 4.0f;
            g.drawLine ((float) w - inset, (float) h - 2.0f, (float) w - 2.0f, (float) h - inset, 1.2f);
        }
    }
}
