#pragma once
// Commands: Switchboard's writer of set commands (PROTOCOL.md, "Inbox <id>.in/ and commands"). Message thread only.
//
// A command is written whole to <id>.in/.<name>.tmp and renamed to <id>.in/<name>.json, so a member never reads part of one.
// <name> is <unix ms>-switchboard-<6 random hex>. The ms part goes up by at least one per command, so the order a member applies
// commands in (name order) is always the order they were sent, even for several in one millisecond.
// A command is confirmed when the plug-in's card lists it in "done". A member applies its inbox in name order, so a confirmed command
// also confirms every earlier one of ours to that plug-in (one poll can apply more commands than "done" keeps). No answer in 3 s is
// a failure: the plug-in did not answer, and the writer takes its command file back.
//
// DragThrottle turns a mouse drag into begin, moves and end, at most 30 commands in any second.
#include "Registry.h"
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace hub
{
enum class Gesture { tap, begin, move, end };

inline const char* gestureName (Gesture g)
{
    switch (g)
    {
        case Gesture::begin: return "begin";
        case Gesture::move:  return "move";
        case Gesture::end:   return "end";
        case Gesture::tap:   break;
    }
    return "tap";
}

// What became of one command.
struct Outcome
{
    juce::String name, instance, param, error;
    bool answered = false;   // the card's done list holds it (or a later command of ours to the same plug-in)
    bool ok = false;         // applied
};

class Commands
{
public:
    static constexpr double answerWithin = 3.0;

    explicit Commands (juce::File folderToWrite) : dir (std::move (folderToWrite)), path (dir.getFullPathName().toStdString()) {}

    // A normalized value, clamped to 0..1. Returns the command's name, or empty when the plug-in has no inbox (it closed) or the
    // write failed.
    juce::String setValue (const juce::String& instance, const juce::String& param, double value, Gesture g, double now)
    {
        auto* o = new juce::DynamicObject();
        juce::var cmd (o);
        o->setProperty ("param", param);
        o->setProperty ("value", juce::jlimit (0.0, 1.0, std::isfinite (value) ? value : 0.0));
        o->setProperty ("gesture", gestureName (g));
        o->setProperty ("from", "switchboard");
        return write (instance, param, cmd, now);
    }

    // Text through the plug-in's own reading of it: a choice name, "6 dB", or "default" for the control's default.
    juce::String setText (const juce::String& instance, const juce::String& param, const juce::String& text, Gesture g, double now)
    {
        auto* o = new juce::DynamicObject();
        juce::var cmd (o);
        o->setProperty ("param", param);
        o->setProperty ("text", text);
        o->setProperty ("gesture", gestureName (g));
        o->setProperty ("from", "switchboard");
        return write (instance, param, cmd, now);
    }

    // Checks the waiting commands against the cards just scanned. Returns those that were answered or failed, in sending order.
    std::vector<Outcome> settle (const Registry& registry, double now)
    {
        std::vector<Outcome> out;
        if (waiting.empty())
            return out;
        std::map<juce::String, juce::String> newest;   // instance -> newest of our names it answered
        std::vector<bool> finished (waiting.size(), false);
        for (size_t k = 0; k < waiting.size(); ++k)
            if (auto* inst = registry.find (waiting[k].instance))
                if (auto* d = inst->doneFor (waiting[k].name))
                {
                    out.push_back ({ waiting[k].name, waiting[k].instance, waiting[k].param, d->error, true, d->ok });
                    finished[k] = true;
                    auto& n = newest[waiting[k].instance];
                    if (waiting[k].name.compare (n) > 0)
                        n = waiting[k].name;
                }
        for (size_t k = 0; k < waiting.size(); ++k)
        {
            if (finished[k])
                continue;
            const auto& w = waiting[k];
            const auto it = newest.find (w.instance);
            if (it != newest.end() && w.name.compare (it->second) < 0)
            {
                out.push_back ({ w.name, w.instance, w.param, {}, true, true });
                finished[k] = true;
            }
            else if (now - w.sentAt >= answerWithin || now < w.sentAt - 60.0)
            {
                unlink (w.file.c_str());   // ours; a member that reads it at this moment sees it gone and skips it
                out.push_back ({ w.name, w.instance, w.param, "the plug-in did not answer", false, false });
                finished[k] = true;
            }
        }
        std::vector<Waiting> left;
        for (size_t k = 0; k < waiting.size(); ++k)
            if (! finished[k])
                left.push_back (std::move (waiting[k]));
        waiting = std::move (left);
        std::sort (out.begin(), out.end(), [] (const Outcome& a, const Outcome& b) { return a.name.compare (b.name) < 0; });
        return out;
    }

    int pending() const { return (int) waiting.size(); }

private:
    struct Waiting
    {
        juce::String name, instance, param;
        std::string file;
        double sentAt = 0.0;
    };

    static bool isId (const juce::String& s) { return s.length() == 16 && s.containsOnly ("0123456789abcdef"); }

    juce::String write (const juce::String& instance, const juce::String& param, const juce::var& cmd, double now)
    {
        if (! isId (instance))
            return {};
        const auto inbox = path + "/" + instance.toStdString() + ".in";
        struct stat st;
        if (stat (inbox.c_str(), &st) != 0 || ! S_ISDIR (st.st_mode))
            return {};   // never made by a writer: a missing inbox means the plug-in is gone
        juce::int64 ms = (juce::int64) std::floor (now * 1000.0);
        if (ms <= lastMs)
            ms = lastMs + 1;
        const auto name = juce::String (ms) + "-switchboard-" + smooth::linkdetail::randomHex (3);
        const auto temp = inbox + "/." + name.toStdString() + ".tmp";
        const auto final = inbox + "/" + name.toStdString() + ".json";
        const auto text = juce::JSON::toString (cmd, juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none)) + "\n";

        const int fd = open (temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0)
            return {};
        const char* bytes = text.toRawUTF8();
        size_t left = std::strlen (bytes);
        bool ok = true;
        while (ok && left > 0)
        {
            const ssize_t n = ::write (fd, bytes, left);
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
        if (! ok || rename (temp.c_str(), final.c_str()) != 0)
        {
            unlink (temp.c_str());
            return {};
        }
        lastMs = ms;
        waiting.push_back ({ name, instance, param, final, now });
        return name;
    }

    const juce::File dir;
    const std::string path;
    std::vector<Waiting> waiting;
    juce::int64 lastMs = 0;
};

// A drag as commands: begin with the first move, moves while it goes on, end with the last value on release. At most one command
// every 34 ms, so never more than 30 in any second; values in between are dropped, the latest is kept. A press and release with no
// move sends nothing. The sink writes one command and returns its name.
class DragThrottle
{
public:
    static constexpr double minGap = 0.034;
    using Sink = std::function<juce::String (double value, Gesture)>;

    explicit DragThrottle (Sink s) : sink (std::move (s)) {}

    void press (double value, double)
    {
        pressed = true;
        begun = ending = pending = false;
        latest = value;
    }

    void drag (double value, double now)
    {
        if (! pressed)
            return;
        if (! juce::exactlyEqual (value, latest))
        {
            latest = value;
            pending = true;
        }
        flush (now);
    }

    void release (double now)
    {
        if (! pressed)
            return;
        pressed = false;
        if (begun || pending)
        {
            ending = true;
            flush (now);
        }
    }

    // From a timer: sends what waited for its turn.
    void tick (double now) { flush (now); }

    // Something is still to be sent (a value that waited for its turn, or the end).
    bool busy() const { return ending || (pressed && pending); }
    // Pressed, or still sending the end.
    bool active() const { return pressed || ending; }
    double value() const { return latest; }
    juce::String lastName() const { return last; }

private:
    void flush (double now)
    {
        if (! pending && ! ending)
            return;
        if (sentAny && now - lastSent < minGap && now >= lastSent)
            return;
        Gesture g;
        if (! begun)
            g = ending ? Gesture::tap : Gesture::begin;
        else
            g = ending ? Gesture::end : Gesture::move;
        last = sink (latest, g);
        lastSent = now;
        sentAny = true;
        pending = false;
        if (g == Gesture::begin)
            begun = true;
        if (g == Gesture::tap || g == Gesture::end)
            begun = ending = false;
    }

    Sink sink;
    bool pressed = false, begun = false, ending = false, pending = false, sentAny = false;
    double latest = 0.0, lastSent = 0.0;
    juce::String last;
};
} // namespace hub
