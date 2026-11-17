#pragma once
// hub-<id>.json: the presence file of one Switchboard instance, from PROTOCOL.md:
//   { "link": 1, "hub": true, "id": "...", "track": "Lead Vox" | null, "version": "1.0.0" }
// Same folder, atomic write and mtime heartbeat as a member card, and the same rule for when it is on (AudioUnit always, anything
// else only with SMOOTH_LINK_DIR). Written at construction and again when the track name changes, touched twice a second from the
// message thread, removed on destruction.
#include "SmoothLink.h"

namespace smooth
{
class HubFile : private juce::Timer
{
public:
    HubFile (juce::AudioProcessor::WrapperType wrapper, juce::String versionString)
        : dir (Link::folder()), on (Link::enabledFor (wrapper) && dir != juce::File()), version (std::move (versionString))
    {
        instanceId = linkdetail::randomHex (8);
        if (! on)
            return;
        linkdetail::makeFolders (dir);
        for (int tries = 0; tries < 8 && dir.getChildFile ("hub-" + instanceId + ".json").exists(); ++tries)
            instanceId = linkdetail::randomHex (8);
        hubFile = dir.getChildFile ("hub-" + instanceId + ".json");
        write (now());
        startTimerHz (4);
    }

    ~HubFile() override
    {
        stopTimer();
        if (on)
            unlink (hubFile.getFullPathName().toRawUTF8());
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

    // A rewrite when the name changed, else the heartbeat when due. The timer calls this; tests call it by hand.
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

    std::function<double()> clock { [] { return juce::Time::getMillisecondCounterHiRes() / 1000.0; } };

private:
    double now() const { return clock ? clock() : juce::Time::getMillisecondCounterHiRes() / 1000.0; }
    void timerCallback() override { pollNow(); }

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
    juce::File hubFile;
    mutable std::mutex trackLock;
    juce::String track;
    std::atomic<bool> trackDirty { false };
    bool writeFailed = false;
    double lastTouch = 0.0;

    JUCE_DECLARE_NON_COPYABLE (HubFile)
};
} // namespace smooth
