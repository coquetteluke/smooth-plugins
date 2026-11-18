#include "PluginProcessor.h"
#include "Editor.h"

SwitchboardProcessor::SwitchboardProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      hub (wrapperType, SWITCHBOARD_VERSION, hub::Screen::tracks().toVar()),
      reg (smooth::Link::folder())
{
    if (hub.enabled())
        startTimerHz (pollHz);
}

SwitchboardProcessor::~SwitchboardProcessor()
{
    stopTimer();
}

// Any speaker layout, as long as the input is the same layout as the output: mono, stereo, LCR, quad, 5.1, 7.1, 7.1.2, Dolby Atmos
// 7.1.4 and so on, so it sits on a track, Stereo Out or a surround master. JUCE has a named layout for every count from 1 to 16, so it
// reports AU channel info {-1, -1} (any count in, the same count out). Refused: disabled, mismatched (even with the same count, 5.1 in
// and 6.0 out), and plain discrete channels with no speaker names. Logic never uses those for a channel strip, and accepting them makes
// JUCE's AU wrapper stop reporting channel layouts at all (busIgnoresLayout), which 1.0.0 did and Logic reads for surround.
bool SwitchboardProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    if (l.inputBuses.size() != 1 || l.outputBuses.size() != 1)
        return false;
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return ! out.isDisabled() && in == out && ! out.isDiscreteLayout();
}

void SwitchboardProcessor::prepareToPlay (double, int)
{
    setLatencySamples (0);
}

void SwitchboardProcessor::passThrough (juce::AudioBuffer<float>& buffer) noexcept
{
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
}

void SwitchboardProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    passThrough (buffer);
}

void SwitchboardProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    passThrough (buffer);
}

juce::AudioProcessorEditor* SwitchboardProcessor::createEditor()
{
    return new SwitchboardEditor (*this);
}

// ---- the screen ----

hub::Screen SwitchboardProcessor::getScreen() const
{
    const std::lock_guard<std::mutex> lock (screenLock);
    return screen;
}

void SwitchboardProcessor::setScreen (const hub::Screen& s)
{
    missingSince = -1.0;
    missingLooks = 0;
    change (s);
}

void SwitchboardProcessor::change (const hub::Screen& s)
{
    if (s == getScreen())
    {
        keep (s);
        return;
    }
    {
        const std::lock_guard<std::mutex> lock (screenLock);
        screen = s;
    }
    ++changes;
    keep (s);
    hub.setShowing (s.toVar());
}

void SwitchboardProcessor::keep (const hub::Screen& s)
{
    keptIds.clear();
    if (s.kind == hub::Screen::Kind::track)
    {
        for (auto* i : reg.onTrack (s.track))
            keptIds.push_back (i->id);
    }
    else if (s.kind == hub::Screen::Kind::controls)
    {
        if (auto* i = reg.find (s.track, s.plugin))
            keptIds.push_back (i->id);
    }
}

void SwitchboardProcessor::catchUp (double t)
{
    if (settleFrom < 0.0)
        settleFrom = t;
    if (! restored.exchange (false))
        return;
    // A session was loaded: what it remembers may not have opened yet, so the settling time starts again, and names, not ids, find it.
    keptIds.clear();
    settleFrom = t;
    missingSince = -1.0;
    missingLooks = 0;
    hub.setShowing (getScreen().toVar());
}

void SwitchboardProcessor::refresh (double t)
{
    if (! hub.enabled())
        return;
    catchUp (t);
    lastLook = t;
    reg.scan (t);
    follow (t);
}

void SwitchboardProcessor::follow (double t)
{
    const auto s = getScreen();
    if (s.kind == hub::Screen::Kind::tracks)
    {
        keptIds.clear();
        missingSince = -1.0;
        missingLooks = 0;
        return;
    }

    bool present = false;
    hub::Screen next = s;
    if (s.kind == hub::Screen::Kind::track)
    {
        present = ! reg.onTrack (s.track).empty();
        if (! present)
        {
            // Renamed: every plug-in it showed that is still open is now on one other track.
            juce::String moved;
            int alive = 0;
            bool oneTrack = true;
            for (const auto& id : keptIds)
                if (auto* i = reg.find (id))
                {
                    if (alive++ == 0)
                        moved = i->track;
                    else if (i->track != moved)
                        oneTrack = false;
                }
            if (alive > 0 && oneTrack)
            {
                present = true;
                next = hub::Screen::onTrack (moved);
            }
        }
    }
    else
    {
        // The plug-in it showed, by id once known: its track may be renamed, or it may become "Tune Pro" when an older copy closes.
        const hub::Instance* i = keptIds.size() == 1 ? reg.find (keptIds.front()) : reg.find (s.track, s.plugin);
        if (i != nullptr)
        {
            present = true;
            next = hub::Screen::controlsOf (i->track, i->name);
        }
    }

    if (present)
    {
        missingSince = -1.0;
        missingLooks = 0;
        change (next);
        return;
    }

    // Missing. Only gone when it stays missing: one look can miss every card when Logic held its main thread for a moment, and
    // just after a project loads the plug-ins it remembers may still be opening.
    if (missingSince < 0.0 || t < missingSince)
    {
        missingSince = t;
        missingLooks = 0;
    }
    ++missingLooks;
    if (t - settleFrom >= settleAfter && t - missingSince >= goneAfter && missingLooks >= 3)
    {
        missingSince = -1.0;
        missingLooks = 0;
        if (s.kind == hub::Screen::Kind::controls && ! reg.onTrack (s.track).empty())
            change (hub::Screen::onTrack (s.track));
        else
            change (hub::Screen::tracks());
    }
}

namespace
{
bool hasKey (const juce::var& v, const char* key) { return v.getDynamicObject() != nullptr && v.getDynamicObject()->hasProperty (key); }

// For error messages: a name as the writer sent it, cut to a readable length.
juce::String quoted (const juce::String& s) { return "\"" + (s.length() > 80 ? s.substring (0, 80) + "..." : s) + "\""; }

juce::String trackWords (const juce::String& track) { return track.isEmpty() ? juce::String ("the plug-ins with no track name") : quoted (track); }
} // namespace

smooth::HubFile::Done SwitchboardProcessor::apply (const juce::String& text)
{
    smooth::HubFile::Done d;
    const auto fail = [&] (const juce::String& error)
    {
        d.ok = false;
        d.error = error;
        d.showing = getScreen().toVar();
        return d;
    };
    juce::var cmd;
    if (text.trim().isEmpty() || juce::JSON::parse (text, cmd).failed())
        return fail ("not valid JSON");
    if (! cmd.isObject())
        return fail ("not a JSON object");
    if (! hasKey (cmd, "show"))
        return fail ("no show (tracks, track or controls)");
    if (! cmd["show"].isString())
        return fail ("show is not a string (tracks, track or controls)");
    const auto show = cmd["show"].toString();

    hub::Screen wanted;
    if (show == "track" || show == "controls")
    {
        if (! hasKey (cmd, "track"))
            return fail ("no track");
        const auto& tv = cmd["track"];
        if (! tv.isString() && ! tv.isVoid())
            return fail ("track is not a string");
        const juce::String asked = tv.isString() ? tv.toString() : juce::String();   // null: the plug-ins with no track name
        // The exact name, else the one track that matches ignoring case.
        juce::String track;
        bool found = false;
        juce::StringArray loose;
        for (const auto& t : reg.tracks())
        {
            if (t == asked)
            {
                track = t;
                found = true;
            }
            else if (t.equalsIgnoreCase (asked))
                loose.add (t);
        }
        if (! found && loose.size() == 1)
        {
            track = loose[0];
            found = true;
        }
        if (! found)
            return fail (asked.isEmpty() ? juce::String ("Switchboard does not see any plug-in without a track name right now")
                                         : "Switchboard does not see a track named " + quoted (asked) + " right now");
        if (show == "track")
            wanted = hub::Screen::onTrack (track);
        else
        {
            if (! hasKey (cmd, "plugin"))
                return fail ("no plugin");
            if (! cmd["plugin"].isString())
                return fail ("plugin is not a string");
            const auto askedPlugin = cmd["plugin"].toString();
            const hub::Instance* inst = reg.find (track, askedPlugin);
            if (inst == nullptr)
            {
                std::vector<const hub::Instance*> matches;
                for (auto* i : reg.onTrack (track))
                    if (i->name.equalsIgnoreCase (askedPlugin))
                        matches.push_back (i);
                if (matches.size() == 1)
                    inst = matches.front();
            }
            if (inst == nullptr)
                return fail ("Switchboard does not see " + quoted (askedPlugin) + " on " + trackWords (track) + " right now");
            wanted = hub::Screen::controlsOf (track, inst->name);
        }
    }
    else if (show != "tracks")
    {
        return fail ("unknown show " + quoted (show) + " (tracks, track or controls)");
    }

    missingSince = -1.0;
    missingLooks = 0;
    change (wanted);
    d.ok = true;
    d.showing = wanted.toVar();
    return d;
}

void SwitchboardProcessor::pollNow (double t)
{
    if (! hub.enabled())
        return;
    ++polls;
    catchUp (t);
    bool looked = false;
    hub.takeCommands ([&] (const juce::String&, const juce::String& text)
    {
        if (! looked)
        {
            refresh (t);   // a command is checked against what Switchboard sees now
            looked = true;
        }
        return apply (text);
    });
    if (! looked && getActiveEditor() == nullptr && getScreen().kind != hub::Screen::Kind::tracks
        && (t - lastLook >= closedLookEvery || t < lastLook))
        refresh (t);
    hub.pollNow();
}

// ---- state: a version, so a later Switchboard can tell what saved the session, and the screen ----

void SwitchboardProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("SWITCHBOARD");
    xml.setAttribute ("version", SWITCHBOARD_VERSION);
    getScreen().save (xml);
    copyXmlToBinary (xml, dest);
}

void SwitchboardProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName ("SWITCHBOARD"))
        {
            hub::Screen s;
            if (hub::Screen::load (*xml, s))
            {
                {
                    const std::lock_guard<std::mutex> lock (screenLock);
                    screen = s;
                }
                ++changes;
                restored.store (true);
            }
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SwitchboardProcessor(); }
