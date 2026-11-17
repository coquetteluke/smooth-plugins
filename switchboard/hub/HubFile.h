#pragma once
// hub-<id>.json and hub-<id>.in/: the presence file of one Switchboard instance and its inbox, from PROTOCOL.md:
//   { "link": 1, "hub": true, "id": "...", "track": "Stereo Out" | null, "version": "1.1.0",
//     "showing": { "screen": "track", "track": "Lead Vox" },
//     "done": [ { "cmd": "...", "ok": true, "error": "", "showing": { ... } } ] }
// Same folder, atomic write and mtime heartbeat as a member card, and the same rule for when it is on (AudioUnit always, anything
// else only with SMOOTH_LINK_DIR). The inbox is made before the file is first written (a reader takes an inbox as the sign of
// 1.1.0) and both go on destruction. The processor's timer drives it: pollNow for the heartbeat and rewrites, takeCommands for the
// inbox. Message thread only, except setTrackName and trackName (any thread).
#include "SmoothLink.h"
#include <deque>

namespace smooth
{
class HubFile
{
public:
    // One command from the inbox, as the hub file's "done" list keeps it.
    struct Done
    {
        juce::String cmd, error;
        bool ok = false;
        juce::var showing;   // the screen right after the command (as it stayed, when it was refused)
    };

    static constexpr int doneKept = 16, maxCommandsPerPoll = 64;
    static constexpr long maxCommandBytes = 64 * 1024;

    HubFile (juce::AudioProcessor::WrapperType wrapper, juce::String versionString, juce::var initialShowing)
        : dir (Link::folder()), on (Link::enabledFor (wrapper) && dir != juce::File()), version (std::move (versionString)),
          showing (std::move (initialShowing))
    {
        instanceId = linkdetail::randomHex (8);
        if (! on)
            return;
        linkdetail::makeFolders (dir);
        for (int tries = 0; tries < 8 && (dir.getChildFile ("hub-" + instanceId + ".json").exists() || dir.getChildFile ("hub-" + instanceId + ".in").exists()); ++tries)
            instanceId = linkdetail::randomHex (8);
        hubFile = dir.getChildFile ("hub-" + instanceId + ".json");
        inboxDir = dir.getChildFile ("hub-" + instanceId + ".in");
        mkdir (inboxDir.getFullPathName().toRawUTF8(), 0755);
        write (now());
    }

    ~HubFile()
    {
        if (! on)
            return;
        unlink (hubFile.getFullPathName().toRawUTF8());
        linkdetail::removeTree (inboxDir.getFullPathName().toStdString());
    }

    // Any thread. Empty means null (no name from the host).
    void setTrackName (const juce::String& name)
    {
        {
            const std::lock_guard<std::mutex> lock (trackLock);
            track = name;
        }
        trackDirty.store (true);
    }

    // Any thread. Empty until the host sends a name.
    juce::String trackName() const
    {
        const std::lock_guard<std::mutex> lock (trackLock);
        return track;
    }

    bool enabled() const { return on; }
    juce::String id() const { return instanceId; }
    juce::File file() const { return on ? hubFile : dir.getChildFile ("hub-" + instanceId + ".json"); }
    juce::File inbox() const { return on ? inboxDir : dir.getChildFile ("hub-" + instanceId + ".in"); }

    // What the window shows, for "showing". Written at once.
    void setShowing (const juce::var& s)
    {
        showing = s;
        if (on)
            write (now());
    }

    // A rewrite when the name changed (or the last write failed), else the heartbeat when due. The processor's timer calls this.
    void pollNow()
    {
        if (! on)
            return;
        const double t = now();
        if (trackDirty.exchange (false) || writeFailed)
            write (t);
        else if (t - lastTouch >= linkdetail::touchInterval || t < lastTouch)
        {
            if (utimes (hubFile.getFullPathName().toRawUTF8(), nullptr) == 0)
                lastTouch = t;
            else
                write (t);
        }
    }

    // Takes up to 64 command files from the inbox in name order (a name starting with "." is never read): reads each whole, deletes
    // it, and hands its name (without .json) and text to apply, which says what became of it. A file it cannot read is refused without
    // apply. Each goes into "done", and the file is rewritten at once. Returns how many it took.
    int takeCommands (const std::function<Done (const juce::String& name, const juce::String& text)>& apply)
    {
        if (! on)
            return 0;
        const auto path = inboxDir.getFullPathName().toStdString();
        DIR* d = opendir (path.c_str());
        if (d == nullptr)
        {
            if (errno == ENOENT)
            {
                linkdetail::makeFolders (dir);
                mkdir (path.c_str(), 0755);
            }
            return 0;
        }
        std::vector<std::string> names;
        while (auto* e = readdir (d))
        {
            const char* n = e->d_name;
            const size_t len = std::strlen (n);
            if (n[0] == '.' || len <= 5 || std::strcmp (n + len - 5, ".json") != 0)
                continue;
            if (e->d_type != DT_REG && e->d_type != DT_UNKNOWN)
                continue;
            names.emplace_back (n);
        }
        closedir (d);
        if (names.empty())
            return 0;
        std::sort (names.begin(), names.end());
        if (names.size() > (size_t) maxCommandsPerPoll)
            names.resize ((size_t) maxCommandsPerPoll);

        int taken = 0;
        for (const auto& n : names)
        {
            const auto file = path + "/" + n;
            const auto name = juce::String (juce::CharPointer_UTF8 (n.substr (0, n.size() - 5).c_str()));
            std::string bytes;
            Done result;
            if (! linkdetail::readFile (file, bytes, maxCommandBytes))
            {
                if (errno == ENOENT && ! linkdetail::exists (file))
                    continue;   // its writer took it back
                unlink (file.c_str());
                result.error = (long) bytes.size() > maxCommandBytes ? "the command file is larger than 64 KB" : "the command file could not be read";
                result.showing = showing;
            }
            else
            {
                unlink (file.c_str());
                result = apply (name, juce::String::fromUTF8 (bytes.data(), (int) bytes.size()));
            }
            result.cmd = name;
            done.push_back (std::move (result));
            while ((int) done.size() > doneKept)
                done.pop_front();
            ++taken;
        }
        if (taken > 0)
            write (now());
        return taken;
    }

    std::function<double()> clock { [] { return juce::Time::getMillisecondCounterHiRes() / 1000.0; } };

private:
    double now() const { return clock ? clock() : juce::Time::getMillisecondCounterHiRes() / 1000.0; }

    void write (double t)
    {
        const auto name = trackName();
        auto* o = new juce::DynamicObject();
        juce::var v (o);
        o->setProperty ("link", linkdetail::protocolVersion);
        o->setProperty ("hub", true);
        o->setProperty ("id", instanceId);
        o->setProperty ("track", name.isEmpty() ? juce::var() : juce::var (name));
        o->setProperty ("version", version);
        if (! showing.isVoid())
            o->setProperty ("showing", showing);
        juce::Array<juce::var> list;
        for (const auto& d : done)
        {
            auto* e = new juce::DynamicObject();
            e->setProperty ("cmd", d.cmd);
            e->setProperty ("ok", d.ok);
            e->setProperty ("error", d.error);
            e->setProperty ("showing", d.showing);
            list.add (juce::var (e));
        }
        o->setProperty ("done", list);
        const auto text = juce::JSON::toString (v, juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none)) + "\n";
        bool ok = linkdetail::writeAtomically (hubFile, text);
        if (! ok && linkdetail::makeFolders (dir))
            ok = linkdetail::writeAtomically (hubFile, text);
        writeFailed = ! ok;
        lastTouch = t;
    }

    const juce::File dir;
    const bool on;
    const juce::String version;
    juce::String instanceId;
    juce::File hubFile, inboxDir;
    mutable std::mutex trackLock;
    juce::String track;
    std::atomic<bool> trackDirty { false };
    juce::var showing;
    std::deque<Done> done;
    bool writeFailed = false;
    double lastTouch = 0.0;

    JUCE_DECLARE_NON_COPYABLE (HubFile)
};
} // namespace smooth
