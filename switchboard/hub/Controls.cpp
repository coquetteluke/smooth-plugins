#include "Controls.h"
#include <cmath>

namespace hub
{
namespace
{
    constexpr float knobRadius = 23.0f;
    constexpr float arcStart = -2.35619449f, arcEnd = 2.35619449f;   // 270 degrees, the gap at the bottom

    float textWidth (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

    // A bipolar control (default in the middle, like a pan) lights from the middle; any other from the bottom.
    double anchorOf (const Param& p) { return std::abs (p.def - 0.5) < 0.02 ? 0.5 : 0.0; }

    // The widget zone of a control on a line: under the label, above the value.
    juce::Rectangle<int> lineWidget (juce::Rectangle<int> r) { return r.withTrimmedTop (22).withHeight (52); }

    void fillPill (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        g.setColour (c);
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    }
}

// ---- a continuous control: a knob on a line ----
class Knob final : public ControlView
{
public:
    using ControlView::ControlView;

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        auto r = getLocalBounds();
        drawLabel (g, r.removeFromTop (18));
        const auto c = lineWidget (getLocalBounds()).toFloat().getCentre();
        const double v = shownValue();
        const float av = arcStart + (float) v * (arcEnd - arcStart);
        const float aa = arcStart + (float) anchorOf (p) * (arcEnd - arcStart);
        const juce::PathStrokeType stroke (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Path track;
        track.addCentredArc (c.x, c.y, knobRadius, knobRadius, 0.0f, arcStart, arcEnd, true);
        g.setColour (tint (hover && isEnabled() ? look::colour::hair.darker (0.15f) : look::colour::hair));
        g.strokePath (track, stroke);
        if (std::abs (av - aa) > 0.002f)
        {
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, knobRadius, knobRadius, 0.0f, std::min (aa, av), std::max (aa, av), true);
            g.setColour (tint (cord));
            g.strokePath (arc, stroke);
        }
        // The plug in the jack: an ink disc with a white pointer.
        g.setColour (tint (look::colour::ink));
        g.fillEllipse (juce::Rectangle<float> (25.0f, 25.0f).withCentre (c));
        const juce::Point<float> dir (std::sin (av), -std::cos (av));
        g.setColour (tint (juce::Colours::white));
        g.drawLine (juce::Line<float> (c + dir * 3.0f, c + dir * 9.5f), 2.2f);

        g.setColour (tint (look::colour::ink));
        g.setFont (look::font (look::Face::number, 15.5f));
        g.drawFittedText (shownText().isNotEmpty() ? shownText() : juce::String (juce::roundToInt (v * 100.0)) + " %",
                          r.removeFromBottom (24), juce::Justification::centred, 1, 0.75f);
    }

    void mouseDown (const juce::MouseEvent& e) override { last = e.position; dragStart(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto d = e.position - last;
        last = e.position;
        dragBy (d.x - d.y, e.mods.isShiftDown(), 200.0f);
    }
    void mouseUp (const juce::MouseEvent&) override { dragEnd(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (isEnabled()) sendDefault(); }

private:
    juce::Point<float> last;
};

// ---- a continuous control in the All controls list: a slim bar and its value ----
class Bar final : public ControlView
{
public:
    using ControlView::ControlView;

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        auto r = getLocalBounds();
        auto text = r.removeFromRight (juce::jmin (110, r.getWidth() / 3));
        const auto bar = barArea();
        const double v = shownValue(), a = anchorOf (p);
        g.setColour (tint (hover && isEnabled() ? look::colour::hair.darker (0.12f) : look::colour::hair));
        g.fillRoundedRectangle (bar, 3.0f);
        const float xv = bar.getX() + (float) v * bar.getWidth(), xa = bar.getX() + (float) a * bar.getWidth();
        g.setColour (tint (cord));
        g.fillRoundedRectangle (juce::Rectangle<float> (std::min (xa, xv), bar.getY(), std::abs (xv - xa), bar.getHeight()), 3.0f);
        g.setColour (tint (look::colour::ink));
        g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ xv, bar.getCentreY() }));
        g.setFont (look::font (look::Face::number, 15.0f));
        g.drawFittedText (shownText(), text.withTrimmedLeft (10), juce::Justification::centredLeft, 1, 0.75f);
    }

    void mouseDown (const juce::MouseEvent& e) override { last = e.position; dragStart(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto d = e.position - last;
        last = e.position;
        dragBy (d.x - d.y, e.mods.isShiftDown(), juce::jmax (120.0f, barArea().getWidth()));
    }
    void mouseUp (const juce::MouseEvent&) override { dragEnd(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (isEnabled()) sendDefault(); }

private:
    juce::Rectangle<float> barArea() const
    {
        auto r = getLocalBounds();
        r.removeFromRight (juce::jmin (110, r.getWidth() / 3));
        return r.toFloat().reduced (8.0f, 0.0f).withSizeKeepingCentre ((float) r.getWidth() - 16.0f, 6.0f);
    }

    juce::Point<float> last;
};

// ---- Off and On: a switch ----
class Toggle final : public ControlView
{
public:
    using ControlView::ControlView;

    bool on() const { return p.positions() >= 2 ? p.positionOf (shownValue()) >= 1 : shownValue() >= 0.5; }

    juce::String word() const
    {
        if (p.choices.size() == 2)
            return p.choices[on() ? 1 : 0];
        return shownText().isNotEmpty() ? shownText() : juce::String (on() ? "On" : "Off");
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        auto r = getLocalBounds();
        juce::Rectangle<float> pill;
        juce::Rectangle<int> text;
        if (compact)
        {
            pill = r.removeFromLeft (52).toFloat().withSizeKeepingCentre (44.0f, 24.0f).withX (8.0f);
            text = r.withTrimmedLeft (10);
        }
        else
        {
            drawLabel (g, r.removeFromTop (18));
            pill = lineWidget (getLocalBounds()).toFloat().withSizeKeepingCentre (46.0f, 26.0f);
            text = r.removeFromBottom (24);
        }
        fillPill (g, pill, tint (on() ? cord : (hover && isEnabled() ? look::colour::hair.darker (0.15f) : look::colour::hair)));
        const float d = pill.getHeight() - 6.0f;
        g.setColour (tint (juce::Colours::white));
        g.fillEllipse ((on() ? pill.getRight() - 3.0f - d : pill.getX() + 3.0f), pill.getY() + 3.0f, d, d);
        g.setColour (tint (look::colour::ink));
        g.setFont (look::font (look::Face::demi, 14.0f));
        g.drawFittedText (word(), text, compact ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.8f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isEnabled() && ! e.mouseWasDraggedSinceMouseDown() && getLocalBounds().contains (e.getPosition()) && e.getNumberOfClicks() == 1)
            sendValue (on() ? 0.0 : 1.0);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (isEnabled()) sendDefault(); }
};

// ---- two or three short choices: a two-way (three-way) switch ----
class Segments final : public ControlView
{
public:
    using ControlView::ControlView;

    static juce::Font face() { return look::font (look::Face::medium, 13.5f); }

    juce::Rectangle<float> box() const
    {
        auto r = getLocalBounds().toFloat();
        if (! compact)
            return lineWidget (getLocalBounds()).toFloat().withSizeKeepingCentre (r.getWidth(), 32.0f);
        return r.reduced (8.0f, 0.0f).withSizeKeepingCentre (juce::jmin (r.getWidth() - 16.0f, (float) naturalWidth()), 30.0f).withX (8.0f);
    }

    int naturalWidth() const
    {
        float w = 6.0f;
        for (auto& c : p.choices)
            w += textWidth (face(), c) + 22.0f;
        return (int) std::ceil (w);
    }

    // The segments' areas, sized to their words and stretched to the box.
    std::vector<juce::Rectangle<float>> segments() const
    {
        auto b = box().reduced (3.0f);
        std::vector<float> widths;
        float total = 0.0f;
        for (auto& c : p.choices)
        {
            widths.push_back (textWidth (face(), c) + 22.0f);
            total += widths.back();
        }
        std::vector<juce::Rectangle<float>> out;
        float x = b.getX();
        for (auto w : widths)
        {
            const float sw = total > 0.0f ? w / total * b.getWidth() : 0.0f;
            out.push_back ({ x, b.getY(), sw, b.getHeight() });
            x += sw;
        }
        return out;
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        if (! compact)
            drawLabel (g, getLocalBounds().removeFromTop (18));
        const auto b = box();
        g.setColour (tint (look::colour::well));
        g.fillRoundedRectangle (b, 9.0f);
        const int sel = p.positionOf (shownValue());
        const auto segs = segments();
        for (int i = 0; i < (int) segs.size(); ++i)
        {
            if (i == sel)
            {
                g.setColour (tint (look::colour::ink));
                g.fillRoundedRectangle (segs[(size_t) i], 7.0f);
            }
            g.setColour (tint (i == sel ? juce::Colours::white : (hover && i == hot && isEnabled() ? look::colour::ink : look::colour::muted)));
            g.setFont (i == sel ? look::font (look::Face::demi, 13.5f) : face());
            g.drawFittedText (p.choices[i], segs[(size_t) i].toNearestInt(), juce::Justification::centred, 1, 0.75f);
        }
    }

    int segmentAt (juce::Point<float> pos) const
    {
        const auto segs = segments();
        for (int i = 0; i < (int) segs.size(); ++i)
            if (segs[(size_t) i].expanded (0.0f, 8.0f).contains (pos))
                return i;
        return -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = segmentAt (e.position);
        if (h != hot)
        {
            hot = h;
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || e.mouseWasDraggedSinceMouseDown() || e.getNumberOfClicks() > 1)
            return;
        const int k = segmentAt (e.position);
        if (k >= 0 && k != p.positionOf (shownValue()))
            sendValue (p.valueAt (k));
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (isEnabled()) sendDefault(); }

private:
    int hot = -1;
};

// ---- more choices: a menu ----
class Menu final : public ControlView
{
public:
    using ControlView::ControlView;

    juce::Rectangle<float> box() const
    {
        auto r = getLocalBounds().toFloat();
        if (! compact)
            return lineWidget (getLocalBounds()).toFloat().withSizeKeepingCentre (r.getWidth(), 32.0f);
        return r.withSizeKeepingCentre (juce::jmin (r.getWidth() - 16.0f, 230.0f), 30.0f).withX (8.0f);
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        if (! compact)
            drawLabel (g, getLocalBounds().removeFromTop (18));
        const auto b = box();
        g.setColour (tint (hover && isEnabled() ? look::colour::hair : look::colour::well));
        g.fillRoundedRectangle (b, 9.0f);
        const int sel = p.positionOf (shownValue());
        auto inner = b.reduced (12.0f, 0.0f);
        auto chevron = inner.removeFromRight (12.0f);
        g.setColour (tint (look::colour::ink));
        g.setFont (look::font (look::Face::demi, 14.0f));
        g.drawFittedText (juce::isPositiveAndBelow (sel, p.choices.size()) ? p.choices[sel] : shownText(), inner.toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.75f);
        juce::Path v;
        const auto cc = chevron.getCentre();
        v.startNewSubPath (cc.x - 4.5f, cc.y - 2.0f);
        v.lineTo (cc.x, cc.y + 2.5f);
        v.lineTo (cc.x + 4.5f, cc.y - 2.0f);
        g.setColour (tint (look::colour::muted));
        g.strokePath (v, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (! isEnabled())
            return;
        juce::PopupMenu m;
        m.setLookAndFeel (&getLookAndFeel());
        const int sel = p.positionOf (shownValue());
        for (int i = 0; i < p.choices.size(); ++i)
            m.addItem (i + 1, p.choices[i], true, i == sel);
        juce::Component::SafePointer<Menu> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()).withStandardItemHeight (30),
                         [safe] (int chosen)
                         {
                             if (safe != nullptr && chosen > 0)
                                 safe->sendValue (safe->p.valueAt (chosen - 1));
                         });
    }
};

// ---- the base ----
std::unique_ptr<ControlView> ControlView::make (Wire& w, const juce::String& instance, const Param& param, juce::Colour cord, bool compact)
{
    switch (param.kind())
    {
        case Kind::toggle:   return std::unique_ptr<ControlView> (new Toggle (w, instance, param, cord, compact));
        case Kind::segments: return std::unique_ptr<ControlView> (new Segments (w, instance, param, cord, compact));
        case Kind::menu:     return std::unique_ptr<ControlView> (new Menu (w, instance, param, cord, compact));
        case Kind::knob:     break;
    }
    if (compact)
        return std::unique_ptr<ControlView> (new Bar (w, instance, param, cord, compact));
    return std::unique_ptr<ControlView> (new Knob (w, instance, param, cord, compact));
}

int ControlView::lineWidth (const Param& p)
{
    switch (p.kind())
    {
        case Kind::toggle: return 88;
        case Kind::segments:
        {
            float w = 6.0f;
            for (auto& c : p.choices)
                w += textWidth (Segments::face(), c) + 22.0f;
            return juce::jlimit (120, 250, (int) std::ceil (w));
        }
        case Kind::menu:
        {
            float w = 0.0f;
            for (auto& c : p.choices)
                w = std::max (w, textWidth (look::font (look::Face::demi, 14.0f), c));
            return juce::jlimit (132, 210, (int) std::ceil (w) + 52);
        }
        case Kind::knob: break;
    }
    return 98;
}

ControlView::ControlView (Wire& w, juce::String instance, const Param& param, juce::Colour c, bool isCompact)
    : wire (w), inst (std::move (instance)), p (param), cord (c), k (param.kind()), compact (isCompact),
      throttle ([this] (double v, Gesture g) { return wire.send (inst, p.id, v, g); })
{
}

ControlView::~ControlView()
{
    if (dragTold)
        wire.dragging (*this, false);
}

void ControlView::drawLabel (juce::Graphics& g, juce::Rectangle<int> r) const
{
    g.setColour (tint (look::colour::muted));
    g.setFont (look::font (look::Face::medium, 13.0f));
    g.drawFittedText (p.name, r.reduced (2, 0), juce::Justification::centred, 1, 0.8f);
}

void ControlView::update (const Param& q)
{
    const bool changed = q.text != p.text || q.name != p.name || q.choices != p.choices || ! juce::exactlyEqual (q.def, p.def) || (! follow && ! juce::exactlyEqual (q.value, p.value));
    p = q;
    if (changed)
        repaint();
}

void ControlView::settled (const juce::String& commandName)
{
    if (follow && ! throttle.active() && commandName == waitFor)
    {
        follow = false;
        waitFor = {};
        repaint();
    }
}

void ControlView::tick (double now)
{
    if (throttle.active())
    {
        throttle.tick (now);
        afterSend();
    }
    if (needsPaint)
    {
        needsPaint = false;
        repaint();
    }
}

void ControlView::showLocal (double v, const juce::String& commandName)
{
    follow = commandName.isNotEmpty();
    local = v;
    waitFor = commandName;
    repaint();
}

void ControlView::sendValue (double v)
{
    showLocal (v, wire.send (inst, p.id, v, Gesture::tap));
}

void ControlView::sendDefault()
{
    showLocal (p.def, wire.sendText (inst, p.id, "default"));
}

void ControlView::dragStart()
{
    if (! isEnabled() || throttle.active())
        return;
    nameBeforeDrag = throttle.lastName();
    local = raw = shownValue();
    follow = true;
    waitFor = {};
    throttle.press (local, wire.now());
    dragTold = true;
    wire.dragging (*this, true);
}

void ControlView::dragBy (float pixels, bool fine, float pixelsForFullRange)
{
    if (! dragTold || pixels == 0.0f)
        return;
    raw = juce::jlimit (0.0, 1.0, raw + (double) pixels / (double) (pixelsForFullRange * (fine ? 8.0f : 1.0f)));
    const double v = p.snapped (raw);
    if (juce::exactlyEqual (v, local))
        return;
    local = v;
    throttle.drag (v, wire.now());
    needsPaint = true;   // painted on the window's next 30 Hz tick
}

void ControlView::dragEnd()
{
    if (! dragTold)
        return;
    throttle.release (wire.now());
    afterSend();
    repaint();
}

// Once the drag is over and its last command went out, the control waits for that command's answer (or shows the card at once when
// the drag sent nothing).
void ControlView::afterSend()
{
    if (throttle.active() || ! dragTold)
        return;
    dragTold = false;
    const auto name = throttle.lastName();
    if (name.isEmpty() || name == nameBeforeDrag)
    {
        follow = false;
        waitFor = {};
        repaint();
    }
    else
    {
        waitFor = name;
    }
    wire.dragging (*this, false);
}
} // namespace hub
