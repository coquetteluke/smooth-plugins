#pragma once
// Registry: Switchboard's reader of member cards (PROTOCOL.md, "Reading rules"). Message thread only.
//
// One scan is a readdir of the link folder and one lstat per card name. A card is read and parsed again only when its file was
// replaced (another inode or birth time: every member write is a whole file renamed into place) or its size changed. The heartbeat
// only moves the mtime, which is all a scan needs for "alive" (mtime within 5 s) and "responding" (within 2 s), so 200 unchanged
// cards cost a fraction of a millisecond. Cards are read through one descriptor (linkdetail::readFile), never with
// juce::File::loadFileAsString, which can cut a card short when it is replaced mid read. A card that fails to parse is skipped
// until it changes, and never deleted. Once a minute a scan removes what the protocol lets a reader remove: cards with their
// inboxes, hub and temp files older than 600 s (linkdetail::removeStale, the members' own start-up sweep), nothing younger.
#include "SmoothLink.h"
#include <algorithm>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <vector>

namespace hub
{
// What the window shows for one control.
enum class Kind { knob, toggle, segments, menu };

struct Param
{
    juce::String id, name, text, label;
    int index = 0, steps = 0;
    double value = 0.0, def = 0.0;
    juce::StringArray choices;
    bool main = false, automatable = true;

    // Off and On (or two steps without names): a switch. Two or three short names: a two-way (three-way) switch. More names, or
    // long ones: a menu. Anything else: a knob, which snaps when the control has steps.
    Kind kind() const
    {
        if (choices.size() == 2 && choices[0].trim().equalsIgnoreCase ("off") && choices[1].trim().equalsIgnoreCase ("on"))
            return Kind::toggle;
        if (choices.isEmpty() && steps == 2)
            return Kind::toggle;
        if (choices.size() >= 2 && choices.size() <= 3 && choiceChars() <= 24)
            return Kind::segments;
        if (choices.size() >= 2)
            return Kind::menu;
        return Kind::knob;
    }

    int choiceChars() const
    {
        int n = 0;
        for (auto& c : choices)
            n += c.length();
        return n;
    }

    // Positions a discrete control can take: its choices, else its steps (0 for a continuous control).
    int positions() const { return choices.size() >= 2 ? choices.size() : (steps >= 2 ? steps : 0); }

    // A discrete control's position k as a normalized value, the way JUCE maps choices: k / (n - 1).
    double valueAt (int k) const
    {
        const int n = positions();
        return n >= 2 ? juce::jlimit (0.0, 1.0, (double) k / (double) (n - 1)) : 0.0;
    }

    int positionOf (double v) const
    {
        const int n = positions();
        return n >= 2 ? juce::jlimit (0, n - 1, juce::roundToInt (v * (n - 1))) : 0;
    }

    // A knob's value on its steps, when it has more than two.
    double snapped (double v) const
    {
        v = juce::jlimit (0.0, 1.0, v);
        return steps > 2 ? (double) juce::roundToInt (v * (steps - 1)) / (double) (steps - 1) : v;
    }
};

// One entry of a card's "done" list: a command the member applied.
struct Done
{
    juce::String cmd, param, error;
    bool ok = false;
};

// One member card, as read.
struct Instance
{
    juce::String id;                    // from the file name: <id>.json, <id>.in
    juce::String plugin, type, subtype, manufacturer, version;
    juce::String track;                 // empty when the card says null (Logic has not named the track yet)
    juce::String name;                  // what the window calls it: the plug-in, or "Tune Pro 2" for the second on one track
    bool hasTrack = false;
    int pid = 0;
    double started = 0.0;               // unix seconds; orders two instances of one plug-in on one track
    juce::int64 seq = 0;
    std::vector<Param> params;
    std::vector<Done> done;
    double mtime = 0.0;                 // unix seconds of the last heartbeat
    juce::int64 revision = 0;           // goes up each time the card is read again because it changed

    const Param* param (const juce::String& paramId) const
    {
        for (auto& p : params)
            if (p.id == paramId)
                return &p;
        return nullptr;
    }

    const Done* doneFor (const juce::String& cmd) const
    {
        for (auto& d : done)
            if (d.cmd == cmd)
                return &d;
        return nullptr;
    }

    // The controls the window shows on the plug-in's line: the ones it names as main (at most 8), or its first six.
    std::vector<const Param*> shown() const
    {
        std::vector<const Param*> out;
        for (auto& p : params)
            if (p.main && out.size() < 8)
                out.push_back (&p);
        if (out.empty())
            for (auto& p : params)
                if (out.size() < 6)
                    out.push_back (&p);
        return out;
    }
};

class Registry
{
public:
    static constexpr double hideAfter = 5.0;      // no heartbeat this long: hidden
    static constexpr double lateAfter = 2.0;      // no heartbeat this long: shown as not responding (it beats twice a second)
    static constexpr double cleanupEvery = 60.0;

    explicit Registry (juce::File folderToRead) : dir (std::move (folderToRead)), path (dir.getFullPathName().toStdString()) {}

    // One look at the folder at unix time now. True when what a reader sees changed: an instance came, went, was renamed, changed a
    // value, or crossed the "responding" or "alive" line.
    bool scan (double now)
    {
        parsed = 0;
        bool changed = false, rebuild = false;
        for (auto& e : entries)
            e.second.seen = false;

        if (DIR* d = opendir (path.c_str()))
        {
            const int fd = dirfd (d);
            while (auto* de = readdir (d))
            {
                const char* n = de->d_name;
                if (! smooth::linkdetail::isCardName (n) || (de->d_type != DT_REG && de->d_type != DT_UNKNOWN))
                    continue;
                struct stat st;
                if (fstatat (fd, n, &st, AT_SYMLINK_NOFOLLOW) != 0 || ! S_ISREG (st.st_mode))
                    continue;
                auto& e = entries[std::string (n, 16)];
                e.seen = true;
                e.mtime = (double) st.st_mtimespec.tv_sec + (double) st.st_mtimespec.tv_nsec / 1.0e9;
                const Key k { (juce::uint64) st.st_dev, (juce::uint64) st.st_ino, (juce::int64) st.st_size,
                              (juce::int64) st.st_birthtimespec.tv_sec, (juce::int64) st.st_birthtimespec.tv_nsec };
                if (! e.known || ! (k == e.key))
                {
                    e.key = k;
                    e.known = true;
                    const bool wasGood = e.good;
                    e.good = read (n, e.instance);
                    ++parsed;
                    if (e.good)
                        e.instance.revision = ++revisions;
                    rebuild = rebuild || e.good || wasGood;
                }
                e.instance.mtime = e.mtime;
            }
            closedir (d);
        }

        for (auto it = entries.begin(); it != entries.end();)
        {
            if (! it->second.seen)
            {
                rebuild = rebuild || it->second.alive;
                it = entries.erase (it);
            }
            else
            {
                ++it;
            }
        }
        for (auto& [id, e] : entries)
        {
            const bool alive = e.good && now - e.mtime <= hideAfter;
            if (alive != e.alive)
            {
                e.alive = alive;
                rebuild = true;
            }
            const bool late = alive && now - e.mtime > lateAfter;
            if (late != e.late)
            {
                e.late = late;
                changed = true;
            }
        }
        if (rebuild)
        {
            rebuildVisible();
            changed = true;
        }
        if (now - lastCleanup >= cleanupEvery || now < lastCleanup)
        {
            lastCleanup = now;
            smooth::linkdetail::removeStale (dir);
        }
        return changed;
    }

    // Every instance with a heartbeat in the last 5 s: by track (named tracks in natural order, no name last), then oldest first.
    const std::vector<const Instance*>& instances() const { return visible; }

    // The tracks of those instances, in the same order. "" stands for the cards with no track name.
    juce::StringArray tracks() const
    {
        juce::StringArray out;
        for (auto* i : visible)
            if (out.isEmpty() || out[out.size() - 1] != i->track)
                out.add (i->track);
        return out;
    }

    // The instances on one track (exact name; "" for no name), oldest first.
    std::vector<const Instance*> onTrack (const juce::String& track) const
    {
        std::vector<const Instance*> out;
        for (auto* i : visible)
            if (i->track == track)
                out.push_back (i);
        return out;
    }

    // A shown instance by id, or nullptr.
    const Instance* find (const juce::String& id) const
    {
        const auto it = entries.find (id.toStdString());
        return it != entries.end() && it->second.alive ? &it->second.instance : nullptr;
    }

    // Its heartbeat is less than 2 s old.
    static bool responding (const Instance& i, double now) { return now - i.mtime <= lateAfter; }

    // Tests: cards read and parsed in the last scan.
    int parsedLastScan() const { return parsed; }
    juce::File folder() const { return dir; }

private:
    struct Key
    {
        juce::uint64 dev = 0, ino = 0;
        juce::int64 size = 0, birthSec = 0, birthNsec = 0;
        bool operator== (const Key& o) const { return dev == o.dev && ino == o.ino && size == o.size && birthSec == o.birthSec && birthNsec == o.birthNsec; }
    };

    struct Entry
    {
        Key key;
        bool known = false, good = false, alive = false, late = false, seen = false;
        double mtime = 0.0;
        Instance instance;
    };

    static double number (const juce::var& v)
    {
        if (v.isDouble() || v.isInt() || v.isInt64() || v.isBool())
        {
            const double d = (double) v;
            return std::isfinite (d) ? d : 0.0;
        }
        return 0.0;
    }

    static juce::String text (const juce::var& v) { return v.isString() ? v.toString() : juce::String(); }

    // Parses <id>.json into out. False (and out untouched) for anything that is not a card: not JSON, not an object, no params.
    bool read (const char* fileName, Instance& out) const
    {
        std::string bytes;
        if (! smooth::linkdetail::readFile (path + "/" + fileName, bytes, 4 * 1024 * 1024) || bytes.empty())
            return false;
        juce::var v;
        if (juce::JSON::parse (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), v).failed() || ! v.isObject())
            return false;
        const auto* params = v["params"].getArray();
        if (params == nullptr)
            return false;

        Instance i;
        i.id = juce::String (juce::CharPointer_UTF8 (fileName)).substring (0, 16);
        i.plugin = text (v["plugin"]).trim();
        if (i.plugin.isEmpty())
            i.plugin = "Plug-in";
        i.type = text (v["type"]);
        i.subtype = text (v["subtype"]);
        i.manufacturer = text (v["manufacturer"]);
        i.version = text (v["version"]);
        i.track = text (v["track"]);
        i.hasTrack = i.track.isNotEmpty();
        i.pid = (int) number (v["pid"]);
        i.started = number (v["started"]);
        i.seq = (juce::int64) number (v["seq"]);
        for (const auto& p : *params)
        {
            if (! p.isObject())
                continue;
            Param q;
            q.id = text (p["id"]);
            if (q.id.isEmpty())
                continue;
            q.index = (int) number (p["index"]);
            q.name = text (p["name"]);
            if (q.name.isEmpty())
                q.name = q.id;
            q.text = text (p["text"]);
            q.label = text (p["label"]);
            q.value = juce::jlimit (0.0, 1.0, number (p["value"]));
            q.def = juce::jlimit (0.0, 1.0, number (p["default"]));
            q.steps = juce::jmax (0, (int) number (p["steps"]));
            if (const auto* ch = p["choices"].getArray())
                for (const auto& c : *ch)
                    q.choices.add (c.toString());
            q.main = p["main"].isBool() && (bool) p["main"];
            q.automatable = ! p["automatable"].isBool() || (bool) p["automatable"];
            i.params.push_back (std::move (q));
        }
        if (const auto* done = v["done"].getArray())
            for (const auto& d : *done)
                if (d.isObject())
                    i.done.push_back ({ text (d["cmd"]), text (d["param"]), text (d["error"]), d["ok"].isBool() && (bool) d["ok"] });
        out = std::move (i);
        return true;
    }

    static bool trackBefore (const Instance& a, const Instance& b)
    {
        if (a.hasTrack != b.hasTrack)
            return a.hasTrack;
        const int c = a.track.compareNatural (b.track, false);
        return c != 0 ? c < 0 : a.track < b.track;
    }

    void rebuildVisible()
    {
        visible.clear();
        for (auto& [id, e] : entries)
            if (e.alive)
                visible.push_back (&e.instance);
        std::sort (visible.begin(), visible.end(), [] (const Instance* a, const Instance* b)
        {
            if (a->track != b->track)
                return trackBefore (*a, *b);
            if (! juce::exactlyEqual (a->started, b->started))
                return a->started < b->started;
            return a->id < b->id;
        });
        // Two of one plug-in on one track (or on two tracks with one name): Name, Name 2, Name 3, oldest first.
        std::map<std::pair<juce::String, juce::String>, int> count;
        for (auto* i : visible)
        {
            const int n = ++count[{ i->track, i->plugin }];
            const_cast<Instance*> (i)->name = n == 1 ? i->plugin : i->plugin + " " + juce::String (n);
        }
    }

    const juce::File dir;
    const std::string path;
    std::map<std::string, Entry> entries;   // by id
    std::vector<const Instance*> visible;
    juce::int64 revisions = 0;
    double lastCleanup = -1.0e18;
    int parsed = 0;
};
} // namespace hub
