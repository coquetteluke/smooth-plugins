// Offline tests for the Switchboard AU (hub/). Run: ctest --test-dir build --output-on-failure, or by hand with both scratch
// variables set: SMOOTH_LINK_DIR=$PWD/work/scratch/hubtest SMOOTH_KEY_JSON=$PWD/work/scratch/key.json build/HubTest_artefacts/Release/HubTest
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"
#include "PlaceholderEditor.h"
#include <cstring>
#include <pwd.h>
#include <random>
#include <sys/stat.h>
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
            if (f != file)
                ++others;
        c.need (others == 0, "nothing else in the folder: Switchboard writes no member card");
    }
    c.need (! file.exists() && dir.isDirectory() && dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0, "removed on destruction, the folder stays");

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

void testWindowShowsTrack()
{
    useFolder ("window_shows_track");
    SwitchboardProcessor p;
    Checks c;
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorIfNeeded());
        auto* w = dynamic_cast<PlaceholderEditor*> (ed.get());
        c.need (w != nullptr && w->getWidth() > 0 && w->getHeight() > 0, "the window opens");
        if (w != nullptr)
        {
            c.need (w->shownTrack().isEmpty(), "no name at first");
            SwitchboardProcessor::TrackProperties props;
            props.name = juce::String ("Bass");
            p.updateTrackProperties (props);
            w->refresh();
            c.need (w->shownTrack() == "Bass", "shows the track name");
            juce::Image img (juce::Image::ARGB, w->getWidth(), w->getHeight(), true);
            juce::Graphics g (img);
            w->paintEntireComponent (g, false);
            c.need (img.getPixelAt (w->getWidth() - 4, w->getHeight() - 4).getAlpha() == 255, "the window paints");
            // SWITCHBOARD_WINDOW_PNG=<absolute path> saves the painted window, to look at.
            if (const char* png = std::getenv ("SWITCHBOARD_WINDOW_PNG"); png != nullptr && png[0] == '/')
            {
                juce::File out (png);
                out.deleteFile();
                juce::FileOutputStream stream (out);
                juce::PNGImageFormat().writeImageToStream (img, stream);
            }
        }
    }
    check (c.ok, "window_shows_track: the placeholder window shows its own track name");
}
} // namespace

int main()
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

    testPassthrough();
    testLatencyZero();
    testNoParameters();
    testHubFileTrack();
    testWindowShowsTrack();

    // What every plug-in passes (TestKit). One preset only: Switchboard has nothing for a preset to change. With the hub file off,
    // for the reason given at testPassthrough: no timer thread may allocate while the sweeps count allocations.
    unsetenv ("SMOOTH_LINK_DIR");
    GenericOptions opt;
    opt.minPresets = 1;
    runGenericChecks ([] { return std::make_unique<SwitchboardProcessor>(); }, opt);

    setenv ("SMOOTH_LINK_DIR", dir, 1);
    return finish();
}
