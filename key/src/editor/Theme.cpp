#include "Theme.h"

#include "WindowAssets.h"

namespace keyui
{
    namespace
    {
        struct PathSink final : OutlineSink
        {
            explicit PathSink (juce::Path& p) : path (p) {}

            void moveTo (float x, float y) override  { path.startNewSubPath (x, -y); }
            void lineTo (float x, float y) override  { path.lineTo (x, -y); }
            void quadTo (float cx, float cy, float x, float y) override  { path.quadraticTo (cx, -cy, x, -y); }
            void cubicTo (float ax, float ay, float bx, float by, float x, float y) override  { path.cubicTo (ax, -ay, bx, -by, x, -y); }
            void close() override  { path.closeSubPath(); }

            juce::Path& path;
        };
    }

    Assets::Assets()
        : variable (WindowAssets::Jostlatin_ttf, (size_t) WindowAssets::Jostlatin_ttfSize),
          plain (juce::Typeface::createSystemTypefaceFor (WindowAssets::Jostlatin_ttf, (size_t) WindowAssets::Jostlatin_ttfSize))
    {
    }

    const TextShape& Assets::shape (const juce::String& text, int weight)
    {
        const std::string name = std::to_string (weight) + "|" + text.toStdString();
        auto found = shapes.find (name);

        if (found != shapes.end())
            return found->second;

        // Tempo and tuning readings can run through many values. The window never needs more than a few at once.
        if (shapes.size() > 600)
            shapes.clear();

        auto& made = shapes[name];
        PathSink sink (made.path);
        made.width = variable.outline (text.toRawUTF8(), weight, sink);
        return made;
    }

    float textWidth (const juce::String& text, int weight, float size)
    {
        juce::SharedResourcePointer<Assets> assets;
        return assets->shape (text, weight).width * size * 0.001f;
    }

    float drawText (juce::Graphics& g, const juce::String& text, int weight, float size, float x, float baseline, Align align)
    {
        juce::SharedResourcePointer<Assets> assets;
        const auto& s = assets->shape (text, weight);
        const float scale = size * 0.001f, width = s.width * scale;

        if (align == Align::centre) x -= width * 0.5f;
        if (align == Align::right)  x -= width;

        g.fillPath (s.path, juce::AffineTransform::scale (scale).translated (x, baseline));
        return width;
    }

    void drawTextIn (juce::Graphics& g, const juce::String& text, int weight, float size, juce::Rectangle<float> area, Align align)
    {
        const float x = align == Align::left ? area.getX() : (align == Align::centre ? area.getCentreX() : area.getRight());
        drawText (g, text, weight, size, x, area.getCentreY() + size * 0.35f, align);
    }

    juce::Font typingFont (float size)
    {
        juce::SharedResourcePointer<Assets> assets;
        return juce::Font (juce::FontOptions (assets->plain).withHeight (size * 1.45f));
    }

    bool parseKey (const juce::String& typed, int& key, int& scale)
    {
        auto text = typed.trim().toLowerCase().removeCharacters (" .,");
        constexpr int naturals[] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g

        if (text.isEmpty() || text[0] < 'a' || text[0] > 'g')
            return false;

        int note = naturals[text[0] - 'a'];
        auto rest = text.substring (1);

        if (rest.startsWithChar ('#') || rest.startsWith ("sharp"))
        {
            note += 1;
            rest = rest.startsWithChar ('#') ? rest.substring (1) : rest.substring (5);
        }
        else if (rest.startsWith ("flat"))
        {
            note += 11;
            rest = rest.substring (4);
        }
        else if (rest.startsWithChar ('b'))
        {
            note += 11;
            rest = rest.substring (1);
        }

        int said = -1;

        if (rest == "m" || rest == "min" || rest == "minor")      said = 1;
        else if (rest == "maj" || rest == "major")                said = 0;
        else if (rest.isNotEmpty())                               return false;

        key = note % 12;
        scale = said;
        return true;
    }

    LookAndFeel::LookAndFeel()
    {
        setColour (juce::Label::textColourId, tone::ink);
        setColour (juce::Label::textWhenEditingColourId, tone::ink);
        setColour (juce::Label::backgroundWhenEditingColourId, tone::paper);
        setColour (juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::backgroundColourId, tone::paper);
        setColour (juce::TextEditor::textColourId, tone::ink);
        setColour (juce::TextEditor::highlightColourId, tone::wash);
        setColour (juce::TextEditor::highlightedTextColourId, tone::ink);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::CaretComponent::caretColourId, tone::red);
    }

    juce::Font LookAndFeel::getLabelFont (juce::Label& label)
    {
        return label.getFont();
    }

    void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::paper);
        g.fillRect (0, 0, width, height);
    }

    void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor&)
    {
        g.setColour (tone::red);
        g.fillRect (0, height - 3, width, 3);
    }

    void LookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        g.setColour (tone::ink.withAlpha (isMouseOver || isMouseDragging ? 0.6f : 0.22f));

        for (int i = 0; i < 2; ++i)
        {
            const float inset = 4.0f + (float) i * 4.0f;
            g.drawLine ((float) w - inset, (float) h - 3.0f, (float) w - 3.0f, (float) h - inset, 1.0f);
        }
    }
}
