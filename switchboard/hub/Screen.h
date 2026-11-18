#pragma once
// What Switchboard's window shows (PROTOCOL.md, "Switchboard's screen and its inbox"): Tracks (the list of tracks), one track's
// plug-ins, or every control of one plug-in. The processor keeps it, not the window, so closing and reopening the window or reloading
// the project comes back to it. Saved with the session; the hub file says it as "showing".
#include <juce_core/juce_core.h>

namespace hub
{
struct Screen
{
    enum class Kind { tracks, track, controls };

    Kind kind = Kind::tracks;
    juce::String track;    // track and controls: the track, "" for the plug-ins Logic has not named a track for
    juce::String plugin;   // controls: the plug-in as Switchboard numbers copies ("Tune Pro", "Tune Pro 2")

    static Screen tracks() { return {}; }
    static Screen onTrack (const juce::String& t) { return { Kind::track, t, {} }; }
    static Screen controlsOf (const juce::String& t, const juce::String& p) { return { Kind::controls, t, p }; }

    bool operator== (const Screen& o) const
    {
        if (kind != o.kind)
            return false;
        if (kind == Kind::tracks)
            return true;
        return track == o.track && (kind == Kind::track || plugin == o.plugin);
    }
    bool operator!= (const Screen& o) const { return ! operator== (o); }

    // "tracks", "track" or "controls", as the hub file and the session say it.
    juce::String name() const
    {
        switch (kind)
        {
            case Kind::track:    return "track";
            case Kind::controls: return "controls";
            case Kind::tracks:   break;
        }
        return "tracks";
    }

    // The "showing" object of the hub file: { "screen": "track", "track": "Lead Vox" }. The track with no name is null.
    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        juce::var v (o);
        o->setProperty ("screen", name());
        if (kind != Kind::tracks)
            o->setProperty ("track", track.isEmpty() ? juce::var() : juce::var (track));
        if (kind == Kind::controls)
            o->setProperty ("plugin", plugin);
        return v;
    }

    // For logs and test messages: tracks, track:Lead Vox, controls:Lead Vox/Tune Pro ("(no name)" for the track with none).
    juce::String describe() const
    {
        const auto t = track.isEmpty() ? juce::String ("(no name)") : track;
        switch (kind)
        {
            case Kind::track:    return "track:" + t;
            case Kind::controls: return "controls:" + t + "/" + plugin;
            case Kind::tracks:   break;
        }
        return "tracks";
    }

    // Longest names a saved session or a command may carry. Longer ones are refused, never cut.
    static constexpr int maxTrackChars = 1024, maxPluginChars = 256;

    // Saved with the session as attributes of the state's root element: screen, and track and plugin where they apply.
    void save (juce::XmlElement& xml) const
    {
        xml.setAttribute ("screen", name());
        if (kind != Kind::tracks)
            xml.setAttribute ("track", track);
        if (kind == Kind::controls)
            xml.setAttribute ("plugin", plugin);
    }

    // From a saved session: 1.1.0's screen attributes, else 1.0.0's view. False (out untouched) when the element holds neither in a
    // form Switchboard knows. 1.0.0 saved "this" (This track, the default) or "all" (All tracks), which both open on Tracks now, or
    // "track:<name>" (one chosen track), which opens that track's screen ("track:" alone was the plug-ins with no track name).
    static bool load (const juce::XmlElement& xml, Screen& out)
    {
        if (xml.hasAttribute ("screen"))
        {
            const auto s = xml.getStringAttribute ("screen");
            const auto t = xml.getStringAttribute ("track");
            const auto p = xml.getStringAttribute ("plugin");
            if (s == "tracks")
            {
                out = tracks();
                return true;
            }
            if (s == "track" && xml.hasAttribute ("track") && t.length() <= maxTrackChars)
            {
                out = onTrack (t);
                return true;
            }
            if (s == "controls" && xml.hasAttribute ("track") && t.length() <= maxTrackChars && p.isNotEmpty() && p.length() <= maxPluginChars)
            {
                out = controlsOf (t, p);
                return true;
            }
            return false;
        }
        if (xml.hasAttribute ("view"))
        {
            const auto v = xml.getStringAttribute ("view");
            if (v == "this" || v == "all")
            {
                out = tracks();
                return true;
            }
            if (v.startsWith ("track:") && v.length() - 6 <= maxTrackChars)
            {
                out = onTrack (v.substring (6));
                return true;
            }
        }
        return false;
    }
};
} // namespace hub
