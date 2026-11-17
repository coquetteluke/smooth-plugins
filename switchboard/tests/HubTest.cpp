// Offline tests for the Switchboard AU (hub/). Run: ctest --test-dir build --output-on-failure, or by hand with both scratch
// variables set: SMOOTH_LINK_DIR=$PWD/work/scratch/hubtest SMOOTH_KEY_JSON=$PWD/work/scratch/key.json build/HubTest_artefacts/Release/HubTest
//
// Two more modes, never run by ctest:
//   SWITCHBOARD_RENDER_DIR=<absolute folder> HubTest  saves the painted screens of window_render (2x) there:
//                                                    switchboard-1.1-tracks.png, -track.png, -controls.png, the smallest size, empty.
//   HubTest --window-cpu <seconds> <LinkStub path>    opens the window on screen with 6 LinkStub plug-ins in the scratch folder and
//                                                    prints the CPU this process used with the window closed, open on Tracks, open
//                                                    on a track, and with a control moving (one core = 100%).
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "TestProcessor.h"
#include "PluginProcessor.h"
#include "Editor.h"
#include "Registry.h"
#include "Commands.h"
#include "Screen.h"
#include <cstring>
#include <pwd.h>
#include <random>
#include <regex>
#include <set>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

using namespace testkit;

namespace
{
juce::File base;

juce::File useFolder (const char* name)
{
    auto f = base.getChildFile (name);
    f.deleteRecursively();
    setenv ("SMOOTH_LINK_DIR", f.getFullPathName().toRawUTF8(), 1);
    return f;
}

juce::var readJson (const juce::File& f)
{
    std::string bytes;
    juce::var v;
    if (! smooth::linkdetail::readFile (f.getFullPathName().toStdString(), bytes)
        || juce::JSON::parse (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), v).failed())
        return {};
    return v;
}

bool has (const juce::var& v, const char* key) { return v.getDynamicObject() != nullptr && v.getDynamicObject()->hasProperty (key); }

long long mtimeNs (const juce::File& f)
{
    struct stat s;
    if (stat (f.getFullPathName().toRawUTF8(), &s) != 0)
        return -1;
    return (long long) s.st_mtimespec.tv_sec * 1000000000LL + s.st_mtimespec.tv_nsec;
}

struct Checks
{
    bool ok = true;
    void need (bool c, const juce::String& what)
    {
        if (! c)
        {
            ok = false;
            std::printf ("  not true: %s\n", what.toRawUTF8());
        }
    }
};

// Every bit pattern a float can hold, NaN payloads, infinities, denormals and negative zero among them.
void fillBits (juce::AudioBuffer<float>& b, unsigned seed)
{
    std::mt19937 g (seed);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto* x = b.getWritePointer (ch);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const juce::uint32 bits = g();
            std::memcpy (x + i, &bits, sizeof bits);
        }
        const juce::uint32 specials[] = { 0x7fc00000u, 0x7fa00001u, 0xffc00123u, 0x7f800000u, 0xff800000u, 0x80000000u, 0x00000001u, 0x807fffffu, 0x7f7fffffu };
        for (int k = 0; k < 9 && k < b.getNumSamples(); ++k)
            std::memcpy (x + k * 7 % b.getNumSamples(), specials + k, sizeof (juce::uint32));
    }
}

// Runs first and with the hub file off, so no timer thread exists yet: the allocation counter counts every thread, and the
// timer thread JUCE starts for the hub's heartbeat allocates as it starts, at moments that can fall inside a processBlock call.
void testPassthrough()
{
    unsetenv ("SMOOTH_LINK_DIR");
    Checks c;
    long allocations = 0;
    for (int channels : { 1, 2 })
        for (int block : { 32, 512, 1013 })
            for (bool bypassed : { false, true })
            {
                SwitchboardProcessor p;
                prepare (p, channels, 48000.0, block);
                juce::AudioBuffer<float> buf (channels, block), copy (channels, block);
                juce::MidiBuffer midi;
                bool same = true;
                for (int b = 0; b < 200; ++b)
                {
                    fillBits (buf, (unsigned) (b * 31 + block + channels));
                    for (int ch = 0; ch < channels; ++ch)
                        copy.copyFrom (ch, 0, buf, ch, 0, block);
                    const auto before = allocs.load();
                    if (bypassed)
                        p.processBlockBypassed (buf, midi);
                    else
                        p.processBlock (buf, midi);
                    allocations += allocs.load() - before;
                    for (int ch = 0; ch < channels; ++ch)
                        same = same && std::memcmp (buf.getReadPointer (ch), copy.getReadPointer (ch), sizeof (float) * (size_t) block) == 0;
                }
                c.need (same, juce::String (channels) + " ch, block " + juce::String (block) + (bypassed ? ", bypassed" : "") + ": output bits equal input bits");
            }
    c.need (allocations == 0, "no allocation in processBlock (counted " + juce::String (allocations) + ")");
    check (c.ok, "passthrough_bit_exact: mono and stereo, blocks 32, 512 and 1013, bypassed or not, every bit pattern");
    setenv ("SMOOTH_LINK_DIR", base.getFullPathName().toRawUTF8(), 1);
}

// The layouts a Logic channel strip or master can have, up to Dolby Atmos 7.1.4 and the largest JUCE knows (9.1.6, 16 channels).
std::vector<std::pair<const char*, juce::AudioChannelSet>> everyLayout()
{
    using S = juce::AudioChannelSet;
    return { { "mono", S::mono() }, { "stereo", S::stereo() }, { "LCR", S::createLCR() }, { "quad", S::quadraphonic() },
             { "5.0", S::create5point0() }, { "5.1", S::create5point1() }, { "6.1", S::create6point1() }, { "7.1", S::create7point1() },
             { "7.1.2", S::create7point1point2() }, { "7.1.4", S::create7point1point4() }, { "9.1.6", S::create9point1point6() },
             { "ambisonic 1st order", S::ambisonic (1) } };
}

// Like testPassthrough, with the hub file off and before any timer thread exists.
void testPassthroughAnyLayout()
{
    unsetenv ("SMOOTH_LINK_DIR");
    Checks c;
    long allocations = 0;
    for (const auto& [label, set] : everyLayout())
    {
        SwitchboardProcessor p;
        auto layout = p.getBusesLayout();
        layout.inputBuses.getReference (0) = set;
        layout.outputBuses.getReference (0) = set;
        c.need (p.checkBusesLayoutSupported (layout), juce::String (label) + ": offered");
        if (! p.setBusesLayout (layout))
        {
            c.need (false, juce::String (label) + ": the layout can be set");
            continue;
        }
        const int channels = set.size();
        c.need (p.getTotalNumInputChannels() == channels && p.getTotalNumOutputChannels() == channels, juce::String (label) + ": "
                + juce::String (channels) + " channels in and out");
        for (int block : { 1, 13, 511, 1013 })
            for (bool bypassed : { false, true })
            {
                p.setRateAndBufferSizeDetails (48000.0, block);
                p.prepareToPlay (48000.0, block);
                c.need (p.getLatencySamples() == 0, juce::String (label) + ": no latency");
                juce::AudioBuffer<float> buf (channels, block), copy (channels, block);
                juce::MidiBuffer midi;
                bool same = true;
                for (int b = 0; b < 40; ++b)
                {
                    fillBits (buf, (unsigned) (b * 17 + block * 3 + channels));
                    for (int ch = 0; ch < channels; ++ch)
                        copy.copyFrom (ch, 0, buf, ch, 0, block);
                    const auto before = allocs.load();
                    if (bypassed)
                        p.processBlockBypassed (buf, midi);
                    else
                        p.processBlock (buf, midi);
                    allocations += allocs.load() - before;
                    for (int ch = 0; ch < channels; ++ch)
                        same = same && std::memcmp (buf.getReadPointer (ch), copy.getReadPointer (ch), sizeof (float) * (size_t) block) == 0;
                }
                c.need (same, juce::String (label) + ", block " + juce::String (block) + (bypassed ? ", bypassed" : "") + ": output bits equal input bits");
            }
    }
    c.need (allocations == 0, "no allocation in processBlock (counted " + juce::String (allocations) + ")");
    check (c.ok, "passthrough_bit_exact_any_layout: mono, stereo, LCR, quad, 5.0, 5.1, 6.1, 7.1, 7.1.2, 7.1.4, 9.1.6 and ambisonic, "
                 "blocks 1, 13, 511 and 1013, bypassed or not, zero latency");
    setenv ("SMOOTH_LINK_DIR", base.getFullPathName().toRawUTF8(), 1);
}

void testMismatchedLayoutRefused()
{
    unsetenv ("SMOOTH_LINK_DIR");
    using S = juce::AudioChannelSet;
    const std::vector<std::tuple<const char*, S, S>> bad {
        { "disabled in and out", S::disabled(), S::disabled() }, { "stereo in, disabled out", S::stereo(), S::disabled() },
        { "disabled in, stereo out", S::disabled(), S::stereo() }, { "mono in, stereo out", S::mono(), S::stereo() },
        { "stereo in, mono out", S::stereo(), S::mono() }, { "5.1 in, stereo out", S::create5point1(), S::stereo() },
        { "5.1 in, 6.0 out (same count, other layout)", S::create5point1(), S::create6point0() },
        { "7.1.4 in, 7.1.2 out", S::create7point1point4(), S::create7point1point2() },
        { "stereo in, 7.1.4 out", S::stereo(), S::create7point1point4() },
        // No speaker names: never a Logic channel strip, and accepting it would make JUCE's AU wrapper drop the layout tags it reports.
        { "3 discrete channels in and out", S::discreteChannels (3), S::discreteChannels (3) },
        { "2 discrete channels in and out", S::discreteChannels (2), S::discreteChannels (2) } };
    Checks c;
    for (const auto& [label, in, out] : bad)
    {
        SwitchboardProcessor p;
        const auto before = p.getBusesLayout();
        auto layout = before;
        layout.inputBuses.getReference (0) = in;
        layout.outputBuses.getReference (0) = out;
        c.need (! p.checkBusesLayoutSupported (layout), juce::String (label) + ": refused");
        c.need (! p.setBusesLayout (layout) && p.getBusesLayout() == before, juce::String (label) + ": cannot be set, the layout stays");
    }
    check (c.ok, "mismatched_layout_refused: disabled, mismatched, same-count other layouts and plain discrete channels are refused");
    setenv ("SMOOTH_LINK_DIR", base.getFullPathName().toRawUTF8(), 1);
}

void testLatencyZero()
{
    Checks c;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        SwitchboardProcessor p;
        prepare (p, 2, sr, 512);
        c.need (p.getLatencySamples() == 0, "latency 0 at " + juce::String (sr));
        c.need (p.getTailLengthSeconds() == 0.0, "tail 0 at " + juce::String (sr));
    }
    check (c.ok, "latency_zero: no latency and no tail at 44.1, 48 and 96 kHz");
}

void testNoParameters()
{
    SwitchboardProcessor p;
    Checks c;
    c.need (p.getParameters().isEmpty(), "getParameters() is empty");
    c.need (p.getBypassParameter() == nullptr, "no bypass parameter of its own");
    c.need (! p.acceptsMidi() && ! p.producesMidi(), "no MIDI");
    check (c.ok, "no_parameters: Switchboard has no AU parameters");
}

juce::String realHome()
{
    struct passwd entry;
    struct passwd* result = nullptr;
    char buffer[4096];
    if (getpwuid_r (getuid(), &entry, buffer, sizeof buffer, &result) != 0 || result == nullptr)
        return {};
    return result->pw_dir;
}

void testHubFileTrack()
{
    const auto dir = useFolder ("hub_file_track");
    Checks c;
    juce::File file;
    {
        SwitchboardProcessor p;
        file = p.hub.file();
        auto v = readJson (file);
        c.need (p.hub.enabled() && file.existsAsFile() && file.getParentDirectory() == dir, "hub file written at construction, in the folder");
        c.need (file.getFileName() == "hub-" + p.hub.id() + ".json" && p.hub.id().length() == 16 && p.hub.id().containsOnly ("0123456789abcdef"), "named hub-<16 hex id>.json");
        c.need (v.isObject() && (int) v["link"] == 1 && v["hub"].isBool() && (bool) v["hub"] && v["id"].toString() == p.hub.id()
                    && v["version"].toString() == SWITCHBOARD_VERSION, "link 1, hub true, id, version");
        c.need (has (v, "track") && v["track"].isVoid(), "track null at first");

        std::thread t ([&] { SwitchboardProcessor::TrackProperties props; props.name = juce::String ("Lead Vox"); p.updateTrackProperties (props); });
        t.join();
        p.hub.pollNow();
        c.need (readJson (file)["track"].toString() == "Lead Vox", "track follows a name set from another thread");
        SwitchboardProcessor::TrackProperties renamed;
        renamed.name = juce::String ("Lead Vox Double");
        p.updateTrackProperties (renamed);
        p.hub.pollNow();
        c.need (readJson (file)["track"].toString() == "Lead Vox Double", "track follows a rename");
        p.hub.setTrackName ({});
        p.hub.pollNow();
        v = readJson (file);
        c.need (has (v, "track") && v["track"].isVoid(), "an empty name is null");

        const auto m0 = mtimeNs (file);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        bool moved = false;
        while (! moved && juce::Time::getMillisecondCounterHiRes() - t0 < 1000.0)
        {
            juce::Thread::sleep (20);
            p.hub.pollNow();
            moved = mtimeNs (file) != m0;
        }
        c.need (moved, "mtime heartbeat within 1 s");

        int others = 0;
        for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, false, "*"))
            if (f != file && f != p.hub.inbox())
                ++others;
        c.need (p.hub.inbox().isDirectory() && p.hub.inbox().getFileName() == "hub-" + p.hub.id() + ".in", "its inbox hub-<id>.in is made with it (1.1.0)");
        c.need (others == 0, "nothing else in the folder: Switchboard writes no member card");
    }
    c.need (! file.exists() && dir.isDirectory() && dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0, "removed on destruction with its inbox, the folder stays");

    // Off without SMOOTH_LINK_DIR outside an AU, so this test can never write the real folder.
    unsetenv ("SMOOTH_LINK_DIR");
    if (! smooth::Link::enabledFor (juce::AudioProcessor::wrapperType_Undefined))
    {
        SwitchboardProcessor p;
        c.need (! p.hub.enabled() && ! p.hub.file().exists(), "off without SMOOTH_LINK_DIR outside an AU");
        c.need (p.hub.file().getParentDirectory() == juce::File (realHome() + "/Library/Application Support/SmoothPlugins/link"), "an AU would write the real folder");
    }
    else
    {
        c.need (false, "enabledFor says yes without SMOOTH_LINK_DIR");
    }
    setenv ("SMOOTH_LINK_DIR", base.getFullPathName().toRawUTF8(), 1);
    check (c.ok, "hub_file_track: written at construction, track follows the name, removed on destruction");
}

// ---- Task 4: the reader (Registry), the commands (Commands, DragThrottle) and the window (SwitchboardEditor) ----

double unixNow() { return smooth::linkdetail::unixNow(); }

// 16 lowercase hex characters: a lead digit and a number. A larger lead sorts later, whatever the number.
juce::String hexId (int n, char lead = '1')
{
    return juce::String::charToString ((juce::juce_wchar) lead) + juce::String::toHexString ((juce::int64) n).paddedLeft ('0', 15);
}

// One parameter of a hand-written card, in the shape SmoothLink writes.
struct P
{
    juce::String id, name, text, label;
    double value = 0.5, def = 0.5;
    int steps = 0;
    juce::StringArray choices;
    bool main = false;
};

std::vector<P> tuneParams()
{
    return { { "retune", "Retune Speed", "3 ms", "ms", 0.03, 0.2, 0, {}, true },
             { "humanize", "Humanize", "0 %", "%", 0.0, 0.0, 0, {}, true },
             { "scale", "Scale", "Chromatic", "", 0.0, 0.0, 4, { "Chromatic", "Major", "Minor", "Blues" }, true },
             { "bypass", "Bypass", "Off", "", 0.0, 0.0, 2, { "Off", "On" }, true },
             { "mix", "Mix", "100 %", "%", 1.0, 1.0, 0, {}, false } };
}

std::vector<P> ropeParams()
{
    return { { "depth", "Depth", "14.0 dB", "dB", 0.58, 0.25, 0, {}, true },
             { "sharp", "Sharpness", "50 %", "%", 0.5, 0.5, 0, {}, true },
             { "quality", "Quality", "Normal", "", 0.5, 0.5, 3, { "Low Latency", "Normal", "High" }, true },
             { "delta", "Delta", "Off", "", 0.0, 0.0, 2, { "Off", "On" }, false } };
}

std::vector<P> punchParams()
{
    return { { "punch", "Punch", "62 %", "%", 0.62, 0.5, 0, {}, true },
             { "ceiling", "Ceiling", "-0.3 dB", "dB", 0.97, 0.97, 0, {}, true },
             { "mode", "Mode", "Tight", "", 0.0, 0.0, 2, { "Tight", "Loose" }, true } };
}

juce::String cardText (const juce::String& id, const juce::String& plugin, const juce::String& track, double started, juce::int64 seq,
                       const std::vector<P>& ps, const juce::StringArray& done = {}, const juce::StringArray& doneValues = {})
{
    auto* o = new juce::DynamicObject();
    juce::var card (o);
    o->setProperty ("link", 1);
    o->setProperty ("id", id);
    o->setProperty ("plugin", plugin);
    o->setProperty ("type", "aumf");
    o->setProperty ("subtype", "STst");
    o->setProperty ("manufacturer", "Luke");
    o->setProperty ("version", "1.9.0");
    o->setProperty ("track", track.isEmpty() ? juce::var() : juce::var (track));
    o->setProperty ("pid", 4242);
    o->setProperty ("started", started);
    o->setProperty ("seq", seq);
    juce::Array<juce::var> params;
    for (size_t i = 0; i < ps.size(); ++i)
    {
        const auto& p = ps[i];
        auto* q = new juce::DynamicObject();
        q->setProperty ("id", p.id);
        q->setProperty ("index", (int) i);
        q->setProperty ("name", p.name);
        q->setProperty ("value", p.value);
        q->setProperty ("text", p.text);
        q->setProperty ("default", p.def);
        q->setProperty ("steps", p.steps);
        juce::Array<juce::var> choices;
        for (auto& ch : p.choices)
            choices.add (ch);
        q->setProperty ("choices", choices);
        q->setProperty ("label", p.label);
        q->setProperty ("automatable", true);
        q->setProperty ("main", p.main);
        params.add (juce::var (q));
    }
    o->setProperty ("params", params);
    juce::Array<juce::var> doneList;
    for (int i = 0; i < done.size(); ++i)
    {
        auto* d = new juce::DynamicObject();
        d->setProperty ("cmd", done[i]);
        d->setProperty ("ok", true);
        d->setProperty ("param", "retune");
        d->setProperty ("value", doneValues[i].isNotEmpty() ? juce::var (doneValues[i].getDoubleValue()) : juce::var());
        d->setProperty ("text", "");
        d->setProperty ("error", "");
        doneList.add (juce::var (d));
    }
    o->setProperty ("done", doneList);
    return juce::JSON::toString (card, juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none)) + "\n";
}

void setMtime (const juce::File& f, double t)
{
    struct timeval tv[2];
    tv[0].tv_sec = (time_t) std::floor (t);
    tv[0].tv_usec = (suseconds_t) ((t - std::floor (t)) * 1.0e6);
    tv[1] = tv[0];
    utimes (f.getFullPathName().toRawUTF8(), tv);
}

// A card as a member writes it (whole file, then rename), with its inbox, and its heartbeat set to mtime.
juce::File putCard (const juce::File& dir, const juce::String& id, const juce::String& text, double mtime)
{
    dir.createDirectory();
    auto f = dir.getChildFile (id + ".json");
    smooth::linkdetail::writeAtomically (f, text);
    dir.getChildFile (id + ".in").createDirectory();
    setMtime (f, mtime);
    return f;
}

juce::StringArray namesOf (const std::vector<const hub::Instance*>& v)
{
    juce::StringArray s;
    for (auto* i : v)
        s.add (i->name);
    return s;
}

juce::String nameOf (const hub::Registry& r, const juce::String& id)
{
    auto* i = r.find (id);
    return i != nullptr ? i->name : juce::String ("(none)");
}

const hub::Param* paramOf (const hub::Registry& r, const juce::String& id, const juce::String& param)
{
    auto* i = r.find (id);
    return i != nullptr ? i->param (param) : nullptr;
}

juce::String joined (const juce::StringArray& s) { return "[" + s.joinIntoString (", ") + "]"; }

// The *.json files of an inbox in name order, the order a member applies them.
juce::StringArray commandNames (const juce::File& inbox)
{
    juce::StringArray names;
    for (const auto& f : inbox.findChildFiles (juce::File::findFiles, false, "*.json"))
        if (! f.getFileName().startsWithChar ('.'))
            names.add (f.getFileNameWithoutExtension());
    names.sort (false);
    return names;
}

float normOf (juce::AudioProcessorParameter& p) { return p.getValue(); }
float defOf (juce::AudioProcessorParameter& p) { return p.getDefaultValue(); }

juce::int64 msOf (const juce::String& commandName) { return commandName.upToFirstOccurrenceOf ("-", false, false).getLargeIntValue(); }

void testRegistryGroupsByTrack()
{
    const auto dir = useFolder ("registry_groups_by_track");
    const double now = unixNow();
    const auto tune = hexId (1), rope = hexId (2), punch = hexId (3), key = hexId (4);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Lead Vox", 100.0, 1, tuneParams()), now);
    putCard (dir, rope, cardText (rope, "Velvet Rope", "Lead Vox", 50.0, 1, ropeParams()), now);
    putCard (dir, punch, cardText (punch, "Punch", "Bass", 10.0, 1, punchParams()), now);
    putCard (dir, key, cardText (key, "Key", "", 20.0, 1, {}), now);

    Checks c;
    hub::Registry r (dir);
    c.need (r.scan (now), "the first scan reports a change");
    c.need (r.instances().size() == 4, "four instances, got " + juce::String ((int) r.instances().size()));
    c.need (r.tracks() == juce::StringArray ({ "Bass", "Lead Vox", "" }), "tracks sorted, no name last: " + joined (r.tracks()));
    c.need (namesOf (r.onTrack ("Lead Vox")) == juce::StringArray ({ "Velvet Rope", "Tune Pro" }), "Lead Vox: oldest first, " + joined (namesOf (r.onTrack ("Lead Vox"))));
    c.need (namesOf (r.onTrack ("Bass")) == juce::StringArray ({ "Punch" }), "Bass: Punch");
    c.need (namesOf (r.onTrack ("")) == juce::StringArray ({ "Key" }) && ! r.onTrack ("")[0]->hasTrack, "no track name: Key, track null");
    if (auto* t = r.find (tune))
    {
        c.need (t->plugin == "Tune Pro" && t->track == "Lead Vox" && t->hasTrack && t->version == "1.9.0" && t->seq == 1, "card fields");
        c.need (t->params.size() == 5, "five params");
        auto* retune = t->param ("retune");
        c.need (retune != nullptr && retune->name == "Retune Speed" && retune->text == "3 ms" && std::abs (retune->value - 0.03) < 1e-9
                    && std::abs (retune->def - 0.2) < 1e-9 && retune->main && retune->label == "ms", "retune read right");
        c.need (retune != nullptr && retune->kind() == hub::Kind::knob, "a continuous control is a knob");
        c.need (t->param ("scale") != nullptr && t->param ("scale")->kind() == hub::Kind::menu && t->param ("scale")->choices.size() == 4, "four named choices are a menu");
        c.need (t->param ("bypass") != nullptr && t->param ("bypass")->kind() == hub::Kind::toggle, "Off and On is a switch");
        c.need (paramOf (r, punch, "mode") != nullptr && paramOf (r, punch, "mode")->kind() == hub::Kind::segments, "two short named choices are a two-way switch");
        c.need (t->shown().size() == 4, "the window shows the four main controls of Tune Pro");
    }
    else
        c.need (false, "Tune Pro found by id");
    c.need (! r.scan (now + 0.1), "a second scan with nothing changed reports no change");

    // A rename reaches the reader with the next card write.
    putCard (dir, tune, cardText (tune, "Tune Pro", "Lead Vox Double", 100.0, 2, tuneParams()), now + 0.2);
    c.need (r.scan (now + 0.2), "a rewritten card is a change");
    c.need (namesOf (r.onTrack ("Lead Vox Double")) == juce::StringArray ({ "Tune Pro" }) && namesOf (r.onTrack ("Lead Vox")) == juce::StringArray ({ "Velvet Rope" }),
            "a renamed track takes its plug-in along");
    dir.getChildFile (punch + ".json").deleteFile();
    c.need (r.scan (now + 0.3) && ! r.tracks().contains ("Bass") && r.find (punch) == nullptr, "a card that goes takes its track along");
    check (c.ok, "registry_groups_by_track: members grouped by track, oldest first, renames follow, gone cards go");
}

void testSameNameNumbered()
{
    const auto dir = useFolder ("same_name_numbered");
    const double now = unixNow();
    // Two tracks both named Vox: their plug-ins share the name. The file names sort against the start order on purpose.
    const auto a = hexId (1, 'f'), b = hexId (2, 'a'), d = hexId (3, 'c'), rope = hexId (4, 'b'), other = hexId (5, 'd');
    putCard (dir, a, cardText (a, "Tune Pro", "Vox", 100.0, 1, tuneParams()), now);
    putCard (dir, b, cardText (b, "Tune Pro", "Vox", 200.0, 1, tuneParams()), now);
    putCard (dir, d, cardText (d, "Tune Pro", "Vox", 300.0, 1, tuneParams()), now);
    putCard (dir, rope, cardText (rope, "Velvet Rope", "Vox", 150.0, 1, ropeParams()), now);
    putCard (dir, other, cardText (other, "Tune Pro", "Bass", 50.0, 1, tuneParams()), now);
    Checks c;
    hub::Registry r (dir);
    r.scan (now);
    c.need (r.instances().size() == 5, "nothing lost: all five show");
    const auto vox = namesOf (r.onTrack ("Vox"));
    c.need (vox == juce::StringArray ({ "Tune Pro", "Velvet Rope", "Tune Pro 2", "Tune Pro 3" }), "Vox by start time, numbered: " + joined (vox));
    c.need (nameOf (r, a) == "Tune Pro" && nameOf (r, b) == "Tune Pro 2" && nameOf (r, d) == "Tune Pro 3", "numbered by started, not by file name");
    c.need (nameOf (r, other) == "Tune Pro", "numbering is per track: Tune Pro on Bass has no number");
    c.need (nameOf (r, rope) == "Velvet Rope", "a name used once has no number");
    dir.getChildFile (a + ".json").deleteFile();
    r.scan (now + 0.1);
    c.need (nameOf (r, b) == "Tune Pro" && nameOf (r, d) == "Tune Pro 2", "when the oldest goes the others count again from the top");
    check (c.ok, "same_name_numbered: two tracks with one name show every plug-in, Name, Name 2, Name 3 by started");
}

void testStaleHiddenAfter5s()
{
    const auto dir = useFolder ("stale_hidden_after_5s");
    const double now = unixNow();
    const auto fresh = hexId (1), late = hexId (2), gone = hexId (3), old599 = hexId (4), old700 = hexId (5);
    putCard (dir, fresh, cardText (fresh, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    putCard (dir, late, cardText (late, "Punch", "Vox", 2.0, 1, punchParams()), now - 4.5);
    putCard (dir, gone, cardText (gone, "Key", "Vox", 3.0, 1, {}), now - 5.5);
    putCard (dir, old599, cardText (old599, "Morph", "Vox", 4.0, 1, {}), now - 599.0);
    putCard (dir, old700, cardText (old700, "Throat", "Vox", 5.0, 1, {}), now - 700.0);
    dir.getChildFile (old700 + ".in").getChildFile ("1-test-abcdef.json").replaceWithText ("{}");
    setMtime (dir.getChildFile (old700 + ".in"), now - 700.0);

    Checks c;
    hub::Registry r (dir);
    r.scan (now);
    c.need (namesOf (r.instances()) == juce::StringArray ({ "Tune Pro", "Punch" }), "4.5 s shows, 5.5 s is hidden: " + joined (namesOf (r.instances())));
    c.need (r.find (fresh) != nullptr && r.responding (*r.find (fresh), now), "a fresh card is responding");
    c.need (r.find (late) != nullptr && ! r.responding (*r.find (late), now), "a card 4.5 s old shows as not responding");
    c.need (r.find (gone) == nullptr, "a card 5.5 s old is hidden");
    r.scan (now + 1.0);
    c.need (namesOf (r.instances()) == juce::StringArray ({ "Tune Pro" }), "a second later the late one is hidden too");
    c.need (r.find (fresh) != nullptr && r.responding (*r.find (fresh), now + 1.0), "1 s old is still responding");
    c.need (dir.getChildFile (fresh + ".json").existsAsFile() && dir.getChildFile (late + ".json").existsAsFile()
                && dir.getChildFile (gone + ".json").existsAsFile() && dir.getChildFile (gone + ".in").isDirectory(), "hidden cards and their inboxes stay");
    c.need (dir.getChildFile (old599 + ".json").existsAsFile() && dir.getChildFile (old599 + ".in").isDirectory(), "599 s old: never deleted by a reader");
    c.need (! dir.getChildFile (old700 + ".json").exists() && ! dir.getChildFile (old700 + ".in").exists(), "700 s old: card and inbox deleted");
    setMtime (dir.getChildFile (gone + ".json"), unixNow());
    r.scan (unixNow());
    c.need (r.find (gone) != nullptr, "a hidden card shows again when its heartbeat comes back");
    check (c.ok, "stale_hidden_after_5s: late cards show as not responding, hidden after 5 s, deleted only past 600 s");
}

void testMalformedSkippedNotDeleted()
{
    const auto dir = useFolder ("malformed_skipped_not_deleted");
    const double now = unixNow();
    const auto good = hexId (1);
    putCard (dir, good, cardText (good, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    const auto full = cardText (hexId (6), "Punch", "Vox", 1.0, 1, punchParams());
    const juce::StringArray bad { "{not json", "[1,2,3]", "{\"link\":1,\"plugin\":\"No Params\"}", "", full.substring (0, full.length() / 2) };
    for (int i = 0; i < bad.size(); ++i)
        putCard (dir, hexId (2 + i), bad[i], now);

    Checks c;
    hub::Registry r (dir);
    r.scan (now);
    c.need (namesOf (r.instances()) == juce::StringArray ({ "Tune Pro" }), "only the good card shows: " + joined (namesOf (r.instances())));
    r.scan (now + 0.1);
    c.need (r.parsedLastScan() == 0, "a bad card is not read again until it changes, parsed " + juce::String (r.parsedLastScan()));
    r.scan (now + 0.2);
    bool all = true;
    for (int i = 0; i < bad.size(); ++i)
        all = all && dir.getChildFile (hexId (2 + i) + ".json").existsAsFile();
    c.need (all, "no bad card is deleted");
    putCard (dir, hexId (2), cardText (hexId (2), "Key", "Vox", 2.0, 1, {}), now + 0.3);
    r.scan (now + 0.3);
    c.need (namesOf (r.instances()) == juce::StringArray ({ "Tune Pro", "Key" }), "a bad card that becomes good shows");
    check (c.ok, "malformed_skipped_not_deleted: bad cards are skipped, kept, and read again when they change");
}

void testHubAndTmpFilesSkipped()
{
    const auto dir = useFolder ("hub_and_tmp_files_skipped");
    const double now = unixNow();
    const auto good = hexId (1);
    putCard (dir, good, cardText (good, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    const auto anyCard = cardText (hexId (2), "Punch", "Vox", 1.0, 1, punchParams());
    const juce::StringArray others { "hub-" + hexId (3) + ".json", hexId (4) + ".json.tmp1a2b3c4d", "." + hexId (5) + ".json",
                                     hexId (0xabcdef, 'e').toUpperCase() + ".json", hexId (7).substring (1) + ".json", hexId (8) + "0.json", "notes.txt",
                                     hexId (9) + ".JSON" };
    for (auto& n : others)
        dir.getChildFile (n).replaceWithText (n.startsWith ("hub-") ? "{\"link\":1,\"hub\":true,\"id\":\"" + hexId (3) + "\",\"track\":\"Vox\",\"version\":\"1.0.0\"}" : anyCard);
    dir.getChildFile (hexId (10) + ".json").createDirectory();   // a folder with a card's name
    symlink (dir.getChildFile (good + ".json").getFullPathName().toRawUTF8(), dir.getChildFile (hexId (11) + ".json").getFullPathName().toRawUTF8());

    Checks c;
    hub::Registry r (dir);
    r.scan (now);
    c.need (namesOf (r.instances()) == juce::StringArray ({ "Tune Pro" }), "only <16 hex>.json files are cards: " + joined (namesOf (r.instances())));
    bool kept = dir.getChildFile (hexId (10) + ".json").isDirectory();
    for (auto& n : others)
        kept = kept && dir.getChildFile (n).existsAsFile();
    c.need (kept, "nothing skipped is deleted");
    check (c.ok, "hub_and_tmp_files_skipped: hub files, temp files, hidden files, odd names, folders and links are not cards");
}

void testScan200()
{
    const auto dir = useFolder ("scan_200_cards_unchanged_under_5ms");
    const double now = unixNow();
    std::vector<P> many;
    for (int k = 0; k < 24; ++k)
        many.push_back ({ "p" + juce::String (k), "Control " + juce::String (k), juce::String (k) + " %", "%", k / 24.0, 0.5, 0, {}, k < 6 });
    for (int i = 0; i < 200; ++i)
        putCard (dir, hexId (i), cardText (hexId (i), "Plug " + juce::String (i % 14), "Track " + juce::String (i % 20), 1000.0 + i, 1, many), now);
    Checks c;
    hub::Registry r (dir);
    r.scan (now);
    c.need (r.instances().size() == 200 && r.parsedLastScan() == 200, "200 cards read once");
    double worst = 0.0, total = 0.0;
    int parsed = 0;
    for (int k = 0; k < 50; ++k)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        r.scan (now + 0.01 * k);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        worst = std::max (worst, ms);
        total += ms;
        parsed += r.parsedLastScan();
    }
    c.need (parsed == 0, "nothing read again with nothing changed");
    // The heartbeat moves every card's mtime twice a second. That alone is no reason to read a card again.
    for (int i = 0; i < 200; ++i)
        setMtime (dir.getChildFile (hexId (i) + ".json"), now + 0.6);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    r.scan (now + 0.6);
    const double heartbeatMs = juce::Time::getMillisecondCounterHiRes() - t0;
    worst = std::max (worst, heartbeatMs);
    c.need (r.parsedLastScan() == 0 && r.instances().size() == 200, "a heartbeat on all 200 reads none of them again");
    putCard (dir, hexId (7), cardText (hexId (7), "Plug 7", "Track 7", 1007.0, 2, many), now + 0.7);
    r.scan (now + 0.7);
    c.need (r.parsedLastScan() == 1, "one changed card is read again, alone");
    info ("scan of 200 unchanged cards, mean", total / 50.0, "ms");
    check (c.ok && worst < 5.0, "scan_200_cards_unchanged_under_5ms: worst scan with nothing changed", worst, "ms");
}

void testCommandFileAtomicAndNamed()
{
    const auto dir = useFolder ("command_file_atomic_and_named");
    const double now = unixNow();
    const auto id = hexId (1);
    putCard (dir, id, cardText (id, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    const auto inbox = dir.getChildFile (id + ".in");
    Checks c;
    hub::Commands cmd (dir);
    const auto name = cmd.setValue (id, "retune", 0.75, hub::Gesture::tap, now);
    c.need (std::regex_match (name.toStdString(), std::regex ("^[0-9]{13}-switchboard-[0-9a-f]{6}$")), "named <unix ms>-switchboard-<6 hex>: " + name);
    c.need (std::abs ((double) msOf (name) - now * 1000.0) < 2.0, "the ms part is the time of sending");
    c.need (inbox.getChildFile (name + ".json").existsAsFile(), "written as <name>.json in the inbox");
    c.need (inbox.getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*") == 1 && inbox.findChildFiles (juce::File::findFiles, false, ".*").isEmpty(),
            "no temporary file left behind");
    auto v = readJson (inbox.getChildFile (name + ".json"));
    c.need (v["param"].toString() == "retune" && std::abs ((double) v["value"] - 0.75) < 1e-12 && v["gesture"].toString() == "tap"
                && v["from"].toString() == "switchboard" && ! has (v, "text"), "a value command: param, value, gesture, from");
    const auto textName = cmd.setText (id, "retune", "default", hub::Gesture::tap, now);
    v = readJson (inbox.getChildFile (textName + ".json"));
    c.need (v["text"].toString() == "default" && ! has (v, "value") && v["param"].toString() == "retune", "a text command has text and no value");
    c.need (textName.compare (name) > 0, "a later command sorts later even within the same ms");
    for (const auto& f : inbox.findChildFiles (juce::File::findFiles, false, "*"))
        f.deleteFile();

    // 100 at once: the order a member applies them (name order) is the order they were sent.
    juce::StringArray sent;
    for (int i = 0; i < 100; ++i)
        sent.add (cmd.setValue (id, "retune", i / 100.0, hub::Gesture::move, now));
    auto sorted = sent;
    sorted.sort (false);
    bool rising = true;
    for (int i = 1; i < sent.size(); ++i)
        rising = rising && msOf (sent[i]) > msOf (sent[i - 1]);
    c.need (sorted == sent && rising && commandNames (inbox).size() == 100, "100 commands sent at once sort in sending order");
    for (const auto& f : inbox.findChildFiles (juce::File::findFiles, false, "*"))
        f.deleteFile();

    // No inbox (the plug-in closed): nothing is written and no folder is made.
    const auto missing = hexId (99);
    c.need (cmd.setValue (missing, "retune", 0.5, hub::Gesture::tap, now).isEmpty() && ! dir.getChildFile (missing + ".in").exists(),
            "no inbox: nothing written, no inbox made");

    // A reader polling the inbox as a member does never sees a part of a command.
    std::atomic<bool> stop { false };
    std::atomic<int> read { 0 }, failures { 0 };
    std::thread reader ([&]
    {
        while (! stop.load() || commandNames (inbox).size() > 0)
        {
            for (auto& n : commandNames (inbox))
            {
                std::string bytes;
                const auto path = inbox.getChildFile (n + ".json").getFullPathName().toStdString();
                if (! smooth::linkdetail::readFile (path, bytes))
                    continue;
                juce::var parsed;
                if (juce::JSON::parse (juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), parsed).failed() || parsed["param"].toString() != "retune")
                    ++failures;
                unlink (path.c_str());
                ++read;
            }
        }
    });
    for (int i = 0; i < 500; ++i)
        cmd.setValue (id, "retune", (i % 100) / 100.0, hub::Gesture::move, unixNow());
    stop.store (true);
    reader.join();
    c.need (read.load() == 500 && failures.load() == 0, "a reader read " + juce::String (read.load()) + " of 500, " + juce::String (failures.load()) + " partial");
    check (c.ok, "command_file_atomic_and_named: .<name>.tmp then <name>.json, named <unix ms>-switchboard-<6 hex>, in sending order");
}

void testUnansweredFailsIn3s()
{
    const auto dir = useFolder ("unanswered_fails_in_3s");
    const double now = unixNow();
    const auto id = hexId (1);
    putCard (dir, id, cardText (id, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    Checks c;
    hub::Registry r (dir);
    hub::Commands cmd (dir);
    r.scan (now);
    const auto name = cmd.setValue (id, "retune", 0.5, hub::Gesture::tap, now);
    const auto file = dir.getChildFile (id + ".in").getChildFile (name + ".json");
    c.need (cmd.settle (r, now + 2.9).empty() && cmd.pending() == 1 && file.existsAsFile(), "no answer yet at 2.9 s: still waiting");
    const auto out = cmd.settle (r, now + 3.05);
    c.need (out.size() == 1 && out[0].name == name && out[0].instance == id && ! out[0].answered && ! out[0].ok && out[0].error.isNotEmpty(),
            "at 3 s: a failure, \"did not answer\"");
    c.need (! file.exists() && cmd.pending() == 0, "the command file is taken back");

    // An answered command is confirmed by the card's done list, and with it every earlier one of ours to the same plug-in.
    const auto first = cmd.setValue (id, "retune", 0.1, hub::Gesture::begin, now);
    const auto second = cmd.setValue (id, "retune", 0.2, hub::Gesture::end, now);
    putCard (dir, id, cardText (id, "Tune Pro", "Vox", 1.0, 2, tuneParams(), { second }, { "0.2" }), unixNow());
    r.scan (unixNow());
    const auto both = cmd.settle (r, unixNow());
    c.need (both.size() == 2 && both[0].answered && both[0].ok && both[1].answered && both[1].ok && cmd.pending() == 0,
            "the newest confirmed answers the older ones too (a member applies in name order)");
    check (c.ok, "unanswered_fails_in_3s: no answer in 3 s is a failure and the command file goes; done confirms");
}

void testDragThrottle()
{
    struct Sent { double t; hub::Gesture g; double v; };
    std::vector<Sent> sent;
    double now = 100.0;
    hub::DragThrottle drag ([&] (double v, hub::Gesture g) { sent.push_back ({ now, g, v }); return juce::String ("n") + juce::String ((int) sent.size()); });
    Checks c;
    drag.press (0.2, now);
    drag.release (now + 0.1);
    c.need (sent.empty() && ! drag.busy(), "a click without a move sends nothing");

    drag.press (0.2, now);
    double v = 0.2;
    for (int i = 0; i < 500; ++i)   // 2 s of mouse moves every 4 ms
    {
        now += 0.004;
        v = 0.2 + 0.6 * (i + 1) / 500.0;
        drag.drag (v, now);
        if (i % 8 == 0)
            drag.tick (now);
    }
    drag.release (now);
    for (int k = 0; k < 10 && drag.busy(); ++k)
    {
        now += 0.0333;
        drag.tick (now);
    }
    int begins = 0, ends = 0, moves = 0, worstWindow = 0;
    for (auto& s : sent)
        (s.g == hub::Gesture::begin ? begins : (s.g == hub::Gesture::end ? ends : moves)) += 1;
    for (size_t i = 0; i < sent.size(); ++i)
    {
        int n = 0;
        for (size_t j = i; j < sent.size() && sent[j].t < sent[i].t + 1.0; ++j)
            ++n;
        worstWindow = std::max (worstWindow, n);
    }
    c.need (! sent.empty() && sent.front().g == hub::Gesture::begin && sent.back().g == hub::Gesture::end && begins == 1 && ends == 1,
            "one begin first, one end last");
    c.need (! sent.empty() && std::abs (sent.back().v - v) < 1e-12, "the end carries the last value");
    c.need (moves >= 50, "it keeps up: " + juce::String (moves) + " moves in 2 s");
    c.need (! drag.busy() && drag.lastName() == "n" + juce::String ((int) sent.size()), "nothing left to send, last name known");
    check (c.ok && worstWindow <= 30, "drag_throttle_30_per_second: begin, moves, end, most commands in any 1 s", worstWindow, "commands");
}

void testEndToEndInProcess()
{
    const auto dir = useFolder ("end_to_end_in_process");
    Checks c;
    linktest::TestProcessor plugin;
    struct Gestures : juce::AudioProcessorParameter::Listener
    {
        int begins = 0, ends = 0;
        void parameterValueChanged (int, float) override {}
        void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends) += 1; }
    } gestures;
    plugin.params.gain->addListener (&gestures);
    SwitchboardProcessor::TrackProperties props;
    props.name = juce::String ("Lead Vox");
    plugin.updateTrackProperties (props);
    plugin.link.pollNow();

    hub::Registry r (dir);
    hub::Commands cmd (dir);
    r.scan (unixNow());
    const auto on = r.onTrack ("Lead Vox");
    c.need (on.size() == 1 && on[0]->name == "Link Test" && on[0]->id == plugin.link.id() && on[0]->params.size() == 5, "the plug-in shows on Lead Vox");
    if (on.size() == 1)
    {
        const auto id = on[0]->id;
        const auto settleAfterPoll = [&] (const juce::String& name)
        {
            plugin.link.pollNow();
            r.scan (unixNow());
            for (auto& o : cmd.settle (r, unixNow()))
                if (o.name == name)
                    return o.answered && o.ok;
            return false;
        };
        const auto name = cmd.setValue (id, "gain", 0.75, hub::Gesture::tap, unixNow());
        c.need (settleAfterPoll (name), "the set is confirmed by the card's done list");
        c.need (std::abs (normOf (*plugin.params.gain) - 0.75f) < 1e-6f, "the parameter moved");
        auto* gain = paramOf (r, id, "gain");
        c.need (gain != nullptr && std::abs (gain->value - 0.75) < 1e-6 && gain->text.contains ("15"), "the card shows it: " + (gain != nullptr ? gain->text : juce::String()));
        c.need (gestures.begins == 1 && gestures.ends == 1, "with one gesture");
        const auto back = cmd.setText (id, "gain", "default", hub::Gesture::tap, unixNow());
        c.need (settleAfterPoll (back) && std::abs (normOf (*plugin.params.gain) - defOf (*plugin.params.gain)) < 1e-6f, "\"default\" puts it back");

        // A drag: begin, moves, end through the throttle, polled as a member does, is one gesture.
        gestures.begins = gestures.ends = 0;
        double t = unixNow();
        juce::String last;
        hub::DragThrottle drag ([&] (double v, hub::Gesture g) { return last = cmd.setValue (id, "gain", v, g, t); });
        drag.press (0.333, t);
        for (int i = 1; i <= 30; ++i)
        {
            t += 0.01;
            drag.drag (0.333 + i * 0.01, t);
            if (i % 3 == 0)
                plugin.link.pollNow();
        }
        drag.release (t);
        while (drag.busy())
        {
            t += 0.034;
            drag.tick (t);
        }
        c.need (settleAfterPoll (last), "the drag's end is confirmed");
        c.need (gestures.begins == 1 && gestures.ends == 1, "a drag is one gesture: " + juce::String (gestures.begins) + " begin, " + juce::String (gestures.ends) + " end");
        c.need (std::abs (normOf (*plugin.params.gain) - 0.633f) < 1e-4f, "the parameter ends where the drag ended");
    }
    plugin.params.gain->removeListener (&gestures);
    check (c.ok, "end_to_end_in_process: a Link in this process, Registry finds it, a set, a default and a drag land and are confirmed");
}

// A mouse event on a control, as JUCE would deliver it.
juce::MouseEvent mouseAt (juce::Component& comp, juce::Point<float> pos, int clicks, bool dragged, juce::Point<float> downPos)
{
    const auto when = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &comp, &comp, when, downPos, when, clicks, dragged);
}

struct Window
{
    explicit Window (const juce::String& track)
    {
        if (track.isNotEmpty())
        {
            SwitchboardProcessor::TrackProperties props;
            props.name = track;
            p.updateTrackProperties (props);
        }
        ed.reset (p.createEditorIfNeeded());
        w = dynamic_cast<SwitchboardEditor*> (ed.get());
    }
    ~Window() { p.editorBeingDeleted (ed.get()); }

    // Switchboard opens on Tracks (1.1.0). The 1.0.0 window tests look at one track's plug-ins: a tap on that track first.
    void openTrack (const juce::String& track)
    {
        if (w != nullptr)
            w->show (hub::Screen::onTrack (track));
    }

    // Closes the window as a host does, the plug-in stays, and opens it again.
    void reopen()
    {
        p.editorBeingDeleted (ed.get());
        ed.reset();
        ed.reset (p.createEditorIfNeeded());
        w = dynamic_cast<SwitchboardEditor*> (ed.get());
    }

    SwitchboardProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    SwitchboardEditor* w = nullptr;
};

void testWindowShowsTrack()
{
    useFolder ("window_shows_track");
    Checks c;
    {
        Window win ("");
        auto* w = win.w;
        c.need (w != nullptr && w->getWidth() > 0 && w->getHeight() > 0, "the window opens");
        if (w != nullptr)
        {
            w->tickNow (unixNow());
            c.need (w->shownTrack().isEmpty(), "no name at first");
            SwitchboardProcessor::TrackProperties props;
            props.name = juce::String ("Bass");
            win.p.updateTrackProperties (props);
            w->tickNow (unixNow());
            c.need (w->shownTrack() == "Bass" && w->subtitleText().contains ("Bass"), "shows the track name: " + w->subtitleText());
            juce::Image img (juce::Image::ARGB, w->getWidth(), w->getHeight(), true);
            juce::Graphics g (img);
            w->paintEntireComponent (g, false);
            c.need (img.getPixelAt (w->getWidth() - 4, w->getHeight() - 4).getAlpha() == 255, "the window paints");
        }
    }
    check (c.ok, "window_shows_track: the window shows its own track name");
}

void testWindowControlKinds()
{
    const auto dir = useFolder ("window_control_kinds");
    const double now = unixNow();
    const auto tune = hexId (1), punch = hexId (2), plain = hexId (3);
    std::vector<P> eight;
    for (int k = 0; k < 8; ++k)
        eight.push_back ({ "c" + juce::String (k), "Control " + juce::String (k), "0.5", "", 0.5, 0.5, 0, {}, false });
    putCard (dir, tune, cardText (tune, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    putCard (dir, punch, cardText (punch, "Punch", "Vox", 2.0, 1, punchParams()), now);
    putCard (dir, plain, cardText (plain, "Old Plug", "Vox", 3.0, 1, eight), now);
    Checks c;
    {
        Window win ("Vox");
        win.openTrack ("Vox");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            const auto kindOf = [&] (const juce::String& id, const juce::String& param)
            {
                auto* ctl = w->findControl (id, param);
                return ctl == nullptr ? juce::String ("none") : juce::String ((int) ctl->kind());
            };
            c.need (kindOf (tune, "retune") == juce::String ((int) hub::Kind::knob) && kindOf (tune, "humanize") == juce::String ((int) hub::Kind::knob), "knobs");
            c.need (kindOf (tune, "scale") == juce::String ((int) hub::Kind::menu), "a menu for four choices");
            c.need (kindOf (tune, "bypass") == juce::String ((int) hub::Kind::toggle), "a switch for Off and On");
            c.need (kindOf (punch, "mode") == juce::String ((int) hub::Kind::segments), "a two-way switch for Tight and Loose");
            c.need (kindOf (tune, "mix") == "none", "a control that is not main stays in All controls");
            c.need (w->findControl (plain, "c5") != nullptr && w->findControl (plain, "c6") == nullptr, "no main controls: the first six show");
            if (auto* knob = w->findControl (tune, "retune"))
                c.need (knob->shownText() == "3 ms" && std::abs (knob->shownValue() - 0.03) < 1e-9, "a knob shows the card's value and text");
        }
    }
    check (c.ok, "window_control_kinds: knobs, switches, two-way switches and menus from steps and choices; main controls first");
}

void testWindowDoubleClickDefault()
{
    const auto dir = useFolder ("window_double_click_default");
    const double now = unixNow();
    const auto tune = hexId (1);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    Checks c;
    {
        Window win ("Vox");
        win.openTrack ("Vox");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            if (auto* knob = w->findControl (tune, "retune"))
            {
                const auto centre = knob->getLocalBounds().getCentre().toFloat();
                knob->mouseDoubleClick (mouseAt (*knob, centre, 2, false, centre));
                const auto names = commandNames (dir.getChildFile (tune + ".in"));
                c.need (names.size() == 1, "one command for a double-click, got " + juce::String (names.size()));
                if (names.size() == 1)
                {
                    auto v = readJson (dir.getChildFile (tune + ".in").getChildFile (names[0] + ".json"));
                    c.need (v["param"].toString() == "retune" && v["text"].toString() == "default" && ! has (v, "value") && v["gesture"].toString() == "tap",
                            "text \"default\" as a tap");
                }
                c.need (std::abs (knob->shownValue() - 0.2) < 1e-9, "the knob shows the default at once");
            }
            else
                c.need (false, "the retune knob is there");
            if (auto* sw = w->findControl (tune, "bypass"))
            {
                const auto centre = sw->getLocalBounds().getCentre().toFloat();
                sw->mouseDown (mouseAt (*sw, centre, 1, false, centre));
                sw->mouseUp (mouseAt (*sw, centre, 1, false, centre));
                const auto names = commandNames (dir.getChildFile (tune + ".in"));
                auto v = names.size() == 2 ? readJson (dir.getChildFile (tune + ".in").getChildFile (names[1] + ".json")) : juce::var();
                c.need (v["param"].toString() == "bypass" && std::abs ((double) v["value"] - 1.0) < 1e-12 && v["gesture"].toString() == "tap", "a click on a switch turns it on");
                sw->mouseDoubleClick (mouseAt (*sw, centre, 2, false, centre));
                const auto after = commandNames (dir.getChildFile (tune + ".in"));
                auto d = after.size() == 3 ? readJson (dir.getChildFile (tune + ".in").getChildFile (after[2] + ".json")) : juce::var();
                c.need (d["param"].toString() == "bypass" && d["text"].toString() == "default", "a double-click on a switch puts it back to its default");
            }
        }
    }
    check (c.ok, "window_double_click_default: double-click sends text \"default\"; a switch click sends a tap");
}

void testWindowDragFollows()
{
    const auto dir = useFolder ("window_drag_follows");
    const double start = unixNow();
    const auto tune = hexId (1);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Vox", 1.0, 1, tuneParams()), start);
    Checks c;
    {
        Window win ("Vox");
        win.openTrack ("Vox");
        if (auto* w = win.w)
        {
            double t = start;
            w->clock = [&t] { return t; };
            w->tickNow (t);
            if (auto* knob = w->findControl (tune, "retune"))
            {
                const auto centre = knob->getLocalBounds().getCentre().toFloat();
                knob->mouseDown (mouseAt (*knob, centre, 1, false, centre));
                double followed = -1.0;
                for (int i = 1; i <= 60; ++i)
                {
                    t += 0.005;
                    knob->mouseDrag (mouseAt (*knob, centre.translated (0.0f, -1.5f * (float) i), 1, true, centre));
                    if (i == 3)
                        followed = knob->shownValue();
                    if (i % 7 == 0)
                        w->tickNow (t);
                }
                const double local = knob->shownValue();
                knob->mouseUp (mouseAt (*knob, centre.translated (0.0f, -90.0f), 1, true, centre));
                for (int k = 0; k < 10; ++k)
                {
                    t += 0.02;
                    w->tickNow (t);
                }
                c.need (followed > 0.03, "the knob follows the drag at once, before any card");
                const auto inbox = dir.getChildFile (tune + ".in");
                const auto names = commandNames (inbox);
                juce::StringArray gestures;
                double lastValue = -1.0;
                bool rising = true, spaced = true;
                for (int i = 0; i < names.size(); ++i)
                {
                    auto v = readJson (inbox.getChildFile (names[i] + ".json"));
                    gestures.add (v["gesture"].toString());
                    rising = rising && (double) v["value"] >= lastValue;
                    lastValue = (double) v["value"];
                    if (i > 0)
                        spaced = spaced && msOf (names[i]) - msOf (names[i - 1]) >= 33;
                }
                c.need (gestures.size() >= 3 && gestures[0] == "begin" && gestures[gestures.size() - 1] == "end", "begin first, end last: " + joined (gestures));
                c.need (gestures.size() >= 3 && gestures.joinIntoString (",").fromFirstOccurrenceOf ("begin,", false, false).upToLastOccurrenceOf (",end", false, false)
                                                     .removeCharacters ("move,").isEmpty(), "moves between");
                c.need (spaced, "at most 30 commands a second (every gap at least 33 ms)");
                c.need (rising && std::abs (lastValue - local) < 1e-9, "the values climb to where the drag ended");
                c.need (std::abs (knob->shownValue() - local) < 1e-9, "with no answer yet the knob stays where the drag left it");

                // The member applies them and its card answers: the knob shows the card from then on.
                putCard (dir, tune, cardText (tune, "Tune Pro", "Vox", 1.0, 2, [&] { auto ps = tuneParams(); ps[0].value = lastValue; ps[0].text = "42 ms"; return ps; }(),
                                              names, [&] { juce::StringArray s; for (int i = 0; i < names.size(); ++i) s.add (""); return s; }()), unixNow());
                t += 0.1;
                w->tickNow (t);
                c.need (std::abs (knob->shownValue() - lastValue) < 1e-9 && knob->shownText() == "42 ms", "after the answer the knob shows the card: " + knob->shownText());
                putCard (dir, tune, cardText (tune, "Tune Pro", "Vox", 1.0, 3, [&] { auto ps = tuneParams(); ps[0].value = 0.9; ps[0].text = "300 ms"; return ps; }()), unixNow());
                t += 0.1;
                w->tickNow (t);
                c.need (std::abs (knob->shownValue() - 0.9) < 1e-9, "and follows the card when the plug-in moves it (automation, its own window)");
            }
            else
                c.need (false, "the retune knob is there");
        }
    }
    check (c.ok, "window_drag_follows: a drag sends begin, moves, end at most 30 a second; the knob follows the drag, then the card");
}

void testWindowNotResponding()
{
    const auto dir = useFolder ("window_not_responding");
    const double now = unixNow();
    const auto tune = hexId (1), punch = hexId (2);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now - 3.0);
    putCard (dir, punch, cardText (punch, "Punch", "Vox", 2.0, 1, punchParams()), now);
    Checks c;
    {
        Window win ("Vox");
        win.openTrack ("Vox");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            c.need (w->isGreyed (tune) && w->statusOf (tune) == "not responding", "a card 3 s old greys out: " + w->statusOf (tune));
            c.need (w->findControl (tune, "retune") != nullptr && ! w->findControl (tune, "retune")->isEnabled(), "its controls are off");
            c.need (! w->isGreyed (punch) && w->statusOf (punch) != "not responding", "a fresh card is live");
            setMtime (dir.getChildFile (tune + ".json"), unixNow());
            w->tickNow (unixNow());
            c.need (! w->isGreyed (tune) && w->findControl (tune, "retune") != nullptr && w->findControl (tune, "retune")->isEnabled(), "live again when the heartbeat comes back");

            // A set the plug-in never answers greys it too, and the command file goes.
            auto* sw = w->findControl (punch, "mode");
            if (sw != nullptr)
            {
                // On the second choice (Loose): a click on the choice already on changes nothing.
                const juce::Point<float> loose ((float) sw->getWidth() * 0.8f, (float) sw->getHeight() * 0.48f);
                sw->mouseDown (mouseAt (*sw, loose, 1, false, loose));
                sw->mouseUp (mouseAt (*sw, loose, 1, false, loose));
            }
            c.need (sw != nullptr && commandNames (dir.getChildFile (punch + ".in")).size() == 1, "a click on the two-way switch sends one command");
            const double later = unixNow() + 3.2;
            setMtime (dir.getChildFile (punch + ".json"), later);
            setMtime (dir.getChildFile (tune + ".json"), later);
            w->tickNow (later);
            c.need (w->isGreyed (punch) && w->statusOf (punch) == "not responding", "no answer in 3 s greys it out: " + w->statusOf (punch));
            c.need (commandNames (dir.getChildFile (punch + ".in")).isEmpty(), "and its command file is taken back");
            putCard (dir, punch, cardText (punch, "Punch", "Vox", 2.0, 2, punchParams()), later + 0.1);
            w->tickNow (later + 0.1);
            c.need (! w->isGreyed (punch), "a new card from it makes it live again");
        }
    }
    check (c.ok, "window_not_responding: a card that stops answering greys out with \"not responding\"");
}

void testWindowAllControls()
{
    const auto dir = useFolder ("window_all_controls");
    const double now = unixNow();
    const auto tune = hexId (1), rope = hexId (2);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    putCard (dir, rope, cardText (rope, "Velvet Rope", "Vox", 2.0, 1, ropeParams()), now);
    Checks c;
    {
        Window win ("Vox");
        win.openTrack ("Vox");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            c.need (w->allControlsRows() < 0, "closed at first");
            w->openAllControls (tune);
            c.need (w->allControlsRows() == 5, "one plug-in's list holds every control: " + juce::String (w->allControlsRows()));
            w->searchAllControls ("mix");
            c.need (w->allControlsRows() == 1, "search by control name");
            w->searchAllControls ("");
            c.need (w->allControlsRows() == 5, "an empty search shows all again");
            w->searchAllControls ("retune speed");
            c.need (w->allControlsRows() == 1, "every word must match");
            w->searchAllControls ("mix");
            if (auto* mix = w->findAllControlsControl (tune, "mix"))
            {
                const auto centre = mix->getLocalBounds().getCentre().toFloat();
                mix->mouseDoubleClick (mouseAt (*mix, centre, 2, false, centre));
                const auto names = commandNames (dir.getChildFile (tune + ".in"));
                c.need (names.size() == 1 && readJson (dir.getChildFile (tune + ".in").getChildFile (names[0] + ".json"))["param"].toString() == "mix",
                        "a control in the list sets the plug-in too");
            }
            else
                c.need (false, "the list has a control for mix");
            w->closeAllControls();
            c.need (w->allControlsRows() < 0, "closes");
            w->openAllControls (rope);
            c.need (w->allControlsRows() == 4, "another plug-in's list holds its own controls: " + juce::String (w->allControlsRows()));
            w->searchAllControls ("high");
            c.need (w->allControlsRows() == 1, "search by a choice name finds the control that has it");
            w->searchAllControls ("retune");
            c.need (w->allControlsRows() == 0, "a list holds one plug-in only");
            w->closeAllControls();
        }
    }
    check (c.ok, "window_all_controls: a searchable list of every control of one plug-in");
}

void saveImage (const juce::Image& img, const juce::File& out)
{
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat().writeImageToStream (img, stream);
}

juce::Image paintAt (juce::Component& comp, float scale)
{
    juce::Image img (juce::Image::ARGB, juce::roundToInt ((float) comp.getWidth() * scale), juce::roundToInt ((float) comp.getHeight() * scale), true);
    juce::Graphics g (img);
    g.addTransform (juce::AffineTransform::scale (scale));
    comp.paintEntireComponent (g, false);
    return img;
}

void testWindowRender()
{
    const auto dir = useFolder ("window_render");
    const double now = unixNow();
    auto tape = punchParams();
    tape[0] = { "drive", "Drive", "4.5 dB", "dB", 0.45, 0.3, 0, {}, true };
    putCard (dir, hexId (1), cardText (hexId (1), "Velvet Rope", "Lead Vox", 100.0, 1, ropeParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Tune Pro", "Lead Vox", 200.0, 1, tuneParams()), now);
    putCard (dir, hexId (3), cardText (hexId (3), "Punch", "Lead Vox", 300.0, 1, punchParams()), now);
    putCard (dir, hexId (4), cardText (hexId (4), "Tape Color", "Lead Vox", 400.0, 1, tape), now - 3.0);
    putCard (dir, hexId (5), cardText (hexId (5), "Key", "Bass", 150.0, 1, {}), now);
    putCard (dir, hexId (6), cardText (hexId (6), "Tape Chorus", "Backing Vox", 250.0, 1, ropeParams()), now);
    putCard (dir, hexId (7), cardText (hexId (7), "Duo", "Backing Vox", 260.0, 1, punchParams()), now);
    putCard (dir, hexId (8), cardText (hexId (8), "Throat", "", 500.0, 1, punchParams()), now);
    Checks c;
    {
        Window win ("Stereo Out");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            const auto tracksImg = paintAt (*w, 1.0f);
            c.need (w->trackRows().size() == 4, "four rows on Tracks: " + juce::String ((int) w->trackRows().size()));
            win.openTrack ("Lead Vox");
            w->tickNow (now);
            c.need (w->shownNames().size() == 4, "four plug-ins on Lead Vox");
            const auto img = paintAt (*w, 1.0f);
            for (const auto* shot : { &tracksImg, &img })
            {
                std::set<juce::uint32> colours;
                for (int y = 0; y < shot->getHeight(); y += 3)
                    for (int x = 0; x < shot->getWidth(); x += 3)
                        colours.insert (shot->getPixelAt (x, y).getARGB());
                c.need (shot->getPixelAt (2, 2).getAlpha() == 255 && shot->getPixelAt (shot->getWidth() - 3, shot->getHeight() - 3).getAlpha() == 255, "opaque edge to edge");
                c.need (colours.size() > 40, "the rows, cards and controls paint: " + juce::String ((int) colours.size()) + " colours");
            }
            c.need (w->getWidth() >= 760 && w->getHeight() >= 480, "opens at a size where the text reads");
            if (auto* cons = w->getConstrainer())
                c.need (cons->getMinimumWidth() >= 480 && cons->getMinimumHeight() >= 340 && w->isResizable(), "resizable with a sensible minimum");
            else
                c.need (false, "resizable");
            // SWITCHBOARD_RENDER_DIR=<absolute folder>: switchboard-1.1-tracks.png, -track.png and -controls.png there (2x), and the
            // smallest size and the empty Tracks screen next to them.
            if (const char* folder = std::getenv ("SWITCHBOARD_RENDER_DIR"); folder != nullptr && folder[0] == '/')
            {
                const juce::File out (folder);
                const auto shot = [&] (const char* tag) { saveImage (paintAt (*w, 2.0f), out.getChildFile (juce::String ("switchboard-1.1-") + tag + ".png")); };
                w->show (hub::Screen::tracks());
                shot ("tracks");
                w->show (hub::Screen::onTrack ("Lead Vox"));
                shot ("track");
                w->show (hub::Screen::controlsOf ("Lead Vox", "Tune Pro"));
                shot ("controls");
                w->setSize (w->getConstrainer()->getMinimumWidth(), w->getConstrainer()->getMinimumHeight());
                w->show (hub::Screen::onTrack ("Lead Vox"));
                shot ("track-smallest");
                w->show (hub::Screen::tracks());
                shot ("tracks-smallest");
                w->setSize (880, 580);
            }
        }
    }
    if (const char* folder = std::getenv ("SWITCHBOARD_RENDER_DIR"); folder != nullptr && folder[0] == '/')
    {
        useFolder ("window_render_empty");
        Window empty ("Stereo Out");
        if (empty.w != nullptr)
        {
            empty.w->tickNow (unixNow());
            saveImage (paintAt (*empty.w, 2.0f), juce::File (folder).getChildFile ("switchboard-1.1-empty.png"));
        }
    }
    check (c.ok, "window_render: Tracks and a track's plug-ins paint edge to edge, resizable with a minimum size");
}

// ---- 1.1.0: the screens (Tracks, a track, its controls), kept by the processor, and the hub inbox ----

void click (juce::Component* comp)
{
    if (comp == nullptr)
        return;
    const auto centre = comp->getLocalBounds().getCentre().toFloat();
    comp->mouseDown (mouseAt (*comp, centre, 1, false, centre));
    comp->mouseUp (mouseAt (*comp, centre, 1, false, centre));
}

bool shownOnScreen (juce::Component* comp) { return comp != nullptr && comp->isVisible() && comp->getParentComponent() != nullptr; }

// What the hub file's "showing" (or a done entry's) says, in hub::Screen::describe's words.
juce::String showingText (const juce::var& s)
{
    if (! s.isObject())
        return "(missing)";
    const auto screen = s["screen"].toString();
    const auto t = s["track"].isVoid() ? juce::String ("(no name)") : s["track"].toString();
    if (screen == "track")
        return "track:" + t;
    if (screen == "controls")
        return "controls:" + t + "/" + s["plugin"].toString();
    return screen;
}

juce::String hubShowing (SwitchboardProcessor& p) { return showingText (readJson (p.hub.file())["showing"]); }

juce::var hubDone (SwitchboardProcessor& p, const juce::String& cmd)
{
    const auto file = readJson (p.hub.file());   // held here: the done list lives inside it
    if (const auto* done = file["done"].getArray())
        for (const auto& d : *done)
            if (d["cmd"].toString() == cmd)
                return d;
    return {};
}

juce::StringArray hubDoneNames (SwitchboardProcessor& p)
{
    juce::StringArray out;
    const auto file = readJson (p.hub.file());
    if (const auto* done = file["done"].getArray())
        for (const auto& d : *done)
            out.add (d["cmd"].toString());
    return out;
}

// A command in the hub inbox as a writer puts it there: .<name>.tmp, then renamed to <name>.json.
juce::String hubCommand (SwitchboardProcessor& p, const juce::String& body, juce::int64 ms = 0)
{
    if (ms == 0)
        ms = (juce::int64) std::floor (unixNow() * 1000.0);
    const auto name = juce::String (ms) + "-test-" + smooth::linkdetail::randomHex (3);
    const auto inbox = p.hub.inbox();
    const auto temp = inbox.getChildFile ("." + name + ".tmp");
    temp.replaceWithText (body);
    std::rename (temp.getFullPathName().toRawUTF8(), inbox.getChildFile (name + ".json").getFullPathName().toRawUTF8());
    return name;
}

int filesIn (const juce::File& folder, const juce::String& pattern = "*")
{
    return folder.getNumberOfChildFiles (juce::File::findFiles, pattern);
}

juce::String rowTitles (const std::vector<SwitchboardEditor::TrackRow>& rows)
{
    juce::StringArray s;
    for (auto& r : rows)
        s.add (r.title);
    return joined (s);
}

void testTracksScreenLoadOrder()
{
    const auto dir = useFolder ("tracks_screen_lists_tracks_in_load_order");
    const double now = unixNow();
    auto tape = punchParams();
    tape[0] = { "drive", "Drive", "4.5 dB", "dB", 0.45, 0.3, 0, {}, true };
    // Load order: Lead Vox (its first plug-in started at 100), Bass (200), Drums (300). By name it would be Bass, Drums, Lead Vox.
    putCard (dir, hexId (1), cardText (hexId (1), "Tune Pro", "Lead Vox", 400.0, 1, tuneParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Velvet Rope", "Lead Vox", 100.0, 1, ropeParams()), now);
    putCard (dir, hexId (3), cardText (hexId (3), "Key", "Bass", 200.0, 1, punchParams()), now);
    putCard (dir, hexId (4), cardText (hexId (4), "Tape Color", "Bass", 250.0, 1, tape), now - 3.0);
    putCard (dir, hexId (5), cardText (hexId (5), "Punch", "Drums", 300.0, 1, punchParams()), now);
    Checks c;
    {
        Window win ("Lead Vox");   // on a normal track it still opens on Tracks
        if (auto* w = win.w)
        {
            w->tickNow (now);
            c.need (win.p.getScreen() == hub::Screen::tracks() && w->screen() == hub::Screen::tracks(), "a new Switchboard opens on Tracks, even on a normal track: "
                    + w->screen().describe());
            const auto rows = w->trackRows();
            c.need (rowTitles (rows) == "[Lead Vox, Bass, Drums]", "tracks in load order: " + rowTitles (rows));
            if (rows.size() == 3)
            {
                c.need (rows[0].plugins == juce::StringArray ({ "Velvet Rope", "Tune Pro" }) && rows[0].note.isEmpty(), "a row lists its plug-ins in load order: "
                        + joined (rows[0].plugins));
                c.need (rows[1].plugins == juce::StringArray ({ "Key", "Tape Color" }) && rows[1].note.contains ("Tape Color") && rows[1].note.contains ("not responding"),
                        "a quiet plug-in gets a note on its row: " + rows[1].note);
                c.need (rows[2].plugins == juce::StringArray ({ "Punch" }) && rows[2].track == "Drums", "Drums: Punch");
            }
            c.need (w->titleText() == "Switchboard" && ! shownOnScreen (w->backButton()), "Tracks is home: the name on top, no back button");
            c.need (shownOnScreen (w->trackRow ("Bass")) && w->trackRow ("Nope") == nullptr, "each row is on screen");
            win.p.pollNow (now);
            c.need (hubShowing (win.p) == "tracks", "the hub file says tracks: " + hubShowing (win.p));

            putCard (dir, hexId (6), cardText (hexId (6), "Duo", "Choir", 50.0, 1, punchParams()), now + 0.2);
            w->tickNow (now + 0.2);
            c.need (rowTitles (w->trackRows()) == "[Choir, Lead Vox, Bass, Drums]", "a track whose plug-in started first goes first: " + rowTitles (w->trackRows()));
            dir.getChildFile (hexId (5) + ".json").deleteFile();
            w->tickNow (now + 0.3);
            c.need (rowTitles (w->trackRows()) == "[Choir, Lead Vox, Bass]", "a track with nothing open goes: " + rowTitles (w->trackRows()));
        }
    }
    {
        useFolder ("tracks_screen_empty");
        Window win ("Stereo Out");
        if (win.w != nullptr)
        {
            win.w->tickNow (unixNow());
            c.need (win.w->trackRows().empty() && win.w->emptyText().contains ("None of your plug-ins are open in this project yet"),
                    "nothing open says so in plain words: " + win.w->emptyText());
        }
    }
    check (c.ok, "tracks_screen_lists_tracks_in_load_order: one row per track with its plug-ins, earliest started first, quiet ones noted");
}

void testUnnamedTrackRow()
{
    const auto dir = useFolder ("unnamed_track_row");
    const double now = unixNow();
    putCard (dir, hexId (1), cardText (hexId (1), "Tune Pro", "Lead Vox", 100.0, 1, tuneParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Key", "", 50.0, 1, punchParams()), now);
    putCard (dir, hexId (3), cardText (hexId (3), "Punch", "", 300.0, 1, punchParams()), now);
    Checks c;
    {
        Window win ("Stereo Out");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            const auto rows = w->trackRows();
            c.need (rowTitles (rows) == "[Lead Vox, No track name yet]", "one row for the plug-ins with no track name, last: " + rowTitles (rows));
            if (rows.size() == 2)
                c.need (rows[1].track.isEmpty() && rows[1].plugins == juce::StringArray ({ "Key", "Punch" }), "it holds both, in load order: " + joined (rows[1].plugins));
            click (w->trackRow (""));
            c.need (w->screen() == hub::Screen::onTrack ("") && w->titleText() == "No track name yet", "a tap opens it: " + w->screen().describe() + ", " + w->titleText());
            c.need (w->shownNames() == juce::StringArray ({ "Key", "Punch" }), "with its plug-ins: " + joined (w->shownNames()));
            c.need (hubShowing (win.p) == "track:(no name)" && readJson (win.p.hub.file())["showing"]["track"].isVoid(), "the hub file says track null: " + hubShowing (win.p));
        }
    }
    check (c.ok, "unnamed_track_row: plug-ins Logic has not named a track for share one row, \"No track name yet\"");
}

void testTrackScreenBackToTracks()
{
    const auto dir = useFolder ("track_screen_back_to_tracks");
    const double now = unixNow();
    putCard (dir, hexId (1), cardText (hexId (1), "Tune Pro", "Lead Vox", 200.0, 1, tuneParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Velvet Rope", "Lead Vox", 100.0, 1, ropeParams()), now);
    putCard (dir, hexId (3), cardText (hexId (3), "Punch", "Bass", 300.0, 1, punchParams()), now);
    Checks c;
    {
        Window win ("Stereo Out");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            click (w->trackRow ("Lead Vox"));
            c.need (w->screen() == hub::Screen::onTrack ("Lead Vox") && win.p.getScreen() == hub::Screen::onTrack ("Lead Vox"), "a tap on a row opens its track: " + w->screen().describe());
            c.need (w->titleText() == "Lead Vox", "the track name is the title: " + w->titleText());
            auto* back = w->backButton();
            c.need (shownOnScreen (back) && w->backText() == juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xb9 Tracks")), "a back button \"< Tracks\": " + w->backText());
            c.need (back != nullptr && back->getX() < 40 && back->getY() < 30 && back->getWidth() >= 80 && back->getHeight() >= 30, "at top left, big enough to hit");
            c.need (w->shownNames() == juce::StringArray ({ "Velvet Rope", "Tune Pro" }) && w->findControl (hexId (1), "retune") != nullptr,
                    "the track's plug-ins with their main controls: " + joined (w->shownNames()));
            c.need (hubShowing (win.p) == "track:Lead Vox", "the hub file says so at once: " + hubShowing (win.p));
            click (back);
            c.need (w->screen() == hub::Screen::tracks() && rowTitles (w->trackRows()) == "[Lead Vox, Bass]" && ! shownOnScreen (w->backButton()),
                    "back shows Tracks again: " + w->screen().describe());
            c.need (w->shownNames().isEmpty() && hubShowing (win.p) == "tracks", "the plug-in lines are gone, the hub file says tracks");
            click (w->trackRow ("Bass"));
            c.need (w->screen() == hub::Screen::onTrack ("Bass") && w->shownNames() == juce::StringArray ({ "Punch" }), "another row, another track");
        }
    }
    check (c.ok, "track_screen_back_to_tracks: a row opens its track, titled by name, and \"< Tracks\" at top left goes back");
}

void testControlsScreenBackToTrack()
{
    const auto dir = useFolder ("controls_screen_back_to_track");
    const double now = unixNow();
    const auto tune = hexId (1), rope = hexId (2);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Lead Vox", 200.0, 1, tuneParams()), now);
    putCard (dir, rope, cardText (rope, "Velvet Rope", "Lead Vox", 100.0, 1, ropeParams()), now);
    Checks c;
    {
        Window win ("Stereo Out");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            win.openTrack ("Lead Vox");
            click (w->controlsLink (tune));
            c.need (w->screen() == hub::Screen::controlsOf ("Lead Vox", "Tune Pro"), "\"All 5 controls\" opens the controls screen: " + w->screen().describe());
            c.need (w->titleText() == "Tune Pro" && w->backText() == juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xb9 Lead Vox")) && shownOnScreen (w->backButton()),
                    "titled by the plug-in, back button \"< Lead Vox\": " + w->titleText() + ", " + w->backText());
            c.need (w->allControlsRows() == 5 && w->shownNames().isEmpty(), "every control of that plug-in, nothing else: " + juce::String (w->allControlsRows()));
            c.need (hubShowing (win.p) == "controls:Lead Vox/Tune Pro", "the hub file says so: " + hubShowing (win.p));
            w->searchAllControls ("mix");
            c.need (w->allControlsRows() == 1, "the search still works");
            click (w->backButton());
            c.need (w->screen() == hub::Screen::onTrack ("Lead Vox") && w->allControlsRows() < 0 && w->shownNames() == juce::StringArray ({ "Velvet Rope", "Tune Pro" }),
                    "back shows the track again: " + w->screen().describe());
            click (w->controlsLink (rope));
            c.need (w->screen() == hub::Screen::controlsOf ("Lead Vox", "Velvet Rope") && w->allControlsRows() == 4, "another plug-in's controls, the search empty again");
            if (auto* search = w->searchBox())
                search->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            c.need (w->searchBox() != nullptr && w->screen() == hub::Screen::onTrack ("Lead Vox"), "Escape in the empty search goes back too");
            click (w->backButton());
            c.need (w->screen() == hub::Screen::tracks(), "and back again is Tracks");
        }
    }
    check (c.ok, "controls_screen_back_to_track: \"All N controls\" opens one plug-in's controls and \"< track\" goes back");
}

void testScreenSurvivesReopen()
{
    const auto dir = useFolder ("screen_survives_editor_close_and_reopen");
    const double now = unixNow();
    putCard (dir, hexId (1), cardText (hexId (1), "Tune Pro", "Lead Vox", 100.0, 1, tuneParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Punch", "Bass", 200.0, 1, punchParams()), now);
    Checks c;
    {
        Window win ("Stereo Out");
        if (win.w != nullptr)
        {
            win.w->tickNow (now);
            win.w->show (hub::Screen::controlsOf ("Lead Vox", "Tune Pro"));
        }
        win.p.editorBeingDeleted (win.ed.get());
        win.ed.reset();
        c.need (win.p.getScreen() == hub::Screen::controlsOf ("Lead Vox", "Tune Pro"), "with the window closed the plug-in keeps the screen: " + win.p.getScreen().describe());
        win.ed.reset (win.p.createEditorIfNeeded());
        win.w = dynamic_cast<SwitchboardEditor*> (win.ed.get());
        if (win.w != nullptr)
        {
            win.w->tickNow (unixNow());
            c.need (win.w->screen() == hub::Screen::controlsOf ("Lead Vox", "Tune Pro") && win.w->allControlsRows() == 5 && win.w->titleText() == "Tune Pro",
                    "opened again it shows the same controls: " + win.w->screen().describe());
            win.w->show (hub::Screen::onTrack ("Bass"));
        }
        win.reopen();
        if (win.w != nullptr)
        {
            win.w->tickNow (unixNow());
            c.need (win.w->screen() == hub::Screen::onTrack ("Bass") && win.w->shownNames() == juce::StringArray ({ "Punch" }), "the same track: " + win.w->screen().describe());
            win.w->show (hub::Screen::tracks());
        }
        win.reopen();
        if (win.w != nullptr)
        {
            win.w->tickNow (unixNow());
            c.need (win.w->screen() == hub::Screen::tracks() && win.w->trackRows().size() == 2, "and Tracks");
        }
    }
    check (c.ok, "screen_survives_editor_close_and_reopen: the plug-in keeps the screen, the window comes back to it");
}

void testScreenSavedWithSession()
{
    const auto dir = useFolder ("screen_saved_with_session_and_restored");
    const double now = unixNow();
    putCard (dir, hexId (1), cardText (hexId (1), "Tune Pro", "Lead Vox", 100.0, 1, tuneParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Tune Pro", "Lead Vox", 200.0, 1, [] { auto ps = tuneParams(); ps.pop_back(); return ps; }()), now);
    putCard (dir, hexId (3), cardText (hexId (3), "Punch", "Bass", 300.0, 1, punchParams()), now);
    Checks c;
    const auto saveWith = [&] (const hub::Screen& s)
    {
        juce::MemoryBlock state;
        Window win ("Stereo Out");
        if (win.w != nullptr)
        {
            win.w->tickNow (unixNow());
            win.w->show (s);
        }
        win.p.getStateInformation (state);
        return state;
    };
    for (const auto& s : { hub::Screen::controlsOf ("Lead Vox", "Tune Pro 2"), hub::Screen::onTrack ("Bass"), hub::Screen::tracks() })
    {
        const auto state = saveWith (s);
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        c.need (xml != nullptr && xml->getStringAttribute ("screen") == s.name() && ! xml->hasAttribute ("view")
                    && xml->getStringAttribute ("version") == SWITCHBOARD_VERSION, s.describe() + ": saved as screen=\"" + s.name() + "\", no view");
        SwitchboardProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        c.need (restored.getScreen() == s, s.describe() + ": restored as " + restored.getScreen().describe());
        restored.pollNow (unixNow());
        c.need (hubShowing (restored) == s.describe(), s.describe() + ": its hub file says so: " + hubShowing (restored));
        std::unique_ptr<juce::AudioProcessorEditor> ed (restored.createEditorIfNeeded());
        if (auto* w = dynamic_cast<SwitchboardEditor*> (ed.get()))
        {
            w->tickNow (unixNow());
            c.need (w->screen() == s, s.describe() + ": its window opens on it: " + w->screen().describe());
            if (s.kind == hub::Screen::Kind::controls)
                c.need (w->allControlsRows() == 4 && w->titleText() == "Tune Pro 2", "the second Tune Pro's 4 controls, not the first one's 5: " + juce::String (w->allControlsRows()));
            if (s.kind == hub::Screen::Kind::track)
                c.need (w->shownNames() == juce::StringArray ({ "Punch" }), "Bass with Punch");
        }
        restored.editorBeingDeleted (ed.get());
    }
    {
        SwitchboardProcessor p;
        const auto state = saveWith (hub::Screen::onTrack ("Bass"));
        p.setStateInformation (state.getData(), (int) state.getSize());
        for (const char* bad : { "mixer", "" })
        {
            juce::XmlElement x ("SWITCHBOARD");
            x.setAttribute ("version", SWITCHBOARD_VERSION);
            x.setAttribute ("screen", bad);
            x.setAttribute ("track", "Lead Vox");
            juce::MemoryBlock b;
            juce::AudioProcessor::copyXmlToBinary (x, b);
            p.setStateInformation (b.getData(), (int) b.getSize());
        }
        juce::XmlElement noPlugin ("SWITCHBOARD");
        noPlugin.setAttribute ("screen", "controls");
        noPlugin.setAttribute ("track", "Lead Vox");
        juce::MemoryBlock b;
        juce::AudioProcessor::copyXmlToBinary (noPlugin, b);
        p.setStateInformation (b.getData(), (int) b.getSize());
        p.setStateInformation ("garbage", 7);
        c.need (p.getScreen() == hub::Screen::onTrack ("Bass"), "a screen it does not know, or a broken state, is ignored: " + p.getScreen().describe());
    }
    check (c.ok, "screen_saved_with_session_and_restored: Tracks, a track or a plug-in's controls come back with the project");
}

void testOldSessionViewLoads()
{
    const auto dir = useFolder ("old_1_0_session_view_loads");
    const double now = unixNow();
    putCard (dir, hexId (1), cardText (hexId (1), "Tune Pro", "Lead Vox", 100.0, 1, tuneParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Punch", "Bass", 200.0, 1, punchParams()), now);
    putCard (dir, hexId (3), cardText (hexId (3), "Key", "", 300.0, 1, punchParams()), now);
    Checks c;
    const auto oldState = [] (const juce::String& view)
    {
        juce::XmlElement x ("SWITCHBOARD");   // exactly what 1.0.0 saved
        x.setAttribute ("version", "1.0.0");
        x.setAttribute ("view", view);
        juce::MemoryBlock b;
        juce::AudioProcessor::copyXmlToBinary (x, b);
        return b;
    };
    const std::vector<std::pair<const char*, hub::Screen>> cases { { "this", hub::Screen::tracks() }, { "all", hub::Screen::tracks() },
                                                                   { "track:Bass", hub::Screen::onTrack ("Bass") }, { "track:", hub::Screen::onTrack ("") } };
    for (const auto& [view, expected] : cases)
    {
        SwitchboardProcessor p;
        p.setScreen (hub::Screen::onTrack ("Lead Vox"));
        const auto b = oldState (view);
        p.setStateInformation (b.getData(), (int) b.getSize());
        c.need (p.getScreen() == expected, juce::String ("1.0.0 view \"") + view + "\" opens " + expected.describe() + ", got " + p.getScreen().describe());
        if (juce::String (view) == "track:Bass")
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorIfNeeded());
            if (auto* w = dynamic_cast<SwitchboardEditor*> (ed.get()))
            {
                w->tickNow (unixNow());
                c.need (w->screen() == hub::Screen::onTrack ("Bass") && w->shownNames() == juce::StringArray ({ "Punch" }), "and its window shows Bass with Punch");
            }
            p.editorBeingDeleted (ed.get());
        }
    }
    {
        SwitchboardProcessor p;
        p.setScreen (hub::Screen::onTrack ("Lead Vox"));
        const auto b = oldState ("nonsense");
        p.setStateInformation (b.getData(), (int) b.getSize());
        c.need (p.getScreen() == hub::Screen::onTrack ("Lead Vox"), "a 1.0.0 view it does not know is ignored");
    }
    check (c.ok, "old_1_0_session_view_loads: 1.0.0's This track and All tracks open on Tracks, a chosen track opens that track");
}

void testRememberedTrackGone()
{
    const auto dir = useFolder ("remembered_track_gone_falls_back_to_tracks");
    const double t0 = unixNow();
    const auto tune = hexId (1), punch = hexId (2), rope = hexId (3);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Lead Vox", 100.0, 1, tuneParams()), t0);
    putCard (dir, punch, cardText (punch, "Punch", "Bass", 200.0, 1, punchParams()), t0);
    Checks c;
    SwitchboardProcessor p;
    const auto at = [&] (double t)
    {
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "????????????????.json"))
            setMtime (f, t);
        p.pollNow (t);
    };
    {
        juce::XmlElement x ("SWITCHBOARD");
        x.setAttribute ("screen", "track");
        x.setAttribute ("track", "Drums");
        juce::MemoryBlock b;
        juce::AudioProcessor::copyXmlToBinary (x, b);
        p.setStateInformation (b.getData(), (int) b.getSize());
    }
    at (t0);
    at (t0 + 2.0);
    c.need (p.getScreen() == hub::Screen::onTrack ("Drums"), "just after a project loads, a track not seen yet is kept (its plug-ins may still be opening): " + p.getScreen().describe());
    double t = t0 + 2.0;
    while (t < t0 + 16.0 && p.getScreen() != hub::Screen::tracks())
        at (t += 0.5);
    c.need (p.getScreen() == hub::Screen::tracks() && t >= t0 + 10.0, "once it has looked long enough, a track that is not there falls back to Tracks, after "
            + juce::String (t - t0, 1) + " s");
    c.need (hubShowing (p) == "tracks", "the hub file says tracks: " + hubShowing (p));

    // A track renamed while it is shown: its plug-ins move to the new name and the screen goes with them.
    p.setScreen (hub::Screen::onTrack ("Lead Vox"));
    at (t += 0.5);
    putCard (dir, tune, cardText (tune, "Tune Pro", "Lead Vox Double", 100.0, 2, tuneParams()), t);
    at (t += 0.5);
    c.need (p.getScreen() == hub::Screen::onTrack ("Lead Vox Double"), "a renamed track is followed, not dropped: " + p.getScreen().describe());

    // A blip: Logic held its main thread, every card looked old for one look. Nothing changes.
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "????????????????.json"))
        setMtime (f, t - 8.0);
    p.pollNow (t += 0.5);
    at (t += 0.5);
    at (t += 0.5);
    c.need (p.getScreen() == hub::Screen::onTrack ("Lead Vox Double"), "one look with every card old does not drop the screen: " + p.getScreen().describe());

    // The plug-in on a controls screen closes: back to its track while the track has others, then to Tracks.
    putCard (dir, rope, cardText (rope, "Velvet Rope", "Lead Vox Double", 300.0, 1, ropeParams()), t);
    at (t += 0.5);
    p.setScreen (hub::Screen::controlsOf ("Lead Vox Double", "Tune Pro"));
    dir.getChildFile (tune + ".json").deleteFile();
    at (t += 0.5);
    at (t += 1.0);
    c.need (p.getScreen() == hub::Screen::controlsOf ("Lead Vox Double", "Tune Pro"), "a plug-in gone for a moment is waited for: " + p.getScreen().describe());
    for (int k = 0; k < 8 && p.getScreen().kind == hub::Screen::Kind::controls; ++k)
        at (t += 0.5);
    c.need (p.getScreen() == hub::Screen::onTrack ("Lead Vox Double"), "a plug-in that stays gone falls back to its track: " + p.getScreen().describe());
    dir.getChildFile (rope + ".json").deleteFile();
    for (int k = 0; k < 10 && p.getScreen().kind != hub::Screen::Kind::tracks; ++k)
        at (t += 0.5);
    c.need (p.getScreen() == hub::Screen::tracks() && hubShowing (p) == "tracks", "and a track that stays empty to Tracks: " + p.getScreen().describe());

    // The window follows the fallback.
    putCard (dir, rope, cardText (rope, "Velvet Rope", "Vox", 300.0, 2, ropeParams()), t);
    at (t += 0.5);
    p.setScreen (hub::Screen::onTrack ("Vox"));
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorIfNeeded());
    if (auto* w = dynamic_cast<SwitchboardEditor*> (ed.get()))
    {
        w->clock = [&t] { return t; };
        w->tickNow (t);
        c.need (w->screen() == hub::Screen::onTrack ("Vox"), "the window shows Vox");
        dir.getChildFile (rope + ".json").deleteFile();
        for (int k = 0; k < 10 && w->screen().kind != hub::Screen::Kind::tracks; ++k)
        {
            t += 0.5;
            for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "????????????????.json"))
                setMtime (f, t);
            w->tickNow (t);
        }
        c.need (w->screen() == hub::Screen::tracks() && p.getScreen() == hub::Screen::tracks(), "an open window falls back with it: " + w->screen().describe());
    }
    p.editorBeingDeleted (ed.get());
    check (c.ok, "remembered_track_gone_falls_back_to_tracks: a gone track falls back to Tracks (a gone plug-in to its track), a renamed one is followed");
}

// A window on Tracks, cards on Lead Vox (Velvet Rope, Tune Pro, Tune Pro 2) and Bass (Punch), for the hub command tests.
juce::File commandFolder (const char* name, double now)
{
    const auto dir = useFolder (name);
    putCard (dir, hexId (1), cardText (hexId (1), "Velvet Rope", "Lead Vox", 100.0, 1, ropeParams()), now);
    putCard (dir, hexId (2), cardText (hexId (2), "Tune Pro", "Lead Vox", 200.0, 1, tuneParams()), now);
    putCard (dir, hexId (3), cardText (hexId (3), "Tune Pro", "Lead Vox", 300.0, 1, [] { auto ps = tuneParams(); ps.pop_back(); return ps; }()), now);
    putCard (dir, hexId (4), cardText (hexId (4), "Punch", "Bass", 400.0, 1, punchParams()), now);
    return dir;
}

void testHubCmdShowTrack()
{
    const double now = unixNow();
    commandFolder ("hub_cmd_show_track", now);
    Checks c;
    {
        Window win ("Stereo Out");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            c.need (win.p.hub.inbox().isDirectory() && win.p.hub.inbox().getFileName() == "hub-" + win.p.hub.id() + ".in", "the hub inbox is hub-<id>.in");
            const auto name = hubCommand (win.p, "{\"show\":\"track\",\"track\":\"Lead Vox\",\"from\":\"test\"}");
            win.p.pollNow (now);
            c.need (win.p.getScreen() == hub::Screen::onTrack ("Lead Vox"), "the command changes the screen: " + win.p.getScreen().describe());
            c.need (filesIn (win.p.hub.inbox()) == 0, "the command file is deleted");
            const auto d = hubDone (win.p, name);
            c.need (d.isObject() && d["ok"].isBool() && (bool) d["ok"] && d["error"].toString().isEmpty() && showingText (d["showing"]) == "track:Lead Vox",
                    "done: ok, no error, showing the track: " + juce::JSON::toString (d, true));
            c.need (hubShowing (win.p) == "track:Lead Vox", "the hub file says so: " + hubShowing (win.p));
            w->tickNow (now + 0.1);
            c.need (w->screen() == hub::Screen::onTrack ("Lead Vox") && w->titleText() == "Lead Vox" && w->shownNames() == juce::StringArray ({ "Velvet Rope", "Tune Pro", "Tune Pro 2" }),
                    "the window follows: " + w->screen().describe() + " " + joined (w->shownNames()));
            hubCommand (win.p, "{\"show\":\"track\",\"track\":\"Bass\"}");
            win.p.pollNow (now + 0.2);
            w->tickNow (now + 0.2);
            c.need (w->screen() == hub::Screen::onTrack ("Bass") && w->shownNames() == juce::StringArray ({ "Punch" }), "\"from\" is optional; another track");
        }
    }
    check (c.ok, "hub_cmd_show_track: { \"show\": \"track\" } opens a track, done says ok and showing, the window follows");
}

void testHubCmdShowControls()
{
    const double now = unixNow();
    commandFolder ("hub_cmd_show_controls", now);
    Checks c;
    {
        Window win ("Stereo Out");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            const auto name = hubCommand (win.p, "{\"show\":\"controls\",\"track\":\"Lead Vox\",\"plugin\":\"Tune Pro 2\",\"from\":\"test\"}");
            win.p.pollNow (now);
            c.need (win.p.getScreen() == hub::Screen::controlsOf ("Lead Vox", "Tune Pro 2"), "the controls of Tune Pro 2: " + win.p.getScreen().describe());
            const auto d = hubDone (win.p, name);
            c.need ((bool) d["ok"] && showingText (d["showing"]) == "controls:Lead Vox/Tune Pro 2" && hubShowing (win.p) == "controls:Lead Vox/Tune Pro 2",
                    "done and the hub file say controls, with track and plugin: " + hubShowing (win.p));
            w->tickNow (now + 0.1);
            c.need (w->screen() == hub::Screen::controlsOf ("Lead Vox", "Tune Pro 2") && w->allControlsRows() == 4 && w->titleText() == "Tune Pro 2",
                    "the window shows the second Tune Pro's 4 controls: " + juce::String (w->allControlsRows()));
            c.need (w->backText() == juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xb9 Lead Vox")), "with its back button");
            hubCommand (win.p, "{\"show\":\"controls\",\"track\":\"Lead Vox\",\"plugin\":\"Tune Pro\"}");
            win.p.pollNow (now + 0.2);
            w->tickNow (now + 0.2);
            c.need (w->allControlsRows() == 5 && w->titleText() == "Tune Pro", "and the first one's 5");
        }
    }
    check (c.ok, "hub_cmd_show_controls: { \"show\": \"controls\" } opens one plug-in's controls, numbered as Switchboard numbers copies");
}

void testHubCmdShowTracks()
{
    const double now = unixNow();
    commandFolder ("hub_cmd_show_tracks", now);
    Checks c;
    {
        Window win ("Stereo Out");
        win.openTrack ("Bass");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            c.need (w->screen() == hub::Screen::onTrack ("Bass"), "on Bass first");
            const auto name = hubCommand (win.p, "{\"show\":\"tracks\",\"from\":\"test\"}");
            win.p.pollNow (now);
            const auto d = hubDone (win.p, name);
            c.need (win.p.getScreen() == hub::Screen::tracks() && (bool) d["ok"] && showingText (d["showing"]) == "tracks" && hubShowing (win.p) == "tracks",
                    "back to Tracks, done ok: " + hubShowing (win.p));
            w->tickNow (now + 0.1);
            c.need (w->screen() == hub::Screen::tracks() && rowTitles (w->trackRows()) == "[Lead Vox, Bass]" && ! shownOnScreen (w->backButton()), "the window follows");
        }
    }
    check (c.ok, "hub_cmd_show_tracks: { \"show\": \"tracks\" } goes home");
}

void testHubCmdUnknownTrack()
{
    const double now = unixNow();
    const auto dir = commandFolder ("hub_cmd_unknown_track_ok_false_screen_kept", now);
    putCard (dir, hexId (5), cardText (hexId (5), "Duo", "Old Track", 500.0, 1, punchParams()), now - 6.0);   // hidden: no heartbeat for 6 s
    Checks c;
    {
        Window win ("Stereo Out");
        win.openTrack ("Lead Vox");
        if (auto* w = win.w)
        {
            w->tickNow (now);
            const std::vector<std::pair<juce::String, juce::String>> bad {
                { "{\"show\":\"track\",\"track\":\"Nope\"}", "Nope" },
                { "{\"show\":\"track\",\"track\":\"Old Track\"}", "Old Track" },
                { "{\"show\":\"controls\",\"track\":\"Lead Vox\",\"plugin\":\"Tune Pro 3\"}", "Tune Pro 3" },
                { "{\"show\":\"controls\",\"track\":\"Nope\",\"plugin\":\"Tune Pro\"}", "Nope" },
                { "{\"show\":\"controls\",\"track\":\"Lead Vox\"}", "plugin" },
                { "{\"show\":\"track\"}", "track" },
                { "{\"show\":\"mixer\"}", "mixer" },
                { "{\"track\":\"Lead Vox\"}", "show" },
                { "{\"show\":3}", "show" } };
            juce::int64 ms = (juce::int64) std::floor (now * 1000.0);
            juce::StringArray names;
            for (auto& b : bad)
                names.add (hubCommand (win.p, b.first, ++ms));
            win.p.pollNow (now);
            for (int i = 0; i < names.size(); ++i)
            {
                const auto d = hubDone (win.p, names[i]);
                c.need (d.isObject() && d["ok"].isBool() && ! (bool) d["ok"] && d["error"].toString().containsIgnoreCase (bad[(size_t) i].second)
                            && showingText (d["showing"]) == "track:Lead Vox",
                        bad[(size_t) i].first + ": ok false, a plain error, the screen as it stayed: " + d["error"].toString());
            }
            c.need (win.p.getScreen() == hub::Screen::onTrack ("Lead Vox") && hubShowing (win.p) == "track:Lead Vox", "the screen stays: " + win.p.getScreen().describe());
            c.need (filesIn (win.p.hub.inbox()) == 0, "every command file is deleted");
            w->tickNow (now + 0.1);
            c.need (w->screen() == hub::Screen::onTrack ("Lead Vox") && w->shownNames().size() == 3, "the window stays on Lead Vox");
        }
    }
    check (c.ok, "hub_cmd_unknown_track_ok_false_screen_kept: a track or plug-in not seen now, or an unknown show, is ok false with a plain error");
}

void testHubCmdBadJson()
{
    const double now = unixNow();
    commandFolder ("hub_cmd_bad_json_ok_false", now);
    Checks c;
    {
        SwitchboardProcessor p;
        p.setScreen (hub::Screen::onTrack ("Bass"));
        const juce::StringArray bad { "{not json", "[1,2,3]", "", "\"show\"", "{\"show\":\"tracks\"" };
        juce::int64 ms = (juce::int64) std::floor (now * 1000.0);
        juce::StringArray names;
        for (auto& b : bad)
            names.add (hubCommand (p, b, ++ms));
        const auto inbox = p.hub.inbox();
        inbox.getChildFile (".later.tmp").replaceWithText ("{\"show\":\"tracks\"}");
        inbox.getChildFile (".hidden.json").replaceWithText ("{\"show\":\"tracks\"}");
        p.pollNow (now);
        for (int i = 0; i < names.size(); ++i)
        {
            const auto d = hubDone (p, names[i]);
            c.need (d.isObject() && ! (bool) d["ok"] && d["error"].toString().isNotEmpty() && showingText (d["showing"]) == "track:Bass",
                    "\"" + bad[i] + "\": ok false: " + d["error"].toString());
        }
        c.need (hubDone (p, names[0])["error"].toString() == "not valid JSON", "bad JSON says so: " + hubDone (p, names[0])["error"].toString());
        c.need (p.getScreen() == hub::Screen::onTrack ("Bass") && hubShowing (p) == "track:Bass", "the screen stays");
        c.need (filesIn (inbox, "*.json") == 1 && inbox.getChildFile (".hidden.json").existsAsFile() && inbox.getChildFile (".later.tmp").existsAsFile(),
                "the bad files are deleted, files starting with a dot are never read");
    }
    check (c.ok, "hub_cmd_bad_json_ok_false: bad JSON, not an object or an empty file is ok false, deleted, the screen kept");
}

void testHubCmdOrderAndDone16()
{
    const double now = unixNow();
    commandFolder ("hub_cmd_order_and_done_last_16", now);
    Checks c;
    {
        SwitchboardProcessor p;
        const juce::StringArray bodies { "{\"show\":\"track\",\"track\":\"Lead Vox\"}", "{\"show\":\"tracks\"}",
                                         "{\"show\":\"controls\",\"track\":\"Lead Vox\",\"plugin\":\"Tune Pro\"}", "{\"show\":\"track\",\"track\":\"Bass\"}" };
        const juce::int64 ms0 = (juce::int64) std::floor (now * 1000.0);
        // Written newest first: the order applied is the name order, not the order the files appeared.
        juce::StringArray names;
        for (int i = 19; i >= 0; --i)
            names.insert (0, hubCommand (p, bodies[i % 4], ms0 + i));
        p.pollNow (now);
        const auto done = hubDoneNames (p);
        juce::StringArray last16;
        for (int i = 4; i < 20; ++i)
            last16.add (names[i]);
        c.need (done == last16, "done holds the last 16 in name order, newest last");
        c.need (p.getScreen() == hub::Screen::onTrack ("Bass") && hubShowing (p) == "track:Bass", "the screen is the last command's: " + p.getScreen().describe());
        c.need (showingText (hubDone (p, names[18])["showing"]) == "controls:Lead Vox/Tune Pro" && showingText (hubDone (p, names[17])["showing"]) == "tracks",
                "each done entry shows the screen right after it");
        c.need (filesIn (p.hub.inbox()) == 0, "all 20 applied in one poll");

        // 70 at once: 64 in one poll, the other 6 in the next, as a member does.
        names.clear();
        for (int i = 0; i < 70; ++i)
            names.add (hubCommand (p, bodies[i % 4], ms0 + 100 + i));
        p.pollNow (now + 0.05);
        const int left = filesIn (p.hub.inbox(), "*.json");
        const auto afterFirst = hubDoneNames (p);
        p.pollNow (now + 0.1);
        c.need (left == 6 && afterFirst.size() == 16 && afterFirst[15] == names[63], "64 in the first poll: " + juce::String (left) + " left");
        c.need (filesIn (p.hub.inbox()) == 0 && hubDoneNames (p).size() == 16 && hubDoneNames (p)[15] == names[69], "the rest in the next");
        c.need (p.getScreen() == hub::Screen::tracks() && hubShowing (p) == "tracks", "and the screen is the 70th's (tracks, after Bass): " + p.getScreen().describe());
    }
    check (c.ok, "hub_cmd_order_and_done_last_16: commands apply in name order, at most 64 a poll, done keeps the last 16");
}

void testHubInboxPolledWindowClosed()
{
    const double now = unixNow();
    commandFolder ("hub_inbox_polled_with_window_closed", now);
    Checks c;
    juce::File hubFile, inbox;
    double rate = 0.0, answeredIn = -1.0;
    {
        SwitchboardProcessor p;   // no window, ever
        hubFile = p.hub.file();
        inbox = p.hub.inbox();
        c.need (inbox.isDirectory() && hubFile.existsAsFile(), "the hub file and its inbox are made at construction");
        auto* mm = juce::MessageManager::getInstance();
        const auto runFor = [mm] (double seconds)
        {
            const double end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
            while (juce::Time::getMillisecondCounterHiRes() < end)
                mm->runDispatchLoopUntil (juce::jmax (1, juce::jmin (5, (int) (end - juce::Time::getMillisecondCounterHiRes()))));
        };
        runFor (0.2);
        const int polls0 = p.inboxPolls();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        runFor (1.5);
        rate = (p.inboxPolls() - polls0) / ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);

        const auto name = hubCommand (p, "{\"show\":\"track\",\"track\":\"Lead Vox\",\"from\":\"test\"}");
        const double sent = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - sent < 2000.0 && ! hubDone (p, name).isObject())
            runFor (0.005);
        if (hubDone (p, name).isObject())
            answeredIn = (juce::Time::getMillisecondCounterHiRes() - sent) / 1000.0;
        c.need (p.getActiveEditor() == nullptr && p.getScreen() == hub::Screen::onTrack ("Lead Vox") && hubShowing (p) == "track:Lead Vox",
                "with no window the command was applied and the hub file rewritten");
        c.need (answeredIn >= 0.0 && answeredIn < 0.25, "answered within a quarter second: " + juce::String (answeredIn, 3) + " s");
    }
    c.need (! hubFile.exists() && ! inbox.exists(), "closing the plug-in removes the hub file and its inbox");
    info ("hub inbox command answered in", answeredIn, "s");
    check (c.ok && rate >= 10.0, "hub_inbox_polled_with_window_closed: hub inbox polls a second with no window", rate, "polls");
}

void testStaleHubInboxCleaned()
{
    const auto dir = useFolder ("stale_hub_inbox_cleaned_only_after_600s");
    const double now = unixNow();
    dir.createDirectory();
    const auto inbox = [&] (const juce::String& id, double mtime, bool withFile)
    {
        auto f = dir.getChildFile ("hub-" + id + ".in");
        f.createDirectory();
        if (withFile)
            f.getChildFile ("1-test-abcdef.json").replaceWithText ("{\"show\":\"tracks\"}");
        setMtime (f, mtime);
        return f;
    };
    const auto hubFile = [&] (const juce::String& id, double mtime)
    {
        auto f = dir.getChildFile ("hub-" + id + ".json");
        f.replaceWithText ("{\"link\":1,\"hub\":true,\"id\":\"" + id + "\",\"track\":null,\"version\":\"1.1.0\"}");
        setMtime (f, mtime);
        return f;
    };
    const auto orphan599 = inbox (hexId (1), now - 599.0, false);
    const auto orphan700 = inbox (hexId (2), now - 700.0, true);
    hubFile (hexId (3), now);
    const auto quietLive = inbox (hexId (3), now - 700.0, false);   // its Switchboard is open, nothing was sent for a while
    hubFile (hexId (4), now - 700.0);
    const auto deadHub = inbox (hexId (4), now - 5.0, true);       // its Switchboard stopped beating 700 s ago
    const auto oddName = dir.getChildFile ("hub-xyz.in");
    oddName.createDirectory();
    setMtime (oddName, now - 700.0);
    const auto member = hexId (5);
    putCard (dir, member, cardText (member, "Tune Pro", "Vox", 1.0, 1, tuneParams()), now);
    setMtime (dir.getChildFile (member + ".in"), now - 700.0);

    Checks c;
    hub::Registry r (dir);
    r.scan (now);
    c.need (orphan599.isDirectory(), "an inbox with no hub file, 599 s old: kept");
    c.need (! orphan700.exists(), "an inbox with no hub file, 700 s old: removed with what is in it");
    c.need (quietLive.isDirectory(), "the inbox of a Switchboard that is open: kept, however old");
    c.need (! deadHub.exists(), "the inbox of a hub file older than 600 s: removed");
    c.need (oddName.isDirectory() && dir.getChildFile (member + ".in").isDirectory() && dir.getChildFile (member + ".json").existsAsFile(),
            "other names and members' inboxes are not touched");
    setMtime (orphan599, now - 601.0);
    r.scan (now + 1.0);
    c.need (orphan599.isDirectory(), "the cleanup runs once a minute, not on every scan");
    r.scan (now + 61.0);
    c.need (! orphan599.exists(), "a minute later the 600 s rule takes it");
    check (c.ok, "stale_hub_inbox_cleaned_only_after_600s: hub-*.in goes once its hub file is gone or 600 s old, never younger");
}

// HubTest --window-cpu <seconds> <LinkStub>: six plug-ins on Lead Vox; CPU of this process with the window closed, then open on
// screen on Tracks, on Lead Vox, and on Lead Vox with one control moving.
int windowCpu (double seconds, const juce::String& stubPath)
{
    const auto dir = useFolder ("window_cpu");
    dir.createDirectory();
    const juce::StringArray names { "Tune Pro", "Velvet Rope", "Punch", "Tape Color", "Duo", "Throat" };
    juce::OwnedArray<juce::ChildProcess> stubs;
    for (auto& n : names)
    {
        auto* cp = stubs.add (new juce::ChildProcess());
        if (! cp->start (juce::StringArray { stubPath, "--dir", dir.getFullPathName(), "--track", "Lead Vox", "--plugin", n, "--seconds", juce::String (seconds * 5 + 30) }))
        {
            std::printf ("FAIL  could not start %s\n", stubPath.toRawUTF8());
            return 1;
        }
    }
    const double waitUntil = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (dir.findChildFiles (juce::File::findFiles, false, "????????????????.json").size() < names.size() && juce::Time::getMillisecondCounterHiRes() < waitUntil)
        juce::Thread::sleep (50);

    SwitchboardProcessor p;
    SwitchboardProcessor::TrackProperties props;
    props.name = juce::String ("Stereo Out");
    p.updateTrackProperties (props);

    const auto cpuSeconds = []
    {
        struct rusage u;
        getrusage (RUSAGE_SELF, &u);
        return (double) u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1.0e6 + (double) u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1.0e6;
    };
    auto* mm = juce::MessageManager::getInstance();
    const auto runFor = [mm] (double s)
    {
        const double end = juce::Time::getMillisecondCounterHiRes() + s * 1000.0;
        while (juce::Time::getMillisecondCounterHiRes() < end)
            mm->runDispatchLoopUntil (juce::jmax (1, (int) (end - juce::Time::getMillisecondCounterHiRes())));
    };
    const auto measure = [&] (double s)
    {
        const double c0 = cpuSeconds(), t0 = juce::Time::getMillisecondCounterHiRes();
        runFor (s);
        return (cpuSeconds() - c0) / ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0) * 100.0;
    };

    // The plug-in with its window closed: the hub inbox poll and the heartbeat only.
    runFor (1.0);
    const double closed = measure (seconds);

    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorIfNeeded());
    auto* w = dynamic_cast<SwitchboardEditor*> (ed.get());
    ed->setTopLeftPosition (120, 120);
    ed->addToDesktop (juce::ComponentPeer::windowHasTitleBar | juce::ComponentPeer::windowIsResizable);
    ed->setVisible (true);
    ed->toFront (false);   // on screen, without taking the keyboard from whatever Luke is using

    // A control that moves 8 times a second, as automation does: commands to one stub, whose card then changes.
    struct Mover : juce::Timer
    {
        hub::Commands cmd;
        juce::String id;
        int n = 0;
        explicit Mover (const juce::File& d) : cmd (d) {}
        void timerCallback() override { cmd.setValue (id, "gain", 0.5 + 0.4 * std::sin (n++ * 0.4), hub::Gesture::tap, smooth::linkdetail::unixNow()); }
    } mover (dir);

    runFor (2.0);   // settle: the window opened on Tracks, the cards were read
    info ("window paints while it opened", w != nullptr ? w->paintCount() : 0);
    info ("window on screen", ed->isShowing() ? 1 : 0);
    const int rows = w != nullptr ? (int) w->trackRows().size() : 0;
    int paints0 = w != nullptr ? w->paintCount() : 0;
    const double tracksIdle = measure (seconds);
    const int tracksPaints = (w != nullptr ? w->paintCount() : 0) - paints0;

    if (w != nullptr)
        w->show (hub::Screen::onTrack ("Lead Vox"));
    runFor (1.0);
    const int shown = w != nullptr ? w->shownNames().size() : 0;
    paints0 = w != nullptr ? w->paintCount() : 0;
    const double idle = measure (seconds);
    const int idlePaints = (w != nullptr ? w->paintCount() : 0) - paints0;

    if (w != nullptr && ! w->registry().instances().empty())
        mover.id = w->registry().instances().front()->id;
    mover.startTimerHz (8);
    paints0 = w != nullptr ? w->paintCount() : 0;
    const double moving = measure (seconds);
    const int movingPaints = (w != nullptr ? w->paintCount() : 0) - paints0;
    mover.stopTimer();

    info ("track rows on Tracks", rows);
    info ("plug-ins shown on Lead Vox", shown);
    info ("window paints on Tracks while idle", tracksPaints);
    info ("window paints on a track while idle", idlePaints);
    info ("window paints while a control moves 8 times a second", movingPaints);
    check (rows == 1 && shown == names.size(), "Tracks shows Lead Vox, and Lead Vox shows all six plug-ins");
    check (closed < 1.0, "CPU with the window closed (hub inbox and heartbeat)", closed, "% of one core");
    check (tracksIdle < 5.0, "CPU with the window open on Tracks, nothing moving", tracksIdle, "% of one core");
    check (idle < 5.0, "CPU with the window open on a track, nothing moving", idle, "% of one core");
    check (moving < 5.0, "CPU with the window open on a track, one control moving 8 times a second", moving, "% of one core");
    p.editorBeingDeleted (ed.get());
    ed.reset();
    for (auto* cp : stubs)
        cp->kill();
    return finish();
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const char* dir = std::getenv ("SMOOTH_LINK_DIR");
    const char* key = std::getenv ("SMOOTH_KEY_JSON");
    if (dir == nullptr || dir[0] != '/' || key == nullptr || key[0] != '/')
    {
        std::puts ("FAIL  SMOOTH_LINK_DIR and SMOOTH_KEY_JSON must both hold absolute scratch paths");
        return 1;
    }
    base = juce::File (dir);
    const auto real = juce::File (realHome() + "/Library/Application Support/SmoothPlugins");
    if (base == real || base.isAChildOf (real))
    {
        std::puts ("FAIL  SMOOTH_LINK_DIR points into Luke's real SmoothPlugins folder");
        return 1;
    }
    base.createDirectory();

    if (argc == 4 && std::strcmp (argv[1], "--window-cpu") == 0)
        return windowCpu (juce::String (argv[2]).getDoubleValue(), juce::String::fromUTF8 (argv[3]));

    testPassthrough();
    testPassthroughAnyLayout();
    testMismatchedLayoutRefused();
    testLatencyZero();
    testNoParameters();
    testHubFileTrack();
    testRegistryGroupsByTrack();
    testSameNameNumbered();
    testStaleHiddenAfter5s();
    testMalformedSkippedNotDeleted();
    testHubAndTmpFilesSkipped();
    testScan200();
    testCommandFileAtomicAndNamed();
    testUnansweredFailsIn3s();
    testDragThrottle();
    testEndToEndInProcess();
    testWindowShowsTrack();
    testWindowControlKinds();
    testWindowDoubleClickDefault();
    testWindowDragFollows();
    testWindowNotResponding();
    testWindowAllControls();
    testWindowRender();
    testTracksScreenLoadOrder();
    testUnnamedTrackRow();
    testTrackScreenBackToTracks();
    testControlsScreenBackToTrack();
    testScreenSurvivesReopen();
    testScreenSavedWithSession();
    testOldSessionViewLoads();
    testRememberedTrackGone();
    testHubCmdShowTrack();
    testHubCmdShowControls();
    testHubCmdShowTracks();
    testHubCmdUnknownTrack();
    testHubCmdBadJson();
    testHubCmdOrderAndDone16();
    testHubInboxPolledWindowClosed();
    testStaleHubInboxCleaned();

    // What every plug-in passes (TestKit). One preset only: Switchboard has nothing for a preset to change. With the hub file off,
    // for the reason given at testPassthrough: no timer thread may allocate while the sweeps count allocations.
    unsetenv ("SMOOTH_LINK_DIR");
    GenericOptions opt;
    opt.minPresets = 1;
    runGenericChecks ([] { return std::make_unique<SwitchboardProcessor>(); }, opt);

    setenv ("SMOOTH_LINK_DIR", dir, 1);
    return finish();
}
