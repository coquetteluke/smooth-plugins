#pragma once
// SmoothLink.h: the member side of the link protocol (switchboard/PROTOCOL.md, link 1). Header only, JUCE 8.
//
// A plug-in adds exactly three things:
//     #include "SmoothLink.h"
//     smooth::Link link { *this, smooth::Link::Info::fromBuild ({ "mainId1", "mainId2" }) };   // declared after the parameters
//     void updateTrackProperties (const TrackProperties& p) override { link.setTrackName (p.name.value_or (juce::String())); }
//
// What it does: publishes a JSON card of every parameter into the shared folder, keeps the card's modification time fresh as a
// heartbeat, and applies set commands from its inbox by moving the plug-in's own parameter with begin and end gestures, exactly as
// a mouse drag in the plug-in's window does, so Logic's automation stays right.
//
// Real-time rules: parameterValueChanged may run on the audio thread, so it only stores an atomic flag. Nothing here is reachable
// from processBlock. Card writes, inbox polls and gestures run on the message thread (a 30 Hz juce::Timer); the constructor writes
// the first card at once.
//
// On for the AudioUnit wrapper. For anything else (a plug-in's own PluginTest, a console tool) only when SMOOTH_LINK_DIR holds an
// absolute path, so tests never write into the real folder unless they ask to.
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <map>
#include <mutex>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace smooth
{
namespace linkdetail
{
    constexpr int protocolVersion = 1;
    constexpr double staleSeconds = 600.0;     // older files of others may be deleted
    constexpr double writeInterval = 0.25;     // at most 4 card writes a second while values keep changing
    constexpr double touchInterval = 0.5;      // heartbeat; the protocol asks for at least once a second
    constexpr double gestureTimeout = 2.0;     // an open drag with no move or end for this long is ended
    constexpr int pollHz = 30;
    constexpr int maxCommandsPerPoll = 64;
    constexpr int doneKept = 16;
    constexpr long maxCommandBytes = 64 * 1024;

    // The real home folder from the user database, never HOME, which a host may redirect (as KeyFile.h does).
    inline juce::String realHome()
    {
        struct passwd entry;
        struct passwd* result = nullptr;
        char buffer[4096];
        if (getpwuid_r (getuid(), &entry, buffer, sizeof buffer, &result) != 0 || result == nullptr || result->pw_dir == nullptr)
            return {};
        return juce::String (juce::CharPointer_UTF8 (result->pw_dir));
    }

    // SMOOTH_LINK_DIR when it holds an absolute path, else empty.
    inline juce::String envFolder()
    {
        const char* e = std::getenv ("SMOOTH_LINK_DIR");
        if (e == nullptr || e[0] != '/')
            return {};
        return juce::String (juce::CharPointer_UTF8 (e));
    }

    inline juce::String randomHex (int bytes)
    {
        unsigned char b[16] {};
        bytes = juce::jlimit (1, 16, bytes);
        arc4random_buf (b, (size_t) bytes);
        static const char digits[] = "0123456789abcdef";
        juce::String s;
        for (int i = 0; i < bytes; ++i)
            s << juce::String::charToString ((juce::juce_wchar) digits[b[i] >> 4]) << juce::String::charToString ((juce::juce_wchar) digits[b[i] & 15]);
        return s;
    }

    inline juce::String fourCC (juce::uint32 v)
    {
        juce::String s;
        for (int shift = 24; shift >= 0; shift -= 8)
            s << juce::String::charToString ((juce::juce_wchar) ((v >> shift) & 0xff));
        return s;
    }

    inline bool isHex16 (const char* s)
    {
        for (int i = 0; i < 16; ++i)
            if (! ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
                return false;
        return true;
    }

    // <id>.json
    inline bool isCardName (const char* n) { return std::strlen (n) == 21 && isHex16 (n) && std::strcmp (n + 16, ".json") == 0; }
    // <id>.in
    inline bool isInboxName (const char* n) { return std::strlen (n) == 19 && isHex16 (n) && std::strcmp (n + 16, ".in") == 0; }
    // hub-<id>.json
    inline bool isHubName (const char* n) { return std::strlen (n) == 25 && std::strncmp (n, "hub-", 4) == 0 && isHex16 (n + 4) && std::strcmp (n + 20, ".json") == 0; }
    // <id>.json.tmp<random> or hub-<id>.json.tmp<random>: a write cut short by a crash
    inline bool isTempName (const char* n)
    {
        const size_t len = std::strlen (n);
        if (len > 25 && std::strncmp (n, "hub-", 4) == 0 && isHex16 (n + 4) && std::strncmp (n + 20, ".json.tmp", 9) == 0)
            return true;
        return len > 21 && isHex16 (n) && std::strncmp (n + 16, ".json.tmp", 9) == 0;
    }

    inline double unixNow()
    {
        struct timeval tv;
        gettimeofday (&tv, nullptr);
        return (double) tv.tv_sec + (double) tv.tv_usec / 1.0e6;
    }

    // Seconds since the path was last modified, or -1 when it cannot be read.
    inline double ageOf (const std::string& path, double now)
    {
        struct stat s;
        if (lstat (path.c_str(), &s) != 0)
            return -1.0;
        return now - ((double) s.st_mtimespec.tv_sec + (double) s.st_mtimespec.tv_nsec / 1.0e9);
    }

    inline bool exists (const std::string& path)
    {
        struct stat s;
        return lstat (path.c_str(), &s) == 0;
    }

    // Every missing folder on the way, mode 0755.
    inline bool makeFolders (const juce::File& f)
    {
        const auto path = f.getFullPathName().toStdString();
        struct stat s;
        if (stat (path.c_str(), &s) == 0)
            return S_ISDIR (s.st_mode);
        std::string partial;
        for (size_t i = 1; i <= path.size(); ++i)
            if (i == path.size() || path[i] == '/')
            {
                partial = path.substr (0, i);
                if (mkdir (partial.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
            }
        return stat (path.c_str(), &s) == 0 && S_ISDIR (s.st_mode);
    }

    // A folder and everything in it. Never follows a link.
    inline void removeTree (const std::string& path)
    {
        struct stat s;
        if (lstat (path.c_str(), &s) != 0)
            return;
        if (! S_ISDIR (s.st_mode))
        {
            unlink (path.c_str());
            return;
        }
        if (DIR* d = opendir (path.c_str()))
        {
            while (auto* e = readdir (d))
                if (std::strcmp (e->d_name, ".") != 0 && std::strcmp (e->d_name, "..") != 0)
                    removeTree (path + "/" + e->d_name);
            closedir (d);
        }
        rmdir (path.c_str());
    }

    // Reads a whole file through one open descriptor, to its end. Readers of cards must read this way: a card is replaced by
    // rename at any moment, and juce::File::loadFileAsString sizes its read from whatever file is at the path by then, which can
    // cut the older card it opened short.
    inline bool readFile (const std::string& path, std::string& out, long maxBytes = 16 * 1024 * 1024)
    {
        out.clear();
        const int fd = open (path.c_str(), O_RDONLY);
        if (fd < 0)
            return false;
        char chunk[8192];
        bool ok = true;
        for (;;)
        {
            const ssize_t n = read (fd, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                ok = false;
            if (n <= 0)
                break;
            out.append (chunk, (size_t) n);
            if ((long) out.size() > maxBytes)
            {
                ok = false;
                break;
            }
        }
        close (fd);
        return ok;
    }

    // Whole file to a temporary name in the same folder, then rename: a reader sees the old file or the new one, never part.
    inline bool writeAtomically (const juce::File& target, const juce::String& text)
    {
        const auto finalPath = target.getFullPathName().toStdString();
        const auto tempPath = finalPath + ".tmp" + randomHex (4).toStdString();
        const int fd = open (tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0)
            return false;
        const char* bytes = text.toRawUTF8();
        size_t left = std::strlen (bytes);
        bool ok = true;
        while (ok && left > 0)
        {
            const ssize_t n = write (fd, bytes, left);
            if (n < 0 && errno == EINTR)
                continue;
            ok = n > 0;
            if (ok)
            {
                bytes += n;
                left -= (size_t) n;
            }
        }
        ok = (close (fd) == 0) && ok;
        if (! ok || rename (tempPath.c_str(), finalPath.c_str()) != 0)
        {
            unlink (tempPath.c_str());
            return false;
        }
        return true;
    }

    // On start, from PROTOCOL.md: cards (with their inboxes) and hub files older than 600 s, and inboxes with no card older than
    // 600 s. Also a temporary card older than 600 s, which only a crash between write and rename leaves. Nothing younger.
    inline void removeStale (const juce::File& folder)
    {
        const auto dir = folder.getFullPathName().toStdString();
        const double now = unixNow();
        std::vector<std::string> cards, inboxes, others;
        if (DIR* d = opendir (dir.c_str()))
        {
            while (auto* e = readdir (d))
            {
                const char* n = e->d_name;
                if (isCardName (n)) cards.emplace_back (n);
                else if (isInboxName (n)) inboxes.emplace_back (n);
                else if (isHubName (n) || isTempName (n)) others.emplace_back (n);
            }
            closedir (d);
        }
        for (const auto& c : cards)
            if (ageOf (dir + "/" + c, now) > staleSeconds)
            {
                unlink ((dir + "/" + c).c_str());
                removeTree (dir + "/" + c.substr (0, 16) + ".in");
            }
        for (const auto& o : others)
            if (ageOf (dir + "/" + o, now) > staleSeconds)
                unlink ((dir + "/" + o).c_str());
        for (const auto& in : inboxes)
            if (! exists (dir + "/" + in.substr (0, 16) + ".json") && ageOf (dir + "/" + in, now) > staleSeconds)
                removeTree (dir + "/" + in);
    }
} // namespace linkdetail

class Link : private juce::Timer, private juce::AudioProcessorParameter::Listener
{
public:
    struct Info
    {
        juce::String plugin, type, subtype, manufacturer { "Luke" }, version;
        juce::StringArray mainIds;

        // Name, AU type, subtype, manufacturer and version from the JucePlugin_ macros of the plug-in build. A plug-in's own
        // PluginTest console app has no such macros: there the build fields stay empty and the Link takes the plug-in name from
        // AudioProcessor::getName(), so the same member line compiles in both.
        static Info fromBuild (juce::StringArray mainIds)
        {
            Info i;
           #if defined (JucePlugin_Name)
            i.plugin = JucePlugin_Name;
           #endif
           #if defined (JucePlugin_AUMainType)
            i.type = linkdetail::fourCC ((juce::uint32) JucePlugin_AUMainType);
           #endif
           #if defined (JucePlugin_AUSubType)
            i.subtype = linkdetail::fourCC ((juce::uint32) JucePlugin_AUSubType);
           #endif
           #if defined (JucePlugin_AUManufacturerCode)
            i.manufacturer = linkdetail::fourCC ((juce::uint32) JucePlugin_AUManufacturerCode);
           #endif
           #if defined (JucePlugin_VersionString)
            i.version = JucePlugin_VersionString;
           #endif
            i.mainIds = std::move (mainIds);
            return i;
        }
    };

    // Declare after the parameter state. Writes the card at once when enabled.
    Link (juce::AudioProcessor& p, Info i)
        : processor (p), info (std::move (i)), dir (folder()), on (enabledFor (p.wrapperType) && dir != juce::File())
    {
        if (info.plugin.isEmpty())
            info.plugin = p.getName();
        startedUnix = (double) juce::Time::currentTimeMillis() / 1000.0;
        instanceId = linkdetail::randomHex (8);
        if (! on)
            return;
        linkdetail::makeFolders (dir);
        linkdetail::removeStale (dir);
        for (int tries = 0; tries < 8 && (dir.getChildFile (instanceId + ".json").exists() || dir.getChildFile (instanceId + ".in").exists()); ++tries)
            instanceId = linkdetail::randomHex (8);
        cardFile = dir.getChildFile (instanceId + ".json");
        inboxDir = dir.getChildFile (instanceId + ".in");
        mkdir (inboxDir.getFullPathName().toRawUTF8(), 0755);
        syncParameters();
        writeCard (now());
        startTimerHz (linkdetail::pollHz);
    }

    // Ends any open gesture and deletes the card, everything in the inbox, and the inbox.
    ~Link() override
    {
        stopTimer();
        if (! on)
            return;
        for (auto& g : openGestures)
            if (juce::isPositiveAndBelow (g.first, (int) entries.size()))
                entries[(size_t) g.first].param->endChangeGesture();
        openGestures.clear();
        for (auto& e : entries)
            e.param->removeListener (this);
        unlink (cardFile.getFullPathName().toRawUTF8());
        linkdetail::removeTree (inboxDir.getFullPathName().toStdString());
    }

    // Any thread. Empty means null (no name from the host).
    void setTrackName (const juce::String& name)
    {
        {
            const std::lock_guard<std::mutex> lock (trackLock);
            pendingTrack = name;
        }
        trackDirty.store (true);
    }

    bool enabled() const { return on; }
    juce::String id() const { return instanceId; }
    juce::File card() const { return on ? cardFile : dir.getChildFile (instanceId + ".json"); }
    juce::File inbox() const { return on ? inboxDir : dir.getChildFile (instanceId + ".in"); }

    // SMOOTH_LINK_DIR when it holds an absolute path, else ~/Library/Application Support/SmoothPlugins/link.
    static juce::File folder()
    {
        const auto e = linkdetail::envFolder();
        if (e.isNotEmpty())
            return juce::File (e);
        const auto home = linkdetail::realHome();
        if (! home.startsWithChar ('/'))
            return {};
        return juce::File (home + "/Library/Application Support/SmoothPlugins/link");
    }

    // AudioUnit: yes. Anything else only when SMOOTH_LINK_DIR holds an absolute path.
    static bool enabledFor (juce::AudioProcessor::WrapperType w)
    {
        return w == juce::AudioProcessor::wrapperType_AudioUnit || linkdetail::envFolder().isNotEmpty();
    }

    // One inbox poll and any due write, on the calling thread. The timer calls this 30 times a second; tests call it by hand.
    void pollNow()
    {
        if (! on)
            return;
        const double t = now();
        syncParameters();
        const bool applied = readInbox (t);
        endIdleGestures (t);
        const bool trackChanged = trackDirty.exchange (false);
        if (! dirty.load (std::memory_order_relaxed) && (t - lastScan >= linkdetail::writeInterval || t < lastScan))
        {
            // A value set without telling its listeners still reaches the card.
            lastScan = t;
            if (valuesMoved())
                dirty.store (true, std::memory_order_relaxed);
        }
        const bool due = dirty.load (std::memory_order_relaxed) && (t - lastWrite >= linkdetail::writeInterval || t < lastWrite);
        const bool retry = writeFailed && (t - lastWrite >= linkdetail::writeInterval || t < lastWrite);
        if (applied || trackChanged || due || retry)
            writeCard (t);
        else if (t - lastTouch >= linkdetail::touchInterval || t < lastTouch)
        {
            if (utimes (cardFile.getFullPathName().toRawUTF8(), nullptr) == 0)
                lastTouch = t;
            else
                writeCard (t);   // the card went missing: write it again
        }
    }

    // Tests: seconds on any steady clock.
    std::function<double()> clock { [] { return juce::Time::getMillisecondCounterHiRes() / 1000.0; } };

private:
    enum class Gesture { tap, begin, move, end };

    struct Entry
    {
        juce::AudioProcessorParameter* param = nullptr;
        juce::String id;
        juce::StringArray choices;
        int steps = 0;
        bool main = false;
    };

    struct Done
    {
        juce::String cmd, param, text, error;
        bool ok = false, hasValue = false;
        double value = 0.0;
    };

    double now() const { return clock ? clock() : juce::Time::getMillisecondCounterHiRes() / 1000.0; }

    void timerCallback() override { pollNow(); }

    // May run on the audio thread: an atomic flag and nothing else.
    void parameterValueChanged (int, float) override { dirty.store (true, std::memory_order_relaxed); }
    void parameterGestureChanged (int, bool) override {}

    static bool looksLikeNumber (juce::String s, const juce::String& label)
    {
        s = s.trim();
        if (label.isNotEmpty() && s.endsWith (label))
            s = s.dropLastCharacters (label.length()).trim();
        return s.isNotEmpty() && s.containsAnyOf ("0123456789") && s.containsOnly ("0123456789.,-+ ");
    }

    // The names of a discrete control's values: every choice of a choice or on/off control, and the values of another discrete
    // control with at most 32 steps when they are words rather than numbers. Numbers alone are not choices.
    static juce::StringArray choicesOf (juce::AudioProcessorParameter& p)
    {
        if (! p.isDiscrete())
            return {};
        const int steps = p.getNumSteps();
        const bool named = dynamic_cast<juce::AudioParameterChoice*> (&p) != nullptr || dynamic_cast<juce::AudioParameterBool*> (&p) != nullptr;
        if (steps < 2 || (! named && steps > 32))
            return {};
        const auto all = p.getAllValueStrings();
        if (all.size() != steps)
            return {};
        if (named)
            return all;
        for (const auto& s : all)
            if (! looksLikeNumber (s, p.getLabel()))
                return all;
        return {};
    }

    static juce::String textOf (juce::AudioProcessorParameter& p, float v)
    {
        auto t = p.getText (v, 64);
        const auto label = p.getLabel();
        if (label.isNotEmpty() && ! t.endsWith (label))
            t << " " << label;
        return t;
    }

    // Listens to every parameter; again whenever the list changes size (a plug-in that adds controls late).
    void syncParameters()
    {
        const auto& params = processor.getParameters();
        if (params.size() == (int) entries.size())
            return;
        for (auto& e : entries)
            e.param->removeListener (this);
        entries.clear();
        byId.clear();
        for (int i = 0; i < params.size(); ++i)
        {
            Entry e;
            e.param = params[i];
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (e.param))
                e.id = hosted->getParameterID();
            if (e.id.isEmpty())
                e.id = "#" + juce::String (i);
            e.steps = e.param->isDiscrete() ? e.param->getNumSteps() : 0;
            e.choices = choicesOf (*e.param);
            e.main = info.mainIds.contains (e.id);
            e.param->addListener (this);
            byId.emplace (e.id, i);
            entries.push_back (std::move (e));
        }
        dirty.store (true, std::memory_order_relaxed);
    }

    bool valuesMoved() const
    {
        if (written.size() != entries.size())
            return true;
        for (size_t i = 0; i < entries.size(); ++i)
            if (! juce::exactlyEqual (entries[i].param->getValue(), written[i]))
                return true;
        return false;
    }

    void writeCard (double t)
    {
        dirty.store (false, std::memory_order_relaxed);
        juce::String track;
        {
            const std::lock_guard<std::mutex> lock (trackLock);
            track = pendingTrack;
        }
        std::vector<float> values;
        values.reserve (entries.size());

        auto* card = new juce::DynamicObject();
        juce::var cardVar (card);
        card->setProperty ("link", linkdetail::protocolVersion);
        card->setProperty ("id", instanceId);
        card->setProperty ("plugin", info.plugin);
        card->setProperty ("type", info.type);
        card->setProperty ("subtype", info.subtype);
        card->setProperty ("manufacturer", info.manufacturer);
        card->setProperty ("version", info.version);
        card->setProperty ("track", track.isEmpty() ? juce::var() : juce::var (track));
        card->setProperty ("pid", (int) getpid());
        card->setProperty ("started", startedUnix);
        card->setProperty ("seq", (juce::int64) (seq + 1));

        juce::Array<juce::var> params;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            auto& p = *e.param;
            const float v = p.getValue();
            values.push_back (v);
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", e.id);
            o->setProperty ("index", (int) i);
            o->setProperty ("name", p.getName (1024));
            o->setProperty ("value", (double) v);
            o->setProperty ("text", textOf (p, v));
            o->setProperty ("default", (double) p.getDefaultValue());
            o->setProperty ("steps", e.steps);
            juce::Array<juce::var> choices;
            for (const auto& c : e.choices)
                choices.add (c);
            o->setProperty ("choices", choices);
            o->setProperty ("label", p.getLabel());
            o->setProperty ("automatable", p.isAutomatable());
            o->setProperty ("main", e.main);
            params.add (juce::var (o));
        }
        card->setProperty ("params", params);

        juce::Array<juce::var> doneList;
        for (const auto& d : done)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("cmd", d.cmd);
            o->setProperty ("ok", d.ok);
            o->setProperty ("param", d.param);
            o->setProperty ("value", d.hasValue ? juce::var (d.value) : juce::var());
            o->setProperty ("text", d.text);
            o->setProperty ("error", d.error);
            doneList.add (juce::var (o));
        }
        card->setProperty ("done", doneList);

        const auto text = juce::JSON::toString (cardVar, juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none)) + "\n";
        bool ok = linkdetail::writeAtomically (cardFile, text);
        if (! ok && linkdetail::makeFolders (dir))
            ok = linkdetail::writeAtomically (cardFile, text);
        lastWrite = t;
        if (ok)
        {
            ++seq;
            lastTouch = t;
            lastScan = t;
            written = std::move (values);
            writeFailed = false;
        }
        else
        {
            writeFailed = true;
        }
    }

    // Applies up to 64 commands in name order. True when it handled any.
    bool readInbox (double t)
    {
        const auto inboxPath = inboxDir.getFullPathName().toStdString();
        DIR* d = opendir (inboxPath.c_str());
        if (d == nullptr)
        {
            if (errno == ENOENT)
            {
                linkdetail::makeFolders (dir);
                mkdir (inboxPath.c_str(), 0755);
            }
            return false;
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
            return false;
        std::sort (names.begin(), names.end());
        if (names.size() > (size_t) linkdetail::maxCommandsPerPoll)
            names.resize ((size_t) linkdetail::maxCommandsPerPoll);
        bool any = false;
        for (const auto& n : names)
            any = applyFile (inboxPath + "/" + n, n.substr (0, n.size() - 5), t) || any;
        return any;
    }

    // Reads, deletes and applies one command file, and records it in done. False when the file was already gone.
    bool applyFile (const std::string& path, const std::string& name, double t)
    {
        Done result;
        result.cmd = juce::String (juce::CharPointer_UTF8 (name.c_str()));
        std::string bytes;
        if (! linkdetail::readFile (path, bytes, linkdetail::maxCommandBytes))
        {
            if (errno == ENOENT && ! linkdetail::exists (path))
                return false;   // its writer took it back
            unlink (path.c_str());
            result.error = (long) bytes.size() > linkdetail::maxCommandBytes ? "the command file is larger than 64 KB" : "the command file could not be read";
            record (result);
            return true;
        }
        unlink (path.c_str());
        apply (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), result, t);
        record (result);
        return true;
    }

    void record (const Done& d)
    {
        done.push_back (d);
        while ((int) done.size() > linkdetail::doneKept)
            done.pop_front();
    }

    static bool hasProperty (const juce::var& v, const char* key) { return v.getDynamicObject() != nullptr && v.getDynamicObject()->hasProperty (key); }

    void apply (const juce::String& text, Done& result, double t)
    {
        juce::var cmd;
        if (juce::JSON::parse (text, cmd).failed())
        {
            result.error = "not valid JSON";
            return;
        }
        if (! cmd.isObject())
        {
            result.error = "not a JSON object";
            return;
        }
        if (! hasProperty (cmd, "param") || ! cmd["param"].isString() || cmd["param"].toString().isEmpty())
        {
            result.error = "no param";
            return;
        }
        result.param = cmd["param"].toString();
        const auto found = byId.find (result.param);
        if (found == byId.end())
        {
            result.error = "unknown param " + result.param;
            return;
        }
        const int index = found->second;
        auto& entry = entries[(size_t) index];
        auto& p = *entry.param;
        const auto current = [&]
        {
            result.hasValue = true;
            result.value = p.getValue();
            result.text = textOf (p, p.getValue());
        };
        const bool hasValue = hasProperty (cmd, "value"), hasText = hasProperty (cmd, "text");
        if (hasValue == hasText)
        {
            result.error = hasValue ? "both value and text" : "neither value nor text";
            current();
            return;
        }
        auto gesture = Gesture::tap;
        if (hasProperty (cmd, "gesture"))
        {
            const auto g = cmd["gesture"].toString();
            if (! cmd["gesture"].isString() || ! (g == "tap" || g == "begin" || g == "move" || g == "end"))
            {
                result.error = "unknown gesture " + g;
                current();
                return;
            }
            gesture = g == "begin" ? Gesture::begin : (g == "move" ? Gesture::move : (g == "end" ? Gesture::end : Gesture::tap));
        }

        double target = 0.0;
        if (hasValue)
        {
            const auto& v = cmd["value"];
            if (! (v.isDouble() || v.isInt() || v.isInt64()) || ! std::isfinite ((double) v))
            {
                result.error = "value is not a number";
                current();
                return;
            }
            target = juce::jlimit (0.0, 1.0, (double) v);
        }
        else
        {
            if (! cmd["text"].isString())
            {
                result.error = "text is not a string";
                current();
                return;
            }
            const auto words = cmd["text"].toString().trim();
            if (words.equalsIgnoreCase ("default"))
            {
                target = p.getDefaultValue();
            }
            else if (! entry.choices.isEmpty())
            {
                int choice = -1;
                for (int k = 0; k < entry.choices.size() && choice < 0; ++k)
                    if (entry.choices[k].trim().equalsIgnoreCase (words))
                        choice = k;
                if (choice < 0)
                {
                    result.error = "no choice named " + words;
                    current();
                    return;
                }
                target = entry.choices.size() > 1 ? (double) choice / (double) (entry.choices.size() - 1) : 0.0;
            }
            else
            {
                const double v = p.getValueForText (words);
                if (! std::isfinite (v))
                {
                    result.error = "text not understood: " + words;
                    current();
                    return;
                }
                target = juce::jlimit (0.0, 1.0, v);
            }
        }

        setWithGesture (index, (float) target, gesture, t);
        result.ok = true;
        current();
    }

    // Exactly what a mouse drag in the plug-in's own window does, on the message thread.
    void setWithGesture (int index, float v, Gesture g, double t)
    {
        auto& p = *entries[(size_t) index].param;
        const auto open = openGestures.find (index);
        const bool isOpen = open != openGestures.end();
        if (! isOpen)
            p.beginChangeGesture();
        p.setValueNotifyingHost (v);
        if (g == Gesture::tap || g == Gesture::end)
        {
            p.endChangeGesture();
            if (isOpen)
                openGestures.erase (open);
        }
        else
        {
            openGestures[index] = t;
        }
    }

    void endIdleGestures (double t)
    {
        for (auto it = openGestures.begin(); it != openGestures.end();)
        {
            if (t < it->second)
                it->second = t;
            if (t - it->second >= linkdetail::gestureTimeout)
            {
                if (juce::isPositiveAndBelow (it->first, (int) entries.size()))
                    entries[(size_t) it->first].param->endChangeGesture();
                it = openGestures.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    juce::AudioProcessor& processor;
    Info info;
    const juce::File dir;
    const bool on;
    juce::String instanceId;
    juce::File cardFile, inboxDir;
    double startedUnix = 0.0;

    std::vector<Entry> entries;
    std::map<juce::String, int> byId;
    std::vector<float> written;
    std::deque<Done> done;
    std::map<int, double> openGestures;

    std::atomic<bool> dirty { true }, trackDirty { false };
    std::mutex trackLock;
    juce::String pendingTrack;

    juce::int64 seq = 0;
    double lastWrite = 0.0, lastTouch = 0.0, lastScan = 0.0;
    bool writeFailed = false;

    JUCE_DECLARE_NON_COPYABLE (Link)
};
} // namespace smooth
