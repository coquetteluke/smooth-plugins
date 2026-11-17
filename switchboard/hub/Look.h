#pragma once
// Switchboard's look: an operator's patch bay, flat. A dark rail on top, a light enamel board below, one white line per plug-in like
// a row of jacks, and every plug-in its own cord colour. The cord colour only ever means "this plug-in's value"; anything you press
// that is not a value is ink. Type is Avenir Next for words and DIN Alternate for the numbers, both part of macOS.
#include <juce_gui_basics/juce_gui_basics.h>

namespace hub::look
{
namespace colour
{
    inline const juce::Colour rail      { 0xff14171b };   // the top bar
    inline const juce::Colour railText  { 0xfff3f5f7 };
    inline const juce::Colour railMuted { 0xff9aa3ad };
    inline const juce::Colour railPill  { 0xff252a30 };
    inline const juce::Colour railHover { 0xff30363d };
    inline const juce::Colour board     { 0xffe6e9ed };   // the enamel the lines sit on
    inline const juce::Colour line      { 0xfffbfcfd };   // one plug-in
    inline const juce::Colour ink       { 0xff14181d };
    inline const juce::Colour muted     { 0xff5a6370 };
    inline const juce::Colour faint     { 0xff8a939e };
    inline const juce::Colour hair      { 0xffdce1e7 };   // knob tracks, switch off, separators
    inline const juce::Colour well      { 0xffeef1f4 };   // boxes on a line
    inline const juce::Colour warn      { 0xffc0392b };   // "not responding"
}

// Paint calls since start, for the CPU check (message thread only).
inline int paints = 0;

enum class Face { regular, medium, demi, bold, number };

inline juce::Font font (Face f, float size)
{
    const char* family = f == Face::number ? "DIN Alternate" : "Avenir Next";
    const char* style = f == Face::regular ? "Regular" : f == Face::medium ? "Medium" : f == Face::demi ? "Demi Bold" : "Bold";
    return juce::Font (juce::FontOptions (family, style, 12.0f).withPointHeight (size));
}

// Caps with a little air, for section names on the board.
inline juce::Font caps (float size) { return juce::Font (juce::FontOptions ("Avenir Next", "Demi Bold", 12.0f).withPointHeight (size).withKerningFactor (0.08f)); }

// One cord colour per plug-in, the same every time. Luke's plug-ins get their own; anything else a colour from its name.
inline juce::Colour cordFor (const juce::String& plugin)
{
    static const std::pair<const char*, juce::uint32> known[] = {
        { "Tune Pro", 0xffe5484d }, { "Velvet Rope", 0xffd4940a }, { "Key", 0xff3e63dd }, { "Punch", 0xff39424e },
        { "Duo", 0xff0a8fc2 }, { "Tape Chorus", 0xff12a594 }, { "Tape Color", 0xffee6a1f }, { "Tape Flanger", 0xff5b5bd6 },
        { "Tune", 0xfff0587a }, { "Glitter", 0xffd6409f }, { "Throat", 0xffb5472a }, { "Alter Ego", 0xff2f9e5f },
        { "Cutup", 0xff6f9e12 }, { "Choir Loft", 0xff9a4bd1 } };
    for (auto& k : known)
        if (plugin == k.first)
            return juce::Colour (k.second);
    static const juce::uint32 others[] = { 0xffe5484d, 0xffee6a1f, 0xffd4940a, 0xff2f9e5f, 0xff12a594, 0xff0a8fc2, 0xff3e63dd, 0xff5b5bd6, 0xff9a4bd1, 0xffd6409f };
    const auto h = (juce::uint32) plugin.hashCode();
    return juce::Colour (others[h % (sizeof others / sizeof others[0])]);
}

// A jack with a plug in it: a ring of the cord colour around an ink hole.
inline void drawJack (juce::Graphics& g, juce::Point<float> centre, float radius, juce::Colour cord, juce::Colour hole = colour::ink)
{
    g.setColour (cord);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));
    g.setColour (hole);
    g.fillEllipse (juce::Rectangle<float> (radius * 0.9f, radius * 0.9f).withCentre (centre));
}

// The mark: two jacks joined by a patch cord.
inline void drawMark (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float rad = r.getHeight() * 0.22f;
    const juce::Point<float> a (r.getX() + rad, r.getBottom() - rad), b (r.getRight() - rad, r.getY() + rad);
    juce::Path cord;
    cord.startNewSubPath (a);
    cord.cubicTo (a.translated (0.0f, -r.getHeight() * 0.9f), b.translated (0.0f, r.getHeight() * 0.9f), b);
    g.setColour (juce::Colour (0xffe5484d));
    g.strokePath (cord, juce::PathStrokeType (rad * 0.62f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    drawJack (g, a, rad, juce::Colour (0xffe5484d), colour::rail);
    drawJack (g, b, rad, juce::Colour (0xff3e63dd), colour::rail);
}

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, colour::line);
        setColour (juce::PopupMenu::textColourId, colour::ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, colour::well);
        setColour (juce::PopupMenu::highlightedTextColourId, colour::ink);
        setColour (juce::TextEditor::backgroundColourId, colour::line);
        setColour (juce::TextEditor::textColourId, colour::ink);
        setColour (juce::TextEditor::highlightColourId, juce::Colour (0xff3e63dd).withAlpha (0.25f));
        setColour (juce::TextEditor::highlightedTextColourId, colour::ink);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::CaretComponent::caretColourId, colour::ink);
        setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ScrollBar::thumbColourId, colour::faint);
    }

    juce::Font getPopupMenuFont() override { return font (Face::medium, 14.0f); }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (colour::line);
        g.setColour (colour::hair);
        g.drawRect (0, 0, w, h, 1);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                            bool, const juce::String& text, const juce::String&, const juce::Drawable*, const juce::Colour*) override
    {
        if (isSeparator)
        {
            g.setColour (colour::hair);
            g.fillRect (area.reduced (12, 0).withSizeKeepingCentre (area.getWidth() - 24, 1));
            return;
        }
        if (isHighlighted && isActive)
        {
            g.setColour (colour::well);
            g.fillRoundedRectangle (area.reduced (4, 1).toFloat(), 6.0f);
        }
        auto r = area.withTrimmedLeft (12).withTrimmedRight (14);
        auto mark = r.removeFromLeft (18);
        if (isTicked)
        {
            g.setColour (colour::ink);
            g.fillEllipse (mark.toFloat().withSizeKeepingCentre (7.0f, 7.0f));
        }
        g.setColour (isActive ? colour::ink : colour::faint);
        g.setFont (isActive ? getPopupMenuFont() : caps (11.0f));
        g.drawText (isActive ? text : text.toUpperCase(), r, juce::Justification::centredLeft, true);
    }

    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& text) override
    {
        g.setColour (colour::faint);
        g.setFont (caps (11.0f));
        g.drawText (text.toUpperCase(), area.withTrimmedLeft (30).withTrimmedRight (14), juce::Justification::bottomLeft, true);
    }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardHeight, int& w, int& h) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardHeight, w, h);
        h = isSeparator ? 9 : 30;
        w = juce::jmax (w + 24, 200);
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize, bool over, bool down) override
    {
        auto track = juce::Rectangle<int> (x, y, w, h).toFloat();
        const auto thumb = vertical ? track.withY ((float) thumbStart).withHeight ((float) thumbSize) : track.withX ((float) thumbStart).withWidth ((float) thumbSize);
        g.setColour (colour::faint.withAlpha (over || down ? 0.75f : 0.45f));
        g.fillRoundedRectangle (thumb.reduced (2.0f), 3.0f);
    }

    int getDefaultScrollbarWidth() override { return 10; }

    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&) override
    {
        g.setColour (colour::line);
        g.fillRoundedRectangle (juce::Rectangle<int> (w, h).toFloat(), 9.0f);
    }

    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e) override
    {
        g.setColour (e.hasKeyboardFocus (true) ? colour::ink.withAlpha (0.55f) : colour::hair);
        g.drawRoundedRectangle (juce::Rectangle<int> (w, h).toFloat().reduced (0.5f), 9.0f, 1.0f);
    }

    void drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging) override
    {
        g.setColour (colour::faint.withAlpha (over || dragging ? 0.9f : 0.5f));
        for (int i = 0; i < 2; ++i)
        {
            const float inset = 4.0f + (float) i * 4.0f;
            g.drawLine ((float) w - inset, (float) h - 3.0f, (float) w - 3.0f, (float) h - inset, 1.2f);
        }
    }
};
} // namespace hub::look
