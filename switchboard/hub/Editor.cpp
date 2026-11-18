#include "Editor.h"

namespace hub
{
namespace
{
    float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }
    juce::String trackTitle (const juce::String& track) { return track.isEmpty() ? juce::String ("No track name yet") : track; }
    juce::String backQuote() { return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xb9")); }   // the single left angle quote
}

// ---- the back button on the rail: a left chevron and where it goes ----
class RailButton final : public juce::Component
{
public:
    RailButton() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    std::function<void()> onClick;

    void setText (const juce::String& newText)
    {
        if (newText == text)
            return;
        text = newText;
        setTitle (backQuote() + " " + text);
        repaint();
    }

    const juce::String& getText() const { return text; }

    int idealWidth() const { return (int) std::ceil (widthOf (font(), text) + 14.0f + 10.0f + 6.0f + 18.0f); }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        const auto r = getLocalBounds().toFloat();
        g.setColour (hover ? look::colour::railHover : look::colour::railPill);
        g.fillRoundedRectangle (r, 10.0f);
        auto inner = r.withTrimmedLeft (14.0f).withTrimmedRight (18.0f);
        const auto chevron = inner.removeFromLeft (10.0f);
        const auto c = chevron.getCentre();
        juce::Path v;
        v.startNewSubPath (c.x + 2.5f, c.y - 5.5f);
        v.lineTo (c.x - 3.0f, c.y);
        v.lineTo (c.x + 2.5f, c.y + 5.5f);
        g.setColour (look::colour::railText);
        g.strokePath (v, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        inner.removeFromLeft (6.0f);
        g.setFont (font());
        g.drawFittedText (text, inner.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick && getLocalBounds().contains (e.getPosition()) && ! e.mouseWasDraggedSinceMouseDown())
            onClick();
    }

private:
    static juce::Font font() { return look::font (look::Face::demi, 14.5f); }

    juce::String text;
    bool hover = false;
};

// ---- "All 12 controls" on a plug-in's line ----
class TextLink final : public juce::Component
{
public:
    TextLink() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    std::function<void()> onClick;
    juce::Colour cord { look::colour::ink };

    void setText (const juce::String& t)
    {
        if (t != text)
        {
            text = t;
            setTitle (t);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        const auto f = look::font (look::Face::demi, 13.5f);
        const float w = widthOf (f, text);
        auto r = getLocalBounds().toFloat();
        g.setColour (look::colour::ink);
        g.setFont (f);
        g.drawText (text, r.withWidth (w + 2.0f), juce::Justification::centredLeft, false);
        if (hover)
            g.fillRect (r.getX(), r.getCentreY() + 9.0f, w, 1.2f);
        const float x = r.getX() + w + 8.0f, y = r.getCentreY();
        juce::Path arrow;
        arrow.startNewSubPath (x, y - 4.0f);
        arrow.lineTo (x + 4.0f, y);
        arrow.lineTo (x, y + 4.0f);
        g.setColour (cord);
        g.strokePath (arrow, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick && getLocalBounds().contains (e.getPosition()))
            onClick();
    }

private:
    juce::String text;
    bool hover = false;
};

// ---- one plug-in: its jack and name on the left, its main controls to the right ----
class PluginLine final : public juce::Component
{
public:
    static constexpr int padTop = 13, padBottom = 13, rowGap = 8, cellGap = 6, minHeight = 126;

    PluginLine (Wire& w, std::function<void (const juce::String&)> openAll) : wire (w), onAll (std::move (openAll))
    {
        all.onClick = [this]
        {
            const auto id = instanceId;   // a copy: what it opens may change this line
            if (onAll)
                onAll (id);
        };
        addAndMakeVisible (all);
    }

    const juce::String& id() const { return instanceId; }
    const juce::String& title() const { return name; }
    bool isLive() const { return liveNow; }
    juce::Component* allLink() { return &all; }

    // Takes the instance's card. True when the line's controls changed, so its height may have.
    bool update (const Instance& i, bool isLive)
    {
        bool structure = false;
        if (i.revision != revision || i.name != name)
        {
            revision = i.revision;
            const bool pluginChanged = i.plugin != plugin;
            const int total = (int) i.params.size();
            const bool textChanged = i.name != name || i.version != version || pluginChanged || total != count;
            instanceId = i.id;
            name = i.name;
            version = i.version;
            count = total;
            if (pluginChanged)
            {
                plugin = i.plugin;
                cord = look::cordFor (plugin);
                all.cord = cord;
            }
            const auto shown = i.shown();
            juce::String key;
            for (auto* p : shown)
                key << p->id << ':' << (int) p->kind() << ';';
            if (key != shownKey || pluginChanged)
            {
                shownKey = key;
                controls.clear();
                for (auto* p : shown)
                {
                    controls.push_back (ControlView::make (wire, instanceId, *p, cord, false));
                    controls.back()->setEnabled (liveNow);
                    addAndMakeVisible (*controls.back());
                }
                structure = true;
            }
            else
            {
                for (size_t k = 0; k < shown.size(); ++k)
                    controls[k]->update (*shown[k]);
            }
            if (textChanged)
            {
                all.setText ("All " + juce::String (count) + (count == 1 ? " control" : " controls"));
                all.setVisible (count > 0);
                repaint();
            }
        }
        if (isLive != liveNow)
        {
            liveNow = isLive;
            for (auto& c : controls)
                c->setEnabled (isLive);
            repaint (plate());
        }
        if (structure)
            resized();
        return structure;
    }

    ControlView* control (const juce::String& param) const
    {
        for (auto& c : controls)
            if (c->param().id == param)
                return c.get();
        return nullptr;
    }

    static int plateWidth (int width) { return width < 640 ? 176 : 214; }

    int heightFor (int width) const
    {
        const int rows = place (width, false);
        return juce::jmax (minHeight, padTop + rows * ControlView::lineHeight + (rows - 1) * rowGap + padBottom);
    }

    void resized() override
    {
        place (getWidth(), true);
        const int pw = plateWidth (getWidth());
        all.setBounds (20, getHeight() - 16 - 24, pw - 30, 24);
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        const auto r = getLocalBounds().toFloat();
        g.setColour (look::colour::line);
        g.fillRoundedRectangle (r, 14.0f);
        const int pw = plateWidth (getWidth());
        g.setColour (look::colour::hair);
        g.fillRect ((float) pw, 18.0f, 1.0f, r.getHeight() - 36.0f);
        // A moving knob repaints only its own patch: skip the text it does not touch (text layout is the costly part).
        if (! g.clipRegionIntersects (plate()) && ! controls.empty())
            return;

        look::drawJack (g, { 36.0f, 36.0f }, 15.0f, liveNow ? cord : look::colour::hair, liveNow ? look::colour::ink : look::colour::faint);
        const juce::Rectangle<int> text (62, 17, pw - 70, 24);
        g.setColour (liveNow ? look::colour::ink : look::colour::faint);
        g.setFont (look::font (look::Face::demi, 18.0f));
        g.drawFittedText (name, text, juce::Justification::centredLeft, 1, 0.75f);
        g.setColour (liveNow ? look::colour::faint : look::colour::warn);
        g.setFont (look::font (liveNow ? look::Face::medium : look::Face::demi, 13.0f));
        g.drawFittedText (liveNow ? version : juce::String ("not responding"), text.translated (0, 23).withHeight (18), juce::Justification::centredLeft, 1, 0.8f);
        if (controls.empty())
        {
            g.setColour (look::colour::faint);
            g.setFont (look::font (look::Face::medium, 14.0f));
            g.drawText ("Its card lists no controls.", getLocalBounds().withTrimmedLeft (pw + 24), juce::Justification::centredLeft, true);
        }
    }

private:
    juce::Rectangle<int> plate() const { return { 0, 0, plateWidth (getWidth()), getHeight() }; }

    // Lays the controls out in rows; returns how many rows.
    int place (int width, bool apply) const
    {
        const int x0 = plateWidth (width) + 14, x1 = width - 14;
        int x = x0, y = padTop, rows = 1;
        for (auto& c : controls)
        {
            const int w = juce::jmin (ControlView::lineWidth (c->param()), juce::jmax (60, x1 - x0));
            if (x > x0 && x + w > x1)
            {
                x = x0;
                y += ControlView::lineHeight + rowGap;
                ++rows;
            }
            if (apply)
                c->setBounds (x, y, w, ControlView::lineHeight);
            x += w + cellGap;
        }
        return rows;
    }

    Wire& wire;
    std::function<void (const juce::String&)> onAll;
    juce::String instanceId, name, version, plugin, shownKey;
    juce::Colour cord { look::colour::ink };
    int count = -1;
    juce::int64 revision = -1;
    bool liveNow = true;
    std::vector<std::unique_ptr<ControlView>> controls;
    TextLink all;
};

// ---- a track's screen: its plug-in lines, in the viewport ----
class Board final : public juce::Component
{
public:
    Board (Wire& w, std::function<void (const juce::String&)> openAll) : wire (w), onAll (std::move (openAll)) {}

    // Shows the instances. Lines are kept by instance id, so a line that stays keeps its controls (and a drag in progress).
    void show (const std::vector<const Instance*>& items, const std::function<bool (const Instance&)>& isLive)
    {
        juce::StringArray key;
        for (auto* i : items)
            key.add (i->id);
        bool relayout = key != order;
        if (relayout)
        {
            order = key;
            std::map<juce::String, std::unique_ptr<PluginLine>> old;
            for (auto& l : lines)
                old[l->id()] = std::move (l);
            lines.clear();
            for (auto* i : items)
            {
                const auto it = old.find (i->id);
                if (it != old.end())
                {
                    lines.push_back (std::move (it->second));
                    old.erase (it);
                }
                else
                {
                    lines.push_back (std::make_unique<PluginLine> (wire, onAll));
                    addAndMakeVisible (*lines.back());
                }
            }
        }
        for (size_t k = 0; k < items.size(); ++k)
            relayout = lines[k]->update (*items[k], isLive (*items[k])) || relayout;
        if (relayout)
            layout (getWidth());
    }

    void layout (int width)
    {
        if (width <= 0)
            return;
        int y = 14;
        for (auto& l : lines)
        {
            const int lh = l->heightFor (width - 32);
            l->setBounds (16, y, width - 32, lh);
            y += lh + 10;
        }
        setSize (width, y + 4);
    }

    PluginLine* line (const juce::String& id) const
    {
        for (auto& l : lines)
            if (l->id() == id)
                return l.get();
        return nullptr;
    }

    juce::StringArray names() const
    {
        juce::StringArray out;
        for (auto& l : lines)
            out.add (l->title());
        return out;
    }

private:
    Wire& wire;
    std::function<void (const juce::String&)> onAll;
    std::vector<std::unique_ptr<PluginLine>> lines;
    juce::StringArray order;
};

// ---- Tracks: one row per track ----
struct Chip
{
    juce::String name;
    juce::Colour cord;
    bool live = true;
    bool operator== (const Chip& o) const { return name == o.name && cord == o.cord && live == o.live; }
};

struct RowData
{
    juce::String track;
    std::vector<Chip> chips;
    bool operator== (const RowData& o) const { return track == o.track && chips == o.chips; }
    bool operator!= (const RowData& o) const { return ! operator== (o); }
};

// A track: its name, and below it its plug-ins in load order, each with its cord colour. A quiet one is greyed and says
// "not responding". The whole row is the button; a chevron on the right says it opens.
class TrackRowView final : public juce::Component
{
public:
    static constexpr int padTop = 15, titleHeight = 26, chipsTop = 45, chipLine = 24, padBottom = 15, rightSpace = 58;

    explicit TrackRowView (std::function<void (const juce::String&)> open) : onOpen (std::move (open))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    const RowData& rowData() const { return data; }

    // True when what it shows changed.
    bool set (const RowData& d)
    {
        if (d == data)
            return false;
        data = d;
        setTitle (trackTitle (data.track));
        repaint();
        return true;
    }

    int heightFor (int width) const { return chipsTop + place (width, nullptr) * chipLine + padBottom; }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        const auto r = getLocalBounds().toFloat();
        g.setColour (hover ? look::colour::well : look::colour::line);
        g.fillRoundedRectangle (r, 14.0f);
        g.setColour (look::colour::ink);
        g.setFont (look::font (look::Face::demi, 18.0f));
        g.drawFittedText (trackTitle (data.track), juce::Rectangle<int> (22, padTop, juce::jmax (40, getWidth() - 22 - rightSpace), titleHeight),
                          juce::Justification::centredLeft, 1, 0.75f);
        place (getWidth(), &g);
        const float x = (float) getWidth() - 30.0f, y = r.getCentreY();
        juce::Path chevron;
        chevron.startNewSubPath (x - 3.0f, y - 6.5f);
        chevron.lineTo (x + 3.5f, y);
        chevron.lineTo (x - 3.0f, y + 6.5f);
        g.setColour (look::colour::muted);
        g.strokePath (chevron, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onOpen && getLocalBounds().contains (e.getPosition()) && ! e.mouseWasDraggedSinceMouseDown())
        {
            const auto track = data.track;   // a copy: what it opens may change this row
            onOpen (track);
        }
    }

private:
    // Lays the plug-ins out in lines under the name, drawing them when g is given. Returns how many lines.
    int place (int width, juce::Graphics* g) const
    {
        const auto nameFont = look::font (look::Face::medium, 14.0f), noteFont = look::font (look::Face::demi, 13.0f);
        const juce::String note ("not responding");
        const float noteW = widthOf (noteFont, note);
        const int x0 = 22, x1 = juce::jmax (x0 + 80, width - rightSpace);
        int x = x0, line = 0;
        for (const auto& c : data.chips)
        {
            const float extra = c.live ? 0.0f : 7.0f + noteW;
            const float nameW = juce::jmin (widthOf (nameFont, c.name), (float) (x1 - x0) - 17.0f - extra);
            const int w = (int) std::ceil (17.0f + nameW + extra);
            if (x > x0 && x + w > x1)
            {
                x = x0;
                ++line;
            }
            if (g != nullptr)
            {
                const int top = chipsTop + line * chipLine;
                const float cy = (float) top + (float) chipLine * 0.5f;
                g->setColour (c.live ? c.cord : look::colour::hair);
                g->fillEllipse ((float) x, cy - 4.5f, 9.0f, 9.0f);
                g->setColour (c.live ? look::colour::muted : look::colour::faint);
                g->setFont (nameFont);
                g->drawFittedText (c.name, juce::Rectangle<int> (x + 17, top, (int) std::ceil (nameW) + 2, chipLine), juce::Justification::centredLeft, 1, 0.8f);
                if (! c.live)
                {
                    g->setColour (look::colour::warn);
                    g->setFont (noteFont);
                    g->drawText (note, juce::Rectangle<int> (x + 17 + (int) std::ceil (nameW) + 7, top, (int) std::ceil (noteW) + 2, chipLine), juce::Justification::centredLeft, false);
                }
            }
            x += w + 24;
        }
        return line + 1;
    }

    std::function<void (const juce::String&)> onOpen;
    RowData data;
    bool hover = false;
};

class TrackList final : public juce::Component
{
public:
    explicit TrackList (std::function<void (const juce::String&)> open) : onOpen (std::move (open)) {}

    // Shows the rows. Rows are kept by track, so only a row that changed repaints.
    void show (const std::vector<RowData>& rows)
    {
        bool same = rows.size() == views.size();
        for (size_t k = 0; same && k < rows.size(); ++k)
            same = views[k]->rowData() == rows[k];
        if (same)
            return;
        std::map<juce::String, std::unique_ptr<TrackRowView>> old;
        for (auto& v : views)
            old[v->rowData().track] = std::move (v);
        views.clear();
        for (const auto& r : rows)
        {
            std::unique_ptr<TrackRowView> v;
            const auto it = old.find (r.track);
            if (it != old.end())
            {
                v = std::move (it->second);
                old.erase (it);
            }
            else
            {
                v = std::make_unique<TrackRowView> (onOpen);
                addAndMakeVisible (*v);
            }
            v->set (r);
            views.push_back (std::move (v));
        }
        layout (getWidth());
    }

    void layout (int width)
    {
        if (width <= 0)
            return;
        int y = 14;
        for (auto& v : views)
        {
            const int h = v->heightFor (width - 32);
            v->setBounds (16, y, width - 32, h);
            y += h + 10;
        }
        setSize (width, y + 4);
    }

    TrackRowView* row (const juce::String& track) const
    {
        for (auto& v : views)
            if (v->rowData().track == track)
                return v.get();
        return nullptr;
    }

    std::vector<RowData> rows() const
    {
        std::vector<RowData> out;
        for (auto& v : views)
            out.push_back (v->rowData());
        return out;
    }

private:
    std::function<void (const juce::String&)> onOpen;
    std::vector<std::unique_ptr<TrackRowView>> views;
};

// ---- what a screen says when it has nothing to show ----
class EmptyState final : public juce::Component
{
public:
    void set (const juce::String& newHeading, const juce::String& newSub)
    {
        if (newHeading != heading || newSub != sub)
        {
            heading = newHeading;
            sub = newSub;
            repaint();
        }
    }

    juce::String text() const { return heading + "\n" + sub; }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        const float cx = (float) getWidth() * 0.5f, cy = (float) centreY() - 46.0f;
        // An empty jack and a plug on a loose cord.
        g.setColour (look::colour::line);
        g.fillEllipse (juce::Rectangle<float> (60.0f, 60.0f).withCentre ({ cx - 34.0f, cy }));
        g.setColour (look::colour::hair);
        g.drawEllipse (juce::Rectangle<float> (60.0f, 60.0f).withCentre ({ cx - 34.0f, cy }), 1.5f);
        g.setColour (look::colour::ink);
        g.fillEllipse (juce::Rectangle<float> (20.0f, 20.0f).withCentre ({ cx - 34.0f, cy }));
        const juce::Colour red (0xffe5484d);
        juce::Path cord;
        cord.startNewSubPath (cx + 44.0f, cy);
        cord.cubicTo (cx + 80.0f, cy, cx + 70.0f, cy + 40.0f, cx + 104.0f, cy + 38.0f);
        g.setColour (red);
        g.strokePath (cord, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillRoundedRectangle (juce::Rectangle<float> (cx + 14.0f, cy - 8.0f, 32.0f, 16.0f), 5.0f);
        g.setColour (look::colour::ink);
        g.fillRoundedRectangle (juce::Rectangle<float> (cx + 2.0f, cy - 3.0f, 14.0f, 6.0f), 2.0f);

        auto r = getLocalBounds().withTop (centreY()).reduced (24, 0);
        g.setColour (look::colour::ink);
        g.setFont (look::font (look::Face::demi, 23.0f));
        g.drawFittedText (heading, r.removeFromTop (34), juce::Justification::centred, 1, 0.75f);
        r.removeFromTop (6);
        g.setColour (look::colour::muted);
        g.setFont (look::font (look::Face::medium, 15.0f));
        g.drawFittedText (sub, r.removeFromTop (44), juce::Justification::centredTop, 2, 0.85f);
    }

private:
    int centreY() const { return juce::jmax (110, getHeight() / 2 - 10); }

    juce::String heading, sub;
};

// ---- the controls screen: every control of one plug-in ----
struct ListRow
{
    juce::String instance, hay;
    Param param;
    juce::Colour cord;
    bool live = true;
};

class RowView final : public juce::Component
{
public:
    explicit RowView (Wire& w) : wire (w) {}

    void set (const ListRow& r)
    {
        const bool same = control != nullptr && r.instance == instance && r.param.id == control->param().id && r.param.kind() == control->kind();
        if (! same)
        {
            control = ControlView::make (wire, r.instance, r.param, r.cord, true);
            addAndMakeVisible (*control);
            instance = r.instance;
        }
        else
        {
            control->update (r.param);
        }
        control->setEnabled (r.live);
        if (r.param.name != name || r.live != live || ! same)
        {
            name = r.param.name;
            live = r.live;
            repaint();
        }
        resized();
    }

    ControlView* view() const { return control.get(); }

    void resized() override
    {
        if (control != nullptr)
            control->setBounds (controlArea());
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        auto r = getLocalBounds().reduced (16, 0);
        g.setColour (look::colour::hair);
        g.fillRect (r.getX(), getHeight() - 1, r.getWidth(), 1);
        g.setColour (live ? look::colour::ink : look::colour::faint);
        g.setFont (look::font (look::Face::demi, 14.5f));
        g.drawFittedText (name, r.removeFromLeft (nameWidth()).withTrimmedRight (8), juce::Justification::centredLeft, 1, 0.8f);
    }

private:
    int nameWidth() const { return getWidth() < 700 ? 160 : 230; }

    juce::Rectangle<int> controlArea() const
    {
        auto r = getLocalBounds().reduced (16, 0);
        r.removeFromLeft (nameWidth());
        return r.withWidth (juce::jmin (r.getWidth(), 460)).withSizeKeepingCentre (juce::jmin (r.getWidth(), 460), 40);
    }

    Wire& wire;
    std::unique_ptr<ControlView> control;
    juce::String instance, name;
    bool live = true;
};

// The search box. Escape clears it, and goes back when it is already empty, at once (TextEditor's own escape is posted for later).
class SearchBox final : public juce::TextEditor
{
public:
    std::function<void()> onEscape;

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey))
        {
            if (onEscape)
                onEscape();
            return true;
        }
        return juce::TextEditor::keyPressed (key);
    }
};

class AllControlsPanel final : public juce::Component, private juce::ListBoxModel
{
public:
    AllControlsPanel (Wire& w, std::function<void()> back) : wire (w), onBack (std::move (back))
    {
        search.setFont (look::font (look::Face::medium, 14.5f));
        search.setTextToShowWhenEmpty ("Search controls", look::colour::faint);
        search.setIndents (14, 0);
        search.setJustification (juce::Justification::centredLeft);
        search.setTitle ("Search controls");
        search.onTextChange = [this] { refilter(); };
        search.onEscape = [this]
        {
            if (search.isEmpty())
            {
                if (onBack)
                    onBack();
            }
            else
            {
                search.clear();
                refilter();
            }
        };
        addAndMakeVisible (search);
        list.setModel (this);
        list.setRowHeight (48);
        list.setClickingTogglesRowSelection (false);
        list.setRowSelectedOnMouseDown (false);
        list.getViewport()->setScrollBarThickness (10);
        addAndMakeVisible (list);
    }

    // Opens on one plug-in: the search empty, the list at its top.
    void open (const Instance& i, const std::function<bool (const Instance&)>& isLive)
    {
        search.setText ({}, juce::dontSendNotification);
        list.getViewport()->setViewPosition (0, 0);
        update (i, isLive);
        repaint();
    }

    // A newer card: rows keep their place, values move, only what changed repaints.
    void update (const Instance& i, const std::function<bool (const Instance&)>& isLive)
    {
        const auto newTitle = i.name + " on " + trackTitle (i.track);
        if (newTitle != title)
        {
            title = newTitle;
            repaint (0, 36, getWidth(), 30);
        }
        rows.clear();
        const bool l = isLive (i);
        for (auto& p : i.params)
        {
            ListRow r { i.id, {}, p, look::cordFor (i.plugin), l };
            r.hay = (p.name + " " + p.id + " " + p.text + " " + p.choices.joinIntoString (" ")).toLowerCase();
            rows.push_back (std::move (r));
        }
        refilter();
    }

    void setSearch (const juce::String& s)
    {
        search.setText (s, juce::dontSendNotification);
        refilter();
    }
    void focusSearch() { if (isShowing()) search.grabKeyboardFocus(); }
    int count() const { return (int) shown.size(); }
    juce::TextEditor* searchBox() { return &search; }

    ControlView* control (const juce::String& instance, const juce::String& param) const
    {
        for (int k = 0; k < (int) shown.size(); ++k)
        {
            const auto& r = rows[(size_t) shown[(size_t) k]];
            if (r.instance == instance && r.param.id == param)
                if (auto* rv = dynamic_cast<RowView*> (list.getComponentForRowNumber (k)))
                    return rv->view();
        }
        return nullptr;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 0);
        auto top = r.removeFromTop (72);
        const int w = juce::jmin (280, juce::jmax (160, getWidth() / 3));
        search.setBounds (top.removeFromRight (w).withSizeKeepingCentre (w, 36));
        card = r.withTrimmedBottom (20);
        list.setBounds (card.reduced (0, 8));
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        g.fillAll (look::colour::board);
        g.setColour (look::colour::ink);
        g.setFont (look::font (look::Face::demi, 20.0f));
        g.drawText ("All controls", 20, 14, getWidth() / 2, 26, juce::Justification::centredLeft, false);
        g.setColour (look::colour::muted);
        g.setFont (look::font (look::Face::medium, 13.5f));
        const auto n = juce::String ((int) shown.size()) + ((int) shown.size() == 1 ? " control" : " controls");
        g.drawFittedText (title + ", " + n, juce::Rectangle<int> (20, 40, juce::jmax (100, search.getX() - 32), 20), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (look::colour::line);
        g.fillRoundedRectangle (card.toFloat(), 14.0f);
        if (shown.empty())
        {
            g.setColour (look::colour::muted);
            g.setFont (look::font (look::Face::medium, 15.0f));
            g.drawText (rows.empty() ? juce::String ("Its card lists no controls.") : "No control matches \"" + search.getText() + "\"", card, juce::Justification::centred, true);
        }
    }

private:
    int getNumRows() override { return (int) shown.size(); }
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}

    juce::Component* refreshComponentForRow (int row, bool, juce::Component* existing) override
    {
        if (! juce::isPositiveAndBelow (row, (int) shown.size()))
        {
            delete existing;
            return nullptr;
        }
        auto* rv = dynamic_cast<RowView*> (existing);
        if (rv == nullptr)
        {
            delete existing;
            rv = new RowView (wire);
        }
        rv->set (rows[(size_t) shown[(size_t) row]]);
        return rv;
    }

    // Every word of the search must appear in the control's name, id, value or choices.
    void refilter()
    {
        juce::StringArray words;
        words.addTokens (search.getText().toLowerCase(), " ", "\"");
        words.removeEmptyStrings();
        std::vector<int> next;
        for (int k = 0; k < (int) rows.size(); ++k)
        {
            bool all = true;
            for (auto& w : words)
                all = all && rows[(size_t) k].hay.contains (w);
            if (all)
                next.push_back (k);
        }
        const bool sameRows = next == shown && shownKey == keyOf (next);
        shown = std::move (next);
        shownKey = keyOf (shown);
        list.updateContent();
        if (! sameRows)
        {
            list.repaint();
            repaint (0, 36, getWidth(), 30);
            if (shown.empty() || ! hadRows)
                repaint();
        }
        hadRows = ! shown.empty();
    }

    juce::String keyOf (const std::vector<int>& v) const
    {
        juce::String k;
        for (auto i : v)
            k << rows[(size_t) i].instance << rows[(size_t) i].param.id << ';';
        return k;
    }

    Wire& wire;
    std::function<void()> onBack;
    SearchBox search;
    juce::ListBox list;
    std::vector<ListRow> rows;
    std::vector<int> shown;
    juce::String title, shownKey;
    juce::Rectangle<int> card;
    bool hadRows = false;
};
} // namespace hub

// ==== the window ====

SwitchboardEditor::SwitchboardEditor (SwitchboardProcessor& p)
    : AudioProcessorEditor (p), owner (p), reg (p.registry()), cmds (smooth::Link::folder())
{
    setLookAndFeel (&lnf);
    setOpaque (true);
    shown = owner.getScreen();
    seenChanges = owner.screenChanges();

    backBtn = std::make_unique<hub::RailButton>();
    backBtn->onClick = [this] { goBack(); };
    addChildComponent (*backBtn);

    list = std::make_unique<hub::TrackList> ([this] (const juce::String& track) { show (hub::Screen::onTrack (track)); });
    board = std::make_unique<hub::Board> (static_cast<hub::Wire&> (*this), [this] (const juce::String& id) { openAllControls (id); });
    for (auto* v : { &listView, &boardView })
    {
        v->setScrollBarsShown (true, false, true, false);
        v->setScrollBarThickness (10);
        addChildComponent (*v);
    }
    listView.setViewedComponent (list.get(), false);
    boardView.setViewedComponent (board.get(), false);

    empty = std::make_unique<hub::EmptyState>();
    addChildComponent (*empty);

    panel = std::make_unique<hub::AllControlsPanel> (static_cast<hub::Wire&> (*this), [this] { goBack(); });
    addChildComponent (*panel);

    setResizable (true, true);
    setResizeLimits (600, 420, 2400, 1600);
    setSize (880, 580);
    tickAt (now(), true);
    startTimerHz (10);
}

SwitchboardEditor::~SwitchboardEditor()
{
    closing = true;
    stopTimer();
    juce::PopupMenu::dismissAllActiveMenus();
    panel.reset();
    empty.reset();
    listView.setViewedComponent (nullptr, false);
    boardView.setViewedComponent (nullptr, false);
    list.reset();
    board.reset();
    backBtn.reset();
    setLookAndFeel (nullptr);
}

void SwitchboardEditor::paint (juce::Graphics& g)
{
    using namespace hub::look;
    ++paints;
    g.fillAll (colour::board);
    // Only what the repaint touches: a moving knob must not lay out the rail's and the footer's text again.
    if (g.clipRegionIntersects (railArea))
        paintRail (g);
    if (hint.isNotEmpty() && ! panel->isVisible() && g.clipRegionIntersects (footerArea))
    {
        g.setColour (colour::faint);
        g.setFont (font (Face::medium, 13.0f));
        g.drawFittedText (hint, footerArea, juce::Justification::centredLeft, 1, 0.8f);
    }
}

void SwitchboardEditor::paintRail (juce::Graphics& g)
{
    using namespace hub::look;
    g.setColour (colour::rail);
    g.fillRect (railArea);
    if (shown.kind == hub::Screen::Kind::tracks)
    {
        drawMark (g, juce::Rectangle<float> (22.0f, 17.0f, 30.0f, 26.0f));
        g.setColour (colour::railText);
        g.setFont (font (Face::bold, 20.0f));
        g.drawText ("Switchboard", 62, 0, 150, railArea.getHeight(), juce::Justification::centredLeft, false);
        const auto sub = subtitleText();
        if (sub.isNotEmpty() && subtitleArea.getWidth() > 40)
        {
            g.setColour (colour::railMuted);
            g.setFont (font (Face::medium, 14.0f));
            g.drawFittedText (sub, subtitleArea, juce::Justification::centredLeft, 1, 0.85f);
        }
    }
    else if (titleArea.getWidth() > 20)
    {
        g.setColour (colour::railText);
        g.setFont (font (Face::bold, 20.0f));
        g.drawFittedText (titleText(), titleArea, juce::Justification::centredLeft, 1, 0.7f);
    }
    if (status.isNotEmpty() && (float) statusArea.getWidth() >= hub::widthOf (font (Face::medium, 14.0f), status) + 4.0f)
    {
        g.setColour (colour::railMuted);
        g.setFont (font (Face::medium, 14.0f));
        g.drawText (status, statusArea, juce::Justification::centredRight, true);
    }
}

void SwitchboardEditor::layoutRail()
{
    using namespace hub::look;
    railArea = getLocalBounds().removeFromTop (60);
    const int statusW = status.isEmpty() ? 0 : (int) std::ceil (hub::widthOf (font (Face::medium, 14.0f), status)) + 4;
    statusArea = { getWidth() - 22 - statusW, 0, statusW, 60 };
    if (shown.kind == hub::Screen::Kind::tracks)
    {
        backBtn->setVisible (false);
        const int wordEnd = 62 + (int) std::ceil (hub::widthOf (font (Face::bold, 20.0f), "Switchboard"));
        subtitleArea = { wordEnd + 12, 0, juce::jmax (0, statusArea.getX() - 16 - (wordEnd + 12)), 60 };
        titleArea = {};
    }
    else
    {
        backBtn->setText (shown.kind == hub::Screen::Kind::controls ? hub::trackTitle (shown.track) : juce::String ("Tracks"));
        const int bw = juce::jmin (backBtn->idealWidth(), juce::jmax (96, getWidth() * 2 / 5));
        backBtn->setBounds (16, 12, bw, 36);
        backBtn->setVisible (true);
        const int tx = backBtn->getRight() + 18;
        titleArea = { tx, 0, juce::jmax (0, statusArea.getX() - 16 - tx), 60 };
        subtitleArea = {};
    }
}

void SwitchboardEditor::resized()
{
    layoutRail();
    auto body = getLocalBounds().withTrimmedTop (60);
    footerArea = body.removeFromBottom (34).withTrimmedLeft (22).withTrimmedRight (40);
    bodyArea = body;
    listView.setBounds (bodyArea);
    boardView.setBounds (bodyArea);
    list->layout (listView.getWidth() - listView.getScrollBarThickness());
    board->layout (boardView.getWidth() - boardView.getScrollBarThickness());
    empty->setBounds (bodyArea);
    panel->setBounds (getLocalBounds().withTrimmedTop (60));
}

void SwitchboardEditor::timerCallback() { tickAt (now(), false); }

void SwitchboardEditor::tickNow (double unixNow) { tickAt (unixNow, true); }

void SwitchboardEditor::tickAt (double t, bool forceScan)
{
    if (auto* d = dragged.getComponent())
        d->tick (t);
    if (! forceScan && t - lastScan < 0.09 && t >= lastScan)
        return;
    lastScan = t;
    owner.refresh (t);
    bool changed = reg.generation() != seenGeneration;
    seenGeneration = reg.generation();
    for (const auto& o : cmds.settle (reg, t))
    {
        // Every control that sent it hears back, shown or not, so a control on a screen left meanwhile shows the card again.
        if (auto* l = board->line (o.instance))
            if (auto* c = l->control (o.param))
                c->settled (o.name);
        if (auto* c = panel->control (o.instance, o.param))
            c->settled (o.name);
        if (! o.answered)
        {
            auto* i = reg.find (o.instance);
            unanswered[o.instance] = i != nullptr ? i->seq : 0;
            changed = true;
        }
    }
    for (auto it = unanswered.begin(); it != unanswered.end();)
    {
        auto* i = reg.find (it->first);
        if (i == nullptr || i->seq > it->second)
        {
            it = unanswered.erase (it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }
    const auto track = owner.hub.trackName();
    if (track != knownTrack)
    {
        knownTrack = track;
        repaint (railArea);
    }
    if (owner.screenChanges() != seenChanges)
    {
        follow();
        changed = true;
    }
    if (changed || first || fresh)
    {
        first = false;
        refreshView (t);
    }
}

void SwitchboardEditor::follow()
{
    seenChanges = owner.screenChanges();
    navigate (owner.getScreen());
}

void SwitchboardEditor::navigate (const hub::Screen& s)
{
    if (s == shown)
        return;
    const bool otherTrack = s.kind != shown.kind || s.track != shown.track;
    shown = s;
    fresh = true;
    if (otherTrack && s.kind == hub::Screen::Kind::track)
        boardView.setViewPosition (0, 0);
    layoutRail();
    repaint();
}

void SwitchboardEditor::show (const hub::Screen& s)
{
    owner.setScreen (s);
    follow();
    refreshView (now());
}

void SwitchboardEditor::goBack()
{
    if (shown.kind == hub::Screen::Kind::controls)
        show (hub::Screen::onTrack (shown.track));
    else if (shown.kind == hub::Screen::Kind::track)
        show (hub::Screen::tracks());
}

bool SwitchboardEditor::live (const hub::Instance& i, double t) const
{
    return hub::Registry::responding (i, t) && unanswered.find (i.id) == unanswered.end();
}

void SwitchboardEditor::refreshView (double t)
{
    fresh = false;
    const auto isLive = [this, t] (const hub::Instance& i) { return live (i, t); };
    enum class Body { list, board, panel, empty };
    Body body = Body::empty;
    juce::String st, h, heading, sub;
    const auto count = [] (int n, const char* one, const char* many) { return juce::String (n) + " " + (n == 1 ? one : many); };

    switch (shown.kind)
    {
        case hub::Screen::Kind::tracks:
        {
            std::vector<hub::RowData> rows;
            for (const auto& tr : reg.tracks())
            {
                hub::RowData d;
                d.track = tr;
                for (auto* i : reg.onTrack (tr))
                    d.chips.push_back ({ i->name, hub::look::cordFor (i->plugin), isLive (*i) });
                rows.push_back (std::move (d));
            }
            list->show (rows);
            if (! rows.empty())
            {
                body = Body::list;
                st = count ((int) rows.size(), "track", "tracks");
                h = "Click a track to see its plug-ins and their controls.";
            }
            else
            {
                heading = "None of your plug-ins are open in this project yet";
                sub = "Each one shows here under its track as soon as it opens. It needs its link version.";
            }
            break;
        }
        case hub::Screen::Kind::track:
        {
            const auto items = reg.onTrack (shown.track);
            board->show (items, isLive);
            if (! items.empty())
            {
                body = Body::board;
                st = count ((int) items.size(), "plug-in", "plug-ins");
                h = "Drag a knob to turn it. Double-click puts a control back to its default.";
            }
            else
            {
                heading = "Nothing on " + hub::trackTitle (shown.track) + " is open right now";
                sub = "Its plug-ins show here while they are open.";
            }
            break;
        }
        case hub::Screen::Kind::controls:
        {
            if (auto* i = reg.find (shown.track, shown.plugin))
            {
                body = Body::panel;
                if (i->id != panelId)
                {
                    panelId = i->id;
                    panel->open (*i, isLive);
                }
                else
                {
                    panel->update (*i, isLive);
                }
            }
            else
            {
                panelId = {};
                heading = shown.plugin + " is not open on " + hub::trackTitle (shown.track) + " right now";
                sub = "Its controls show here while it is open.";
            }
            break;
        }
    }
    if (shown.kind != hub::Screen::Kind::controls)
        panelId = {};

    if (body == Body::empty)
        empty->set (heading, sub);
    const bool panelBefore = panel->isVisible();
    listView.setVisible (body == Body::list);
    boardView.setVisible (body == Body::board);
    panel->setVisible (body == Body::panel);
    empty->setVisible (body == Body::empty);
    if (body == Body::panel && ! panelBefore)
        panel->focusSearch();

    if (st != status || h != hint)
    {
        status = st;
        hint = h;
        layoutRail();
        repaint (railArea);
        repaint (footerArea);
    }
}

juce::String SwitchboardEditor::titleText() const
{
    switch (shown.kind)
    {
        case hub::Screen::Kind::track:    return hub::trackTitle (shown.track);
        case hub::Screen::Kind::controls: return shown.plugin;
        case hub::Screen::Kind::tracks:   break;
    }
    return "Switchboard";
}

juce::String SwitchboardEditor::subtitleText() const
{
    return shown.kind == hub::Screen::Kind::tracks && knownTrack.isNotEmpty() ? "on " + knownTrack : juce::String();
}

juce::String SwitchboardEditor::backText() const
{
    return backBtn->isVisible() ? hub::backQuote() + " " + backBtn->getText() : juce::String();
}

juce::Component* SwitchboardEditor::backButton() const { return backBtn.get(); }

std::vector<SwitchboardEditor::TrackRow> SwitchboardEditor::trackRows() const
{
    std::vector<TrackRow> out;
    if (! listView.isVisible())
        return out;
    for (const auto& r : list->rows())
    {
        TrackRow row { r.track, hub::trackTitle (r.track), {}, {} };
        juce::StringArray quiet;
        for (const auto& c : r.chips)
        {
            row.plugins.add (c.name);
            if (! c.live)
                quiet.add (c.name + " not responding");
        }
        row.note = quiet.joinIntoString (", ");
        out.push_back (std::move (row));
    }
    return out;
}

juce::Component* SwitchboardEditor::trackRow (const juce::String& track) const
{
    return listView.isVisible() ? list->row (track) : nullptr;
}

juce::Component* SwitchboardEditor::controlsLink (const juce::String& instance) const
{
    auto* l = boardView.isVisible() ? board->line (instance) : nullptr;
    return l != nullptr ? l->allLink() : nullptr;
}

juce::TextEditor* SwitchboardEditor::searchBox() const { return panel->searchBox(); }

juce::StringArray SwitchboardEditor::shownNames() const { return boardView.isVisible() ? board->names() : juce::StringArray(); }

juce::String SwitchboardEditor::emptyText() const { return empty->isVisible() ? empty->text() : juce::String(); }

hub::ControlView* SwitchboardEditor::findControl (const juce::String& instance, const juce::String& param) const
{
    auto* l = boardView.isVisible() ? board->line (instance) : nullptr;
    return l != nullptr ? l->control (param) : nullptr;
}

bool SwitchboardEditor::isGreyed (const juce::String& instance) const
{
    auto* l = boardView.isVisible() ? board->line (instance) : nullptr;
    return l != nullptr && ! l->isLive();
}

juce::String SwitchboardEditor::statusOf (const juce::String& instance) const
{
    auto* l = boardView.isVisible() ? board->line (instance) : nullptr;
    return l == nullptr ? juce::String() : (l->isLive() ? juce::String() : juce::String ("not responding"));
}

void SwitchboardEditor::openAllControls (const juce::String& instance)
{
    if (auto* i = reg.find (instance))
        show (hub::Screen::controlsOf (i->track, i->name));
}

void SwitchboardEditor::closeAllControls()
{
    if (shown.kind == hub::Screen::Kind::controls)
        goBack();
}

int SwitchboardEditor::allControlsRows() const { return panel->isVisible() ? panel->count() : -1; }

void SwitchboardEditor::searchAllControls (const juce::String& s) { panel->setSearch (s); }

hub::ControlView* SwitchboardEditor::findAllControlsControl (const juce::String& instance, const juce::String& param) const
{
    return panel->isVisible() ? panel->control (instance, param) : nullptr;
}

juce::String SwitchboardEditor::send (const juce::String& instance, const juce::String& param, double value, hub::Gesture g)
{
    return cmds.setValue (instance, param, value, g, now());
}

juce::String SwitchboardEditor::sendText (const juce::String& instance, const juce::String& param, const juce::String& text)
{
    return cmds.setText (instance, param, text, hub::Gesture::tap, now());
}

void SwitchboardEditor::dragging (hub::ControlView& c, bool on)
{
    if (closing)
        return;
    if (on)
    {
        dragged = &c;
        startTimerHz (30);
    }
    else if (dragged.getComponent() == &c || dragged.getComponent() == nullptr)
    {
        dragged = nullptr;
        startTimerHz (10);
    }
}
