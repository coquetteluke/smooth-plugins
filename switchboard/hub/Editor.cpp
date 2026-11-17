#include "Editor.h"

namespace hub
{
namespace
{
    float widthOf (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }
    juce::String trackTitle (const juce::String& track) { return track.isEmpty() ? juce::String ("No track name") : track; }
}

// ---- a button on the rail (the view menu, All controls) or an ink one on the board ----
class RailButton final : public juce::Component
{
public:
    enum class Style { view, rail, ink };

    explicit RailButton (Style s) : style (s) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    std::function<void()> onClick;

    void setText (const juce::String& newPrefix, const juce::String& newMain)
    {
        if (newPrefix == prefix && newMain == main)
            return;
        prefix = newPrefix;
        main = newMain;
        repaint();
    }

    int idealWidth() const
    {
        float w = widthOf (mainFont(), main) + 32.0f;
        if (prefix.isNotEmpty())
            w += widthOf (prefixFont(), prefix) + 7.0f;
        if (style == Style::view)
            w += 20.0f;
        return (int) std::ceil (w);
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        const auto r = getLocalBounds().toFloat();
        juce::Colour bg = style == Style::ink ? (hover ? look::colour::ink.brighter (0.3f) : look::colour::ink)
                                              : (hover ? look::colour::railHover : look::colour::railPill);
        g.setColour (bg);
        g.fillRoundedRectangle (r, 10.0f);
        auto inner = r.reduced (16.0f, 0.0f);
        if (style == Style::view)
        {
            auto chevron = inner.removeFromRight (12.0f);
            const auto c = chevron.getCentre();
            juce::Path v;
            v.startNewSubPath (c.x - 4.5f, c.y - 2.0f);
            v.lineTo (c.x, c.y + 2.5f);
            v.lineTo (c.x + 4.5f, c.y - 2.0f);
            g.setColour (look::colour::railMuted);
            g.strokePath (v, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            inner.removeFromRight (8.0f);
        }
        if (prefix.isNotEmpty())
        {
            g.setColour (look::colour::railMuted);
            g.setFont (prefixFont());
            const float pw = widthOf (prefixFont(), prefix);
            g.drawText (prefix, inner.removeFromLeft (pw + 1.0f), juce::Justification::centredLeft, false);
            inner.removeFromLeft (6.0f);
        }
        g.setColour (style == Style::ink ? juce::Colours::white : look::colour::railText);
        g.setFont (mainFont());
        g.drawFittedText (main, inner.toNearestInt(), style == Style::view ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.8f);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick && getLocalBounds().contains (e.getPosition()) && ! e.mouseWasDraggedSinceMouseDown())
            onClick();
    }

private:
    juce::Font mainFont() const { return look::font (look::Face::demi, 14.5f); }
    juce::Font prefixFont() const { return look::font (look::Face::medium, 13.5f); }

    const Style style;
    juce::String prefix, main;
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
        all.onClick = [this] { if (onAll) onAll (instanceId); };
        addAndMakeVisible (all);
    }

    const juce::String& id() const { return instanceId; }
    const juce::String& title() const { return name; }
    bool isLive() const { return liveNow; }

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

// A track and the plug-ins on it, as the board shows them.
struct Group
{
    juce::String track;
    bool header = false;
    std::vector<const Instance*> items;
};

// ---- the lines, in the viewport ----
class Board final : public juce::Component
{
public:
    Board (Wire& w, std::function<void (const juce::String&)> openAll) : wire (w), onAll (std::move (openAll)) {}

    // Shows the groups. Lines are kept by instance id, so a line that stays keeps its controls (and a drag in progress).
    void show (const std::vector<Group>& groups, const std::function<bool (const Instance&)>& isLive, const juce::String& ownTrack)
    {
        juce::StringArray key;
        for (auto& gr : groups)
        {
            if (gr.header)
                key.add ("#" + gr.track + (gr.track == ownTrack ? "*" : "") + juce::String ((int) gr.items.size()));
            for (auto* i : gr.items)
                key.add (i->id);
        }
        bool relayout = key != order;
        if (relayout)
        {
            order = key;
            std::map<juce::String, std::unique_ptr<PluginLine>> old;
            for (auto& l : lines)
                old[l->id()] = std::move (l);
            lines.clear();
            headers.clear();
            for (auto& gr : groups)
            {
                if (gr.header)
                    headers.push_back ({ gr.track, gr.track == ownTrack && ownTrack.isNotEmpty(), (int) lines.size(), {} });
                for (auto* i : gr.items)
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
        }
        size_t k = 0;
        for (auto& gr : groups)
            for (auto* i : gr.items)
                relayout = lines[k++]->update (*i, isLive (*i)) || relayout;
        if (relayout)
            layout (getWidth());
    }

    void layout (int width)
    {
        if (width <= 0)
            return;
        int y = 14;
        size_t h = 0;
        for (size_t k = 0; k <= lines.size(); ++k)
        {
            while (h < headers.size() && headers[h].beforeLine == (int) k)
            {
                headers[h].area = { 20, y, width - 40, 32 };
                y += 32;
                ++h;
            }
            if (k == lines.size())
                break;
            const int lh = lines[k]->heightFor (width - 32);
            lines[k]->setBounds (16, y, width - 32, lh);
            y += lh + 10;
        }
        setSize (width, y + 4);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        ++look::paints;
        for (auto& hd : headers)
        {
            auto r = hd.area;
            const auto title = trackTitle (hd.track).toUpperCase();
            const auto f = look::caps (12.0f);
            g.setFont (f);
            g.setColour (look::colour::muted);
            const int w = (int) std::ceil (widthOf (f, title)) + 4;
            g.drawText (title, r.removeFromLeft (w), juce::Justification::centredLeft, false);
            if (hd.own)
            {
                r.removeFromLeft (10);
                const auto tf = look::caps (10.0f);
                const juce::String tag ("THIS TRACK");
                const int tw = (int) std::ceil (widthOf (tf, tag)) + 18;
                const auto pill = r.removeFromLeft (tw).withSizeKeepingCentre (tw, 20);
                g.setColour (look::colour::ink);
                g.fillRoundedRectangle (pill.toFloat(), 10.0f);
                g.setColour (juce::Colours::white);
                g.setFont (tf);
                g.drawText (tag, pill, juce::Justification::centred, false);
            }
        }
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
    struct Header
    {
        juce::String track;
        bool own = false;
        int beforeLine = 0;
        juce::Rectangle<int> area;
    };

    Wire& wire;
    std::function<void (const juce::String&)> onAll;
    std::vector<std::unique_ptr<PluginLine>> lines;
    std::vector<Header> headers;
    juce::StringArray order;
};

// ---- what the board says when it has nothing to show ----
class EmptyState final : public juce::Component
{
public:
    EmptyState() : button (RailButton::Style::ink)
    {
        button.onClick = [this] { if (onAllTracks) onAllTracks(); };
        addChildComponent (button);
    }

    std::function<void()> onAllTracks;

    void set (const juce::String& newHeading, const juce::String& newSub, int elsewhere)
    {
        if (newHeading != heading || newSub != sub)
        {
            heading = newHeading;
            sub = newSub;
            repaint();
        }
        button.setText ({}, "Show all tracks (" + juce::String (elsewhere) + (elsewhere == 1 ? " plug-in)" : " plug-ins)"));
        button.setVisible (elsewhere > 0);
        resized();
    }

    juce::String text() const { return heading + "\n" + sub; }
    bool offers() const { return button.isVisible(); }

    void resized() override
    {
        const int w = button.idealWidth();
        button.setBounds ((getWidth() - w) / 2, centreY() + 92, w, 38);
    }

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

    RailButton button;
    juce::String heading, sub;
};

// ---- the All controls list ----
struct ListRow
{
    juce::String instance, pluginName, hay;
    Param param;
    juce::Colour cord;
    bool live = true;
};

class RowView final : public juce::Component
{
public:
    explicit RowView (Wire& w) : wire (w) {}

    void set (const ListRow& r, bool showPlugin)
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
        if (r.pluginName != plugin || r.param.name != name || showPlugin != withPlugin || r.cord != cord || r.live != live || ! same)
        {
            plugin = r.pluginName;
            name = r.param.name;
            withPlugin = showPlugin;
            cord = r.cord;
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
        if (withPlugin)
        {
            auto p = r.removeFromLeft (pluginWidth());
            g.setColour (live ? cord : look::colour::hair);
            g.fillEllipse (p.removeFromLeft (10).toFloat().withSizeKeepingCentre (10.0f, 10.0f));
            p.removeFromLeft (10);
            g.setColour (live ? look::colour::muted : look::colour::faint);
            g.setFont (look::font (look::Face::medium, 13.5f));
            g.drawFittedText (plugin, p.withTrimmedRight (8), juce::Justification::centredLeft, 1, 0.8f);
        }
        g.setColour (live ? look::colour::ink : look::colour::faint);
        g.setFont (look::font (look::Face::demi, 14.5f));
        g.drawFittedText (name, r.removeFromLeft (nameWidth()).withTrimmedRight (8), juce::Justification::centredLeft, 1, 0.8f);
    }

private:
    int pluginWidth() const { return getWidth() < 700 ? 130 : 170; }
    int nameWidth() const { return getWidth() < 700 ? 160 : 230; }

    juce::Rectangle<int> controlArea() const
    {
        auto r = getLocalBounds().reduced (16, 0);
        if (withPlugin)
            r.removeFromLeft (pluginWidth());
        r.removeFromLeft (nameWidth());
        return r.withWidth (juce::jmin (r.getWidth(), 460)).withSizeKeepingCentre (juce::jmin (r.getWidth(), 460), 40);
    }

    Wire& wire;
    std::unique_ptr<ControlView> control;
    juce::String instance, plugin, name;
    juce::Colour cord;
    bool withPlugin = false, live = true;
};

class AllControlsPanel final : public juce::Component, private juce::ListBoxModel
{
public:
    AllControlsPanel (Wire& w, std::function<void()> onDone) : wire (w), done (RailButton::Style::ink)
    {
        done.setText ({}, "Done");
        done.onClick = std::move (onDone);
        addAndMakeVisible (done);
        search.setFont (look::font (look::Face::medium, 14.5f));
        search.setTextToShowWhenEmpty ("Search controls", look::colour::faint);
        search.setIndents (14, 0);
        search.setJustification (juce::Justification::centredLeft);
        search.onTextChange = [this] { refilter(); };
        search.onEscapeKey = [this] { if (search.isEmpty()) { if (done.onClick) done.onClick(); } else search.clear(); };
        addAndMakeVisible (search);
        list.setModel (this);
        list.setRowHeight (48);
        list.setClickingTogglesRowSelection (false);
        list.setRowSelectedOnMouseDown (false);
        list.getViewport()->setScrollBarThickness (10);
        addAndMakeVisible (list);
    }

    // scopeId: one plug-in, or empty for every plug-in in the view.
    void open (const juce::String& scopeId, const juce::String& scopeText, const std::vector<const Instance*>& view, const std::function<bool (const Instance&)>& isLive)
    {
        scope = scopeId;
        title = scopeText;
        search.setText ({}, juce::dontSendNotification);
        list.getViewport()->setViewPosition (0, 0);
        update (view, isLive);
        repaint();
    }

    // Newer cards: rows keep their place, values move, only what changed repaints.
    void update (const std::vector<const Instance*>& view, const std::function<bool (const Instance&)>& isLive)
    {
        rows.clear();
        int plugins = 0;
        for (auto* i : view)
        {
            if (scope.isNotEmpty() && i->id != scope)
                continue;
            ++plugins;
            const bool l = isLive (*i);
            for (auto& p : i->params)
            {
                ListRow r { i->id, i->name, {}, p, look::cordFor (i->plugin), l };
                r.hay = (i->name + " " + p.name + " " + p.id + " " + p.text + " " + p.choices.joinIntoString (" ")).toLowerCase();
                rows.push_back (std::move (r));
            }
        }
        multi = plugins > 1 || scope.isEmpty();
        refilter();
    }

    void setSearch (const juce::String& s)
    {
        search.setText (s, juce::dontSendNotification);
        refilter();
    }
    void focusSearch() { if (isShowing()) search.grabKeyboardFocus(); }
    int count() const { return (int) shown.size(); }

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
        done.setBounds (top.removeFromRight (92).withSizeKeepingCentre (92, 36));
        top.removeFromRight (12);
        search.setBounds (top.removeFromRight (juce::jmin (280, juce::jmax (160, getWidth() / 3))).withSizeKeepingCentre (juce::jmin (280, juce::jmax (160, getWidth() / 3)), 36));
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
            g.drawText ("No control matches \"" + search.getText() + "\"", card, juce::Justification::centred, true);
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
        rv->set (rows[(size_t) shown[(size_t) row]], multi);
        return rv;
    }

    // Every word of the search must appear in the row's plug-in, control name, id, value or choices.
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
    RailButton done;
    juce::TextEditor search;
    juce::ListBox list;
    std::vector<ListRow> rows;
    std::vector<int> shown;
    juce::String scope, title, shownKey;
    juce::Rectangle<int> card;
    bool multi = false, hadRows = false;
};
} // namespace hub

// ==== the window ====

namespace
{
juce::String encodeView (const SwitchboardEditor::View& v)
{
    switch (v.kind)
    {
        case SwitchboardEditor::ViewKind::allTracks: return "all";
        case SwitchboardEditor::ViewKind::oneTrack:  return "track:" + v.track;
        case SwitchboardEditor::ViewKind::thisTrack: break;
    }
    return "this";
}

SwitchboardEditor::View decodeView (const juce::String& s)
{
    if (s == "all")
        return { SwitchboardEditor::ViewKind::allTracks, {} };
    if (s.startsWith ("track:"))
        return { SwitchboardEditor::ViewKind::oneTrack, s.substring (6) };
    return {};
}
} // namespace

SwitchboardEditor::SwitchboardEditor (SwitchboardProcessor& p)
    : AudioProcessorEditor (p), owner (p), reg (smooth::Link::folder()), cmds (smooth::Link::folder())
{
    setLookAndFeel (&lnf);
    setOpaque (true);
    current = decodeView (owner.getView());

    viewButton = std::make_unique<hub::RailButton> (hub::RailButton::Style::view);
    viewButton->onClick = [this] { showViewMenu(); };
    allButton = std::make_unique<hub::RailButton> (hub::RailButton::Style::rail);
    allButton->setText ({}, "All controls");
    allButton->onClick = [this] { openAllControls ({}); };
    addAndMakeVisible (*viewButton);
    addChildComponent (*allButton);   // shown once the view has a plug-in

    board = std::make_unique<hub::Board> (static_cast<hub::Wire&> (*this), [this] (const juce::String& id) { openAllControls (id); });
    viewport.setViewedComponent (board.get(), false);
    viewport.setScrollBarsShown (true, false, true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    empty = std::make_unique<hub::EmptyState>();
    empty->onAllTracks = [this] { chooseView ({ ViewKind::allTracks, {} }); };
    addChildComponent (*empty);

    panel = std::make_unique<hub::AllControlsPanel> (static_cast<hub::Wire&> (*this), [this] { closeAllControls(); });
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
    viewport.setViewedComponent (nullptr, false);
    board.reset();
    setLookAndFeel (nullptr);
}

void SwitchboardEditor::paint (juce::Graphics& g)
{
    using namespace hub::look;
    ++paints;
    g.fillAll (colour::board);
    g.setColour (colour::rail);
    g.fillRect (railArea);
    drawMark (g, juce::Rectangle<float> (22.0f, 17.0f, 30.0f, 26.0f));
    g.setColour (colour::railText);
    g.setFont (font (Face::bold, 20.0f));
    g.drawText ("Switchboard", 62, 0, 150, railArea.getHeight(), juce::Justification::centredLeft, false);
    if (status.isNotEmpty() && (float) statusArea.getWidth() >= hub::widthOf (font (Face::medium, 14.0f), status) + 4.0f)
    {
        g.setColour (colour::railMuted);
        g.setFont (font (Face::medium, 14.0f));
        g.drawText (status, statusArea, juce::Justification::centredRight, true);
    }
    if (! panel->isVisible() && status.isNotEmpty())
    {
        g.setColour (colour::faint);
        g.setFont (font (Face::medium, 13.0f));
        g.drawFittedText ("Drag a knob to turn it. Double-click puts a control back to its default.", footerArea, juce::Justification::centredLeft, 1, 0.8f);
    }
}

void SwitchboardEditor::layoutRail()
{
    railArea = getLocalBounds().removeFromTop (60);
    const int wordEnd = 62 + (int) std::ceil (hub::widthOf (hub::look::font (hub::look::Face::bold, 20.0f), "Switchboard"));
    const int allW = allButton->idealWidth();
    allButton->setBounds (getWidth() - 20 - allW, 12, allW, 36);
    const int viewX = wordEnd + 22;
    const int viewW = juce::jmin (viewButton->idealWidth(), juce::jmax (120, allButton->getX() - 16 - viewX));
    viewButton->setBounds (viewX, 12, viewW, 36);
    statusArea = juce::Rectangle<int> (viewButton->getRight() + 12, 0, allButton->getX() - 14 - viewButton->getRight() - 12, 60);
}

void SwitchboardEditor::resized()
{
    layoutRail();
    auto body = getLocalBounds().withTrimmedTop (60);
    footerArea = body.removeFromBottom (34).withTrimmedLeft (22).withTrimmedRight (40);
    bodyArea = body;
    viewport.setBounds (bodyArea);
    board->layout (viewport.getWidth() - viewport.getScrollBarThickness());
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
    bool changed = reg.scan (t);
    for (const auto& o : cmds.settle (reg, t))
    {
        if (auto* c = findControl (o.instance, o.param))
            c->settled (o.name);
        if (auto* c = findAllControlsControl (o.instance, o.param))
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
        changed = true;
    }
    if (changed || first)
    {
        first = false;
        refreshView (t);
    }
}

bool SwitchboardEditor::live (const hub::Instance& i, double t) const
{
    return hub::Registry::responding (i, t) && unanswered.find (i.id) == unanswered.end();
}

std::vector<SwitchboardEditor::Section> SwitchboardEditor::sections() const
{
    std::vector<Section> out;
    const auto everyTrack = [&]
    {
        for (const auto& t : reg.tracks())
            out.push_back ({ t, true, reg.onTrack (t) });
    };
    switch (current.kind)
    {
        case ViewKind::thisTrack:
            if (knownTrack.isNotEmpty())
                out.push_back ({ knownTrack, false, reg.onTrack (knownTrack) });
            else
                everyTrack();   // Logic has not named this track yet
            break;
        case ViewKind::allTracks: everyTrack(); break;
        case ViewKind::oneTrack: out.push_back ({ current.track, false, reg.onTrack (current.track) }); break;
    }
    return out;
}

std::vector<const hub::Instance*> SwitchboardEditor::inView() const
{
    std::vector<const hub::Instance*> out;
    for (auto& s : sections())
        out.insert (out.end(), s.items.begin(), s.items.end());
    return out;
}

void SwitchboardEditor::refreshView (double t)
{
    const auto secs = sections();
    std::vector<hub::Group> groups;
    int count = 0;
    for (auto& s : secs)
    {
        groups.push_back ({ s.track, s.header, s.items });
        count += (int) s.items.size();
    }
    const auto isLive = [this, t] (const hub::Instance& i) { return live (i, t); };
    board->show (groups, isLive, knownTrack);

    const auto st = count == 0 ? juce::String() : juce::String (count) + (count == 1 ? " plug-in" : " plug-ins");
    if (st != status)
    {
        status = st;
        repaint (statusArea);
        repaint (footerArea);
        allButton->setVisible (count > 0);
    }

    juce::String prefix, main;
    switch (current.kind)
    {
        case ViewKind::thisTrack:
            if (knownTrack.isNotEmpty()) { prefix = "This track"; main = knownTrack; }
            else { prefix = {}; main = "All tracks"; }
            break;
        case ViewKind::allTracks: main = "All tracks"; break;
        case ViewKind::oneTrack: prefix = "Track"; main = trackTitle (current.track); break;
    }
    const int before = viewButton->idealWidth();
    viewButton->setText (prefix, main);
    if (viewButton->idealWidth() != before)
    {
        layoutRail();
        repaint (railArea);
    }

    if (count == 0)
    {
        const int elsewhere = (int) reg.instances().size();
        juce::String heading = "Put one of your plug-ins on this track";
        juce::String sub = "It shows here with its main controls. Each plug-in needs its link version.";
        bool offer = false;
        if (current.kind == ViewKind::thisTrack && knownTrack.isNotEmpty())
            offer = elsewhere > 0;
        else if (current.kind == ViewKind::oneTrack)
        {
            heading = "Nothing open on " + trackTitle (current.track) + " right now";
            sub = "Its plug-ins show here while they are open.";
            offer = elsewhere > 0;
        }
        else
        {
            heading = "Put one of your plug-ins on a track";
        }
        empty->set (heading, sub, offer ? elsewhere : 0);
        empty->setVisible (! panel->isVisible());
        viewport.setVisible (false);
    }
    else
    {
        empty->setVisible (false);
        viewport.setVisible (! panel->isVisible());
    }
    if (panel->isVisible())
        panel->update (inView(), isLive);
}

juce::String SwitchboardEditor::viewLabel() const
{
    switch (current.kind)
    {
        case ViewKind::thisTrack: return knownTrack.isNotEmpty() ? "This track: " + knownTrack : juce::String ("All tracks");
        case ViewKind::allTracks: return "All tracks";
        case ViewKind::oneTrack:  return "Track: " + trackTitle (current.track);
    }
    return {};
}

juce::StringArray SwitchboardEditor::viewMenuItems() const
{
    juce::StringArray items { "This track", "All tracks" };
    for (const auto& t : reg.tracks())
        items.add (trackTitle (t));
    return items;
}

void SwitchboardEditor::showViewMenu()
{
    juce::PopupMenu m;
    m.setLookAndFeel (&lnf);
    m.addItem (1, knownTrack.isNotEmpty() ? "This track (" + knownTrack + ")" : juce::String ("This track (no name yet)"), true, current.kind == ViewKind::thisTrack);
    m.addItem (2, "All tracks", true, current.kind == ViewKind::allTracks);
    const auto tracks = reg.tracks();
    if (! tracks.isEmpty())
    {
        m.addSeparator();
        m.addSectionHeader ("Tracks");
        for (int i = 0; i < tracks.size(); ++i)
            m.addItem (100 + i, trackTitle (tracks[i]), true, current.kind == ViewKind::oneTrack && current.track == tracks[i]);
    }
    juce::Component::SafePointer<SwitchboardEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (viewButton.get()).withMinimumWidth (240).withStandardItemHeight (30),
                     [safe, tracks] (int chosen)
                     {
                         if (safe == nullptr || chosen == 0)
                             return;
                         if (chosen == 1)
                             safe->chooseView ({ ViewKind::thisTrack, {} });
                         else if (chosen == 2)
                             safe->chooseView ({ ViewKind::allTracks, {} });
                         else if (juce::isPositiveAndBelow (chosen - 100, tracks.size()))
                             safe->chooseView ({ ViewKind::oneTrack, tracks[chosen - 100] });
                     });
}

void SwitchboardEditor::chooseView (View v)
{
    current = v;
    owner.setView (encodeView (v));
    refreshView (now());
}

juce::StringArray SwitchboardEditor::shownNames() const { return board->names(); }

juce::String SwitchboardEditor::emptyText() const { return empty->isVisible() || (! viewport.isVisible() && board->names().isEmpty()) ? empty->text() : juce::String(); }

bool SwitchboardEditor::offersAllTracks() const { return emptyText().isNotEmpty() && empty->offers(); }

hub::ControlView* SwitchboardEditor::findControl (const juce::String& instance, const juce::String& param) const
{
    auto* l = board->line (instance);
    return l != nullptr ? l->control (param) : nullptr;
}

bool SwitchboardEditor::isGreyed (const juce::String& instance) const
{
    auto* l = board->line (instance);
    return l != nullptr && ! l->isLive();
}

juce::String SwitchboardEditor::statusOf (const juce::String& instance) const
{
    auto* l = board->line (instance);
    return l == nullptr ? juce::String() : (l->isLive() ? juce::String() : juce::String ("not responding"));
}

void SwitchboardEditor::openAllControls (const juce::String& instance)
{
    const auto items = inView();
    juce::String scope;
    if (instance.isNotEmpty())
    {
        for (auto* i : items)
            if (i->id == instance)
                scope = i->name + " on " + trackTitle (i->track);
    }
    else
    {
        scope = juce::String ((int) items.size()) + ((int) items.size() == 1 ? " plug-in" : " plug-ins") + ", " + viewLabel();
    }
    const double t = now();
    panel->open (instance, scope, items, [this, t] (const hub::Instance& i) { return live (i, t); });
    panel->setVisible (true);
    viewport.setVisible (false);
    empty->setVisible (false);
    panel->focusSearch();
    repaint (footerArea);
}

void SwitchboardEditor::closeAllControls()
{
    panel->setVisible (false);
    refreshView (now());
    repaint (footerArea);
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
