// Offline tests for link/SmoothLink.h, the member side of PROTOCOL.md.
// Run: ctest --test-dir build --output-on-failure, or by hand with both scratch variables set:
//   SMOOTH_LINK_DIR=$PWD/work/scratch/linktest SMOOTH_KEY_JSON=$PWD/work/scratch/key.json build/LinkTest_artefacts/Release/LinkTest
// SMOOTH_LINK_DIR must hold an absolute scratch path. Every test makes its own folder under it; the real link folder is never used.
#include "TestKit.h"
#include "TestProcessor.h"
#include <algorithm>
#include <cstdio>
#include <pwd.h>
#include <random>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

using namespace testkit;
using linktest::TestProcessor;

namespace
{
juce::File base;

// One test's own folder, gone before the test starts. The link creates it.
juce::File useFolder (const char* name)
{
    auto f = base.getChildFile (name);
    f.deleteRecursively();
    setenv ("SMOOTH_LINK_DIR", f.getFullPathName().toRawUTF8(), 1);
    return f;
}

// Read through one descriptor to the end, as a reader must (see linkdetail::readFile): never juce::File::loadFileAsString.
juce::String readText (const juce::File& f)
{
    std::string bytes;
    if (! smooth::linkdetail::readFile (f.getFullPathName().toStdString(), bytes))
        return {};
    return juce::String::fromUTF8 (bytes.data(), (int) bytes.size());
}

juce::var readJson (const juce::File& f)
{
    juce::var v;
    if (! f.existsAsFile() || juce::JSON::parse (readText (f), v).failed())
        return {};
    return v;
}

bool has (const juce::var& v, const char* key) { return v.getDynamicObject() != nullptr && v.getDynamicObject()->hasProperty (key); }
bool isHexId (const juce::String& s) { return s.length() == 16 && s.containsOnly ("0123456789abcdef"); }
bool near (double a, double b, double tol = 1.0e-6) { return std::abs (a - b) <= tol; }
juce::int64 seqOf (const juce::var& card) { return has (card, "seq") ? (juce::int64) card["seq"] : -1; }
juce::var paramOf (const juce::var& card, int index) { auto* a = card["params"].getArray(); return a != nullptr && index < a->size() ? a->getReference (index) : juce::var(); }
double valueOf (const juce::var& card, int index) { return (double) paramOf (card, index)["value"]; }

juce::StringArray stringsOf (const juce::var& v)
{
    juce::StringArray out;
    if (auto* a = v.getArray())
        for (auto& s : *a)
            out.add (s.toString());
    return out;
}

// Sub-checks of one test, which prints one PASS or FAIL line. Each failed sub-check says what was not true.
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

// Command names sort in the order they are made: 13 digit unix ms, as the protocol writes them.
juce::String commandName (juce::int64 n)
{
    return juce::String (n) + "-test-" + juce::String::toHexString ((int) (0x100000 + (n % 0xeffff))).toLowerCase();
}
juce::int64 nextCommand = 1794430230000LL;
juce::String newCommandName() { return commandName (nextCommand++); }

void writeRaw (const juce::File& f, const juce::String& text)
{
    if (auto* out = std::fopen (f.getFullPathName().toRawUTF8(), "wb"))
    {
        std::fwrite (text.toRawUTF8(), 1, text.getNumBytesAsUTF8(), out);
        std::fclose (out);
    }
}

// A writer's way, from PROTOCOL.md: a hidden temporary name in the inbox, then a rename.
void sendCommand (const smooth::Link& link, const juce::String& name, const juce::String& json)
{
    const auto tmp = link.inbox().getChildFile ("." + name + ".tmp");
    writeRaw (tmp, json);
    std::rename (tmp.getFullPathName().toRawUTF8(), link.inbox().getChildFile (name + ".json").getFullPathName().toRawUTF8());
}

int countVisibleFiles (const juce::File& dir)
{
    int n = 0;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
        if (! f.getFileName().startsWithChar ('.'))
            ++n;
    return n;
}

juce::var doneEntry (const juce::var& card, const juce::String& name)
{
    if (auto* a = card["done"].getArray())
        for (auto& e : *a)
            if (e["cmd"].toString() == name)
                return e;
    return {};
}

long long mtimeNs (const juce::File& f)
{
    struct stat s;
    if (stat (f.getFullPathName().toRawUTF8(), &s) != 0)
        return -1;
    return (long long) s.st_mtimespec.tv_sec * 1000000000LL + s.st_mtimespec.tv_nsec;
}

void setAge (const juce::File& f, double secondsOld)
{
    struct timeval now;
    gettimeofday (&now, nullptr);
    struct timeval tv[2];
    tv[0].tv_sec = now.tv_sec - (time_t) secondsOld;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    utimes (f.getFullPathName().toRawUTF8(), tv);
}

// What a host sees: value changes and gestures, through the same listener the AU wrapper uses.
struct Recorder final : juce::AudioProcessorListener
{
    struct Event { char kind; int index; float value; };
    std::vector<Event> events;

    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float v) override { events.push_back ({ 'v', index, v }); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override { events.push_back ({ 'b', index, 0.0f }); }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override { events.push_back ({ 'e', index, 0.0f }); }

    std::string kinds (int index) const
    {
        std::string s;
        for (auto& e : events)
            if (e.index == index)
                s += e.kind;
        return s;
    }
    std::vector<float> values (int index) const
    {
        std::vector<float> v;
        for (auto& e : events)
            if (e.index == index && e.kind == 'v')
                v.push_back (e.value);
        return v;
    }
};

// ---------------------------------------------------------------------------------------------------------------------

void testCardAtConstruction()
{
    const auto dir = useFolder ("card_at_construction");
    TestProcessor p;
    Checks c;
    const auto card = p.link.card();
    const auto v = readJson (card);
    c.need (p.link.enabled(), "link enabled with SMOOTH_LINK_DIR set");
    c.need (dir.isDirectory(), "the folder was created");
    struct stat st {};
    c.need (stat (dir.getFullPathName().toRawUTF8(), &st) == 0 && (st.st_mode & 0777) == 0755, "the folder has mode 0755");
    c.need (card.existsAsFile() && card.getParentDirectory() == dir, "card exists in the folder");
    c.need (v.isObject(), "card parses as one JSON object");
    c.need (v["link"].isInt() && (int) v["link"] == 1, "link 1");
    c.need (isHexId (p.link.id()) && v["id"].toString() == p.link.id() && card.getFileName() == p.link.id() + ".json", "16 hex id, the same in the name and the card");
    c.need (v["plugin"].toString() == "Link Test" && v["type"].toString() == "aufx" && v["subtype"].toString() == "SLtt"
                && v["manufacturer"].toString() == "Luke" && v["version"].toString() == "1.2.3", "plugin, type, subtype, manufacturer, version");
    c.need (has (v, "track") && v["track"].isVoid(), "track null");
    c.need ((int) v["pid"] == (int) getpid(), "pid");
    c.need (near ((double) v["started"], juce::Time::currentTimeMillis() / 1000.0, 10.0), "started is now, in unix seconds");
    c.need (seqOf (v) >= 1, "seq");
    auto* params = v["params"].getArray();
    c.need (params != nullptr && params->size() == 5, "5 params");
    if (params != nullptr && params->size() == 5)
    {
        const juce::StringArray ids { "gain", "on", "mode", "voices", "#4" }, names { "Gain", "On", "Mode", "Voices", "Plain" };
        for (int i = 0; i < 5; ++i)
        {
            const auto& pr = params->getReference (i);
            c.need (pr["id"].toString() == ids[i] && (int) pr["index"] == i && pr["name"].toString() == names[i], "id, index and name of param " + juce::String (i));
            c.need (has (pr, "value") && has (pr, "text") && has (pr, "default") && has (pr, "steps") && has (pr, "choices") && has (pr, "label")
                        && has (pr, "automatable") && has (pr, "main") && pr["choices"].isArray(), "every field on param " + juce::String (i));
            c.need ((bool) pr["automatable"], "automatable " + juce::String (i));
        }
        // text is getText (value, 64) with the label after it, exactly as the plug-in gives it (JUCE writes 0 dB here as "-0.00").
        auto* gainParam = static_cast<juce::AudioProcessorParameter*> (p.params.gain);
        const auto gainText = gainParam->getText (gainParam->getValue(), 64) + " dB";
        const auto& g = params->getReference (0);
        c.need (near (g["value"], 1.0 / 3.0) && near (g["default"], 1.0 / 3.0) && (int) g["steps"] == 0 && g["choices"].getArray()->isEmpty()
                    && g["label"].toString() == "dB" && g["text"].toString() == gainText && ! gainText.endsWith ("dB dB") && (bool) g["main"],
                "gain: value, default, steps, no choices, label, text, main " + juce::JSON::toString (g, true));
        const auto& o = params->getReference (1);
        c.need (near (o["value"], 1.0) && (int) o["steps"] == 2 && stringsOf (o["choices"]) == juce::StringArray { "Off", "On" } && o["text"].toString() == "On"
                    && ! (bool) o["main"], "on: steps 2, choices Off On, text On, not main " + juce::JSON::toString (o, true));
        const auto& m = params->getReference (2);
        c.need (near (m["value"], 0.0) && (int) m["steps"] == 3 && stringsOf (m["choices"]) == juce::StringArray { "Soft", "Hard", "Robot" } && m["text"].toString() == "Soft",
                "mode: steps 3, choices Soft Hard Robot " + juce::JSON::toString (m, true));
        // steps is getNumSteps() only when isDiscrete() is true, from PROTOCOL.md. JUCE's AudioParameterInt is not discrete (its AU
        // host sees a plain 0 to 1 control too), so Voices has steps 0 even though it snaps to 1, 2, 3 and 4.
        const auto& n = params->getReference (3);
        c.need (near (n["value"], 1.0 / 3.0) && (int) n["steps"] == 0 && ! p.params.voices->isDiscrete() && n["choices"].getArray()->isEmpty() && n["text"].toString() == "2",
                "voices: steps 0 as JUCE does not call it discrete, no choices, text 2 " + juce::JSON::toString (n, true));
        const auto& pl = params->getReference (4);
        c.need (near (pl["value"], 0.25) && near (pl["default"], 0.25) && (int) pl["steps"] == 0 && pl["label"].toString().isEmpty() && pl["text"].toString() == "0.25",
                "plain: value, default, no steps, text " + juce::JSON::toString (pl, true));
    }
    c.need (v["done"].isArray() && v["done"].getArray()->isEmpty(), "done is an empty list");
    c.need (p.link.inbox().isDirectory() && p.link.inbox().getFileName() == p.link.id() + ".in", "inbox folder <id>.in exists");
    check (c.ok, "card_at_construction: the card is written at once and lists every control");
}

void testChangeFromOtherThread()
{
    useFolder ("change_from_other_thread");
    TestProcessor p;
    Checks c;
    const auto seq0 = seqOf (readJson (p.link.card()));
    std::thread t ([&] { p.params.gain->setValueNotifyingHost (0.9f); });
    t.join();
    juce::Thread::sleep (300);
    const auto before = readJson (p.link.card());
    c.need (seqOf (before) == seq0 && near (valueOf (before, 0), 1.0 / 3.0), "card unchanged before the poll");
    p.link.pollNow();
    const auto after = readJson (p.link.card());
    c.need (seqOf (after) == seq0 + 1, "seq up by one after the poll");
    c.need (near (valueOf (after, 0), 0.9, 1.0e-4), "new value on the card after the poll");
    check (c.ok, "change_from_other_thread_writes_on_poll_only: a change marks the card, the message thread writes it");
}

void testDebounce()
{
    useFolder ("debounce_4hz");
    TestProcessor p;
    double now = 1000.0;
    p.link.clock = [&] { return now; };
    p.link.pollNow();
    const auto seq0 = seqOf (readJson (p.link.card()));
    float last = 0.0f;
    for (int i = 0; i < 40; ++i)
    {
        now = 1000.0 + 0.025 * i + 0.001;
        last = (float) (i + 1) / 50.0f;
        p.params.gain->setValueNotifyingHost (last);
        p.link.pollNow();
    }
    const auto writes = seqOf (readJson (p.link.card())) - seq0;
    now += 0.3;
    p.link.pollNow();
    const auto final = readJson (p.link.card());
    Checks c;
    c.need (writes >= 1 && writes <= 5, "at most 5 writes in the second of changes");
    c.need (near (valueOf (final, 0), last, 1.0e-4), "the last value is on the card a quarter second later");
    check (c.ok, "debounce_4hz: 40 changes in 1 s, card writes", (double) writes);
}

void testCmdValueWithGesture()
{
    useFolder ("cmd_value_with_gesture");
    Recorder rec;
    TestProcessor p;
    p.addListener (&rec);
    const auto seq0 = seqOf (readJson (p.link.card()));
    const auto name = newCommandName();
    sendCommand (p.link, name, R"({ "param": "gain", "value": 0.75, "gesture": "tap", "from": "test" })");
    p.link.pollNow();
    const auto card = readJson (p.link.card());
    const auto done = doneEntry (card, name);
    Checks c;
    c.need (near (static_cast<juce::AudioProcessorParameter*> (p.params.gain)->getValue(), 0.75, 1.0e-5), "gain moved to 0.75");
    c.need (rec.kinds (0) == "bve", "host saw gesture begin, value, gesture end, in that order (saw '" + juce::String (rec.kinds (0)) + "')");
    c.need (rec.values (0).size() == 1 && near (rec.values (0)[0], 0.75, 1.0e-6), "host saw the value 0.75");
    c.need (countVisibleFiles (p.link.inbox()) == 0 && p.link.inbox().getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0, "inbox empty");
    c.need (seqOf (card) > seq0, "card written in the same poll");
    c.need (done.isObject() && (bool) done["ok"] && done["param"].toString() == "gain" && near (done["value"], 0.75, 1.0e-5)
                && done["text"].toString() == "15.00 dB" && done["error"].toString().isEmpty(), "done records the command with ok, value, text");
    c.need (near (valueOf (card, 0), 0.75, 1.0e-5), "card shows the new value");
    p.removeListener (&rec);
    check (c.ok, "cmd_value_with_gesture: a value command moves the control with a gesture and is confirmed in done");
}

void testCmdText()
{
    useFolder ("cmd_text");
    Recorder rec;
    TestProcessor p;
    p.addListener (&rec);
    Checks c;
    const auto n1 = newCommandName();
    sendCommand (p.link, n1, R"({ "param": "gain", "text": "6 dB" })");
    p.link.pollNow();
    auto card = readJson (p.link.card());
    c.need (near (p.params.gain->get(), 6.0, 1.0e-3) && (bool) doneEntry (card, n1)["ok"] && doneEntry (card, n1)["text"].toString() == "6.00 dB", "\"6 dB\" sets Gain to 6 dB");
    const auto n2 = newCommandName();
    sendCommand (p.link, n2, R"({ "param": "mode", "text": "robot" })");
    p.link.pollNow();
    card = readJson (p.link.card());
    c.need (p.params.mode->getIndex() == 2 && (bool) doneEntry (card, n2)["ok"] && doneEntry (card, n2)["text"].toString() == "Robot", "\"robot\" picks the choice Robot");
    const auto n3 = newCommandName();
    sendCommand (p.link, n3, R"({ "param": "on", "text": "OFF" })");
    p.link.pollNow();
    card = readJson (p.link.card());
    c.need (! p.params.on->get() && (bool) doneEntry (card, n3)["ok"], "\"OFF\" turns On off");
    p.params.voices->setValueNotifyingHost (1.0f);
    c.need (p.params.voices->get() == 4, "voices at 4 before default");
    const auto n4 = newCommandName();
    sendCommand (p.link, n4, R"({ "param": "voices", "text": "default" })");
    p.link.pollNow();
    card = readJson (p.link.card());
    c.need (p.params.voices->get() == 2 && (bool) doneEntry (card, n4)["ok"] && doneEntry (card, n4)["text"].toString() == "2", "\"default\" returns Voices to 2");
    c.need (rec.kinds (0) == "bve" && rec.kinds (2) == "bve" && rec.kinds (1) == "bve", "each text command is one gesture");
    p.removeListener (&rec);
    check (c.ok, "cmd_text: text commands \"6 dB\", \"robot\", \"OFF\" and \"default\"");
}

void testCmdBad()
{
    useFolder ("cmd_bad");
    Recorder rec;
    TestProcessor p;
    p.addListener (&rec);
    std::vector<float> before;
    for (auto* pr : p.getParameters())
        before.push_back (pr->getValue());
    struct Bad { const char* what; const char* json; };
    const Bad bad[] = { { "bad JSON", "{ \"param\": \"gain\", \"value\": " },
                        { "not an object", "[ 1, 2 ]" },
                        { "unknown param", R"({ "param": "nope", "value": 0.5 })" },
                        { "no param", R"({ "value": 0.5 })" },
                        { "both value and text", R"({ "param": "gain", "value": 0.5, "text": "3 dB" })" },
                        { "neither value nor text", R"({ "param": "gain" })" },
                        { "value not a number", R"({ "param": "gain", "value": "0.5" })" },
                        { "unknown choice name", R"({ "param": "mode", "text": "opera" })" },
                        { "unknown gesture", R"({ "param": "gain", "value": 0.5, "gesture": "wiggle" })" } };
    juce::StringArray names;
    for (auto& b : bad)
    {
        names.add (newCommandName());
        sendCommand (p.link, names[names.size() - 1], b.json);
    }
    p.link.pollNow();
    const auto card = readJson (p.link.card());
    Checks c;
    for (int i = 0; i < names.size(); ++i)
    {
        const auto d = doneEntry (card, names[i]);
        c.need (d.isObject() && has (d, "ok") && ! (bool) d["ok"] && d["error"].toString().isNotEmpty(), juce::String (bad[i].what) + ": done says ok false with an error");
    }
    c.need (p.link.inbox().getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0, "every bad file deleted");
    bool same = true;
    for (int i = 0; i < p.getParameters().size(); ++i)
        same = same && p.getParameters()[i]->getValue() == before[(size_t) i];
    c.need (same, "no value changed");
    c.need (rec.events.empty(), "the host saw no gesture and no change");
    p.removeListener (&rec);
    check (c.ok, "cmd_bad: bad JSON, unknown param, both, neither and other bad commands are refused and deleted");
}

void testDragGesture()
{
    useFolder ("drag_gesture");
    Recorder rec;
    TestProcessor p;
    p.addListener (&rec);
    double now = 2000.0;
    p.link.clock = [&] { return now; };
    Checks c;
    const char* steps[][2] = { { "begin", "0.2" }, { "move", "0.3" }, { "move", "0.4" }, { "end", "0.5" } };
    const char* expect[] = { "bv", "bvv", "bvvv", "bvvvve" };
    for (int i = 0; i < 4; ++i)
    {
        sendCommand (p.link, newCommandName(), juce::String ("{ \"param\": \"gain\", \"value\": ") + steps[i][1] + ", \"gesture\": \"" + steps[i][0] + "\" }");
        p.link.pollNow();
        c.need (rec.kinds (0) == expect[i], juce::String ("after ") + steps[i][0] + " the host saw '" + expect[i] + "' (saw '" + juce::String (rec.kinds (0)) + "')");
        now += 0.1;
    }
    const auto v = rec.values (0);
    c.need (v.size() == 4 && near (v[0], 0.2) && near (v[1], 0.3) && near (v[2], 0.4) && near (v[3], 0.5), "the drag's values in order");
    rec.events.clear();
    now = 2010.0;
    sendCommand (p.link, newCommandName(), R"({ "param": "gain", "value": 0.6, "gesture": "begin" })");
    p.link.pollNow();
    now = 2011.0;
    p.link.pollNow();
    now = 2011.9;
    p.link.pollNow();
    c.need (rec.kinds (0) == "bv", "a begin alone is still open after 1.9 s (saw '" + juce::String (rec.kinds (0)) + "')");
    now = 2012.05;
    p.link.pollNow();
    c.need (rec.kinds (0) == "bve", "a begin alone is ended after 2 s (saw '" + juce::String (rec.kinds (0)) + "')");
    now = 2015.0;
    p.link.pollNow();
    c.need (rec.kinds (0) == "bve", "and ended only once");
    p.removeListener (&rec);
    check (c.ok, "drag_gesture: begin, move, end is one gesture; a begin left alone is ended after 2 s");
}

void testOrderAnd64()
{
    useFolder ("order_and_64_limit");
    Recorder rec;
    TestProcessor p;
    p.addListener (&rec);
    std::vector<int> order (100);
    for (int i = 0; i < 100; ++i)
        order[(size_t) i] = i;
    std::mt19937 g (11);
    std::shuffle (order.begin(), order.end(), g);
    const juce::int64 first = 1794430240000LL;
    for (int i : order)
        sendCommand (p.link, commandName (first + i), "{ \"param\": \"gain\", \"value\": " + juce::String (i / 100.0) + " }");
    Checks c;
    p.link.pollNow();
    auto v = rec.values (0);
    bool inOrder = v.size() == 64;
    for (size_t i = 0; inOrder && i < v.size(); ++i)
        inOrder = near (v[i], (float) (i / 100.0));
    c.need (inOrder, "first poll applies 64, in name order (applied " + juce::String ((int) v.size()) + ")");
    c.need (countVisibleFiles (p.link.inbox()) == 36, "36 left for the next poll");
    p.link.pollNow();
    v = rec.values (0);
    inOrder = v.size() == 100;
    for (size_t i = 0; inOrder && i < v.size(); ++i)
        inOrder = near (v[i], (float) (i / 100.0));
    c.need (inOrder, "second poll applies the other 36, in name order");
    c.need (countVisibleFiles (p.link.inbox()) == 0, "inbox empty after two polls");
    const auto card = readJson (p.link.card());
    auto* done = card["done"].getArray();
    bool last16 = done != nullptr && done->size() == 16;
    for (int k = 0; last16 && k < 16; ++k)
        last16 = done->getReference (k)["cmd"].toString() == commandName (first + 84 + k);
    c.need (last16, "done holds the last 16, newest last");
    p.removeListener (&rec);
    check (c.ok, "order_and_64_limit: 100 commands, 64 in the first poll and 36 in the next, in name order");
}

void testTrackName()
{
    useFolder ("track_name");
    TestProcessor p;
    Checks c;
    std::thread t ([&] { p.link.setTrackName ("Lead Vox"); });
    t.join();
    p.link.pollNow();
    c.need (readJson (p.link.card())["track"].toString() == "Lead Vox", "track set from another thread");
    std::thread t2 ([&] { TestProcessor::TrackProperties props; props.name = juce::String ("Lead Vox Double"); p.updateTrackProperties (props); });
    t2.join();
    p.link.pollNow();
    c.need (readJson (p.link.card())["track"].toString() == "Lead Vox Double", "a rename through updateTrackProperties follows");
    p.link.setTrackName (juce::String (juce::CharPointer_UTF8 ("Ch\xc5\x93ur \xc3\xa9t\xc3\xa9")));
    p.link.pollNow();
    c.need (readJson (p.link.card())["track"].toString() == juce::String (juce::CharPointer_UTF8 ("Ch\xc5\x93ur \xc3\xa9t\xc3\xa9")), "a name with accents survives");
    p.link.setTrackName ({});
    p.link.pollNow();
    const auto v = readJson (p.link.card());
    c.need (has (v, "track") && v["track"].isVoid(), "an empty name is null");
    TestProcessor::TrackProperties none;
    p.link.setTrackName ("Bass");
    p.link.pollNow();
    p.updateTrackProperties (none);
    p.link.pollNow();
    const auto w = readJson (p.link.card());
    c.need (has (w, "track") && w["track"].isVoid(), "no name from the host is null");
    check (c.ok, "track_name: set from another thread, renamed, empty is null");
}

void testDestructionCleans()
{
    const auto dir = useFolder ("destruction_cleans");
    auto a = std::make_unique<TestProcessor>();
    auto b = std::make_unique<TestProcessor>();
    const auto aCard = a->link.card(), aInbox = a->link.inbox(), bCard = b->link.card(), bInbox = b->link.inbox();
    const auto notes = dir.getChildFile ("notes.txt"), other = dir.getChildFile ("0123456789abcdef.json"), hub = dir.getChildFile ("hub-fedcba9876543210.json");
    writeRaw (notes, "not ours");
    writeRaw (other, "{}");
    writeRaw (hub, "{}");
    sendCommand (a->link, newCommandName(), R"({ "param": "gain", "value": 0.5 })");
    writeRaw (aInbox.getChildFile (".half.tmp"), "{");
    Checks c;
    c.need (aCard.existsAsFile() && aInbox.isDirectory() && aInbox.getNumberOfChildFiles (juce::File::findFiles) == 2, "a's card and inbox with two files before");
    a.reset();
    c.need (! aCard.exists() && ! aInbox.exists(), "a's card and inbox are gone");
    c.need (bCard.existsAsFile() && bInbox.isDirectory(), "b's card and inbox stay");
    c.need (notes.existsAsFile() && other.existsAsFile() && hub.existsAsFile(), "files that are not a's stay");
    b.reset();
    c.need (! bCard.exists() && ! bInbox.exists() && dir.isDirectory(), "b cleans up too, the folder stays");
    int tmp = 0;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
        if (f.getFileName().contains (".tmp"))
            ++tmp;
    c.need (tmp == 0, "no temporary files left");
    check (c.ok, "destruction_cleans: only the instance's own card and inbox go");
}

void testStaleCleanup()
{
    const auto dir = useFolder ("stale_cleanup");
    dir.createDirectory();
    const auto staleCard = dir.getChildFile ("aaaaaaaaaaaaaaaa.json"), staleInbox = dir.getChildFile ("aaaaaaaaaaaaaaaa.in");
    const auto liveCard = dir.getChildFile ("bbbbbbbbbbbbbbbb.json"), liveInbox = dir.getChildFile ("bbbbbbbbbbbbbbbb.in");
    const auto orphanOld = dir.getChildFile ("cccccccccccccccc.in"), orphanNew = dir.getChildFile ("dddddddddddddddd.in");
    const auto hubOld = dir.getChildFile ("hub-1111111111111111.json"), hubNew = dir.getChildFile ("hub-2222222222222222.json");
    const auto tmpOld = dir.getChildFile ("eeeeeeeeeeeeeeee.json.tmp1234"), tmpNew = dir.getChildFile ("ffffffffffffffff.json.tmp5678");
    const auto notes = dir.getChildFile ("notes.txt");
    staleInbox.createDirectory();
    writeRaw (staleInbox.getChildFile ("1794430230000-test-abcdef.json"), "{}");
    writeRaw (staleCard, "{}");
    liveInbox.createDirectory();
    writeRaw (liveCard, "{}");
    orphanOld.createDirectory();
    writeRaw (orphanOld.getChildFile ("x.json"), "{}");
    orphanNew.createDirectory();
    writeRaw (hubOld, "{}");
    writeRaw (hubNew, "{}");
    writeRaw (tmpOld, "{");
    writeRaw (tmpNew, "{");
    writeRaw (notes, "not ours");
    setAge (staleCard, 700);
    setAge (liveCard, 100);
    setAge (liveInbox, 700);
    setAge (orphanOld, 700);
    setAge (orphanNew, 100);
    setAge (hubOld, 700);
    setAge (hubNew, 100);
    setAge (tmpOld, 700);
    setAge (tmpNew, 100);
    setAge (notes, 700);
    TestProcessor p;
    Checks c;
    c.need (! staleCard.exists() && ! staleInbox.exists(), "700 s card and its inbox removed");
    c.need (! hubOld.exists(), "700 s hub file removed");
    c.need (! orphanOld.exists(), "700 s inbox with no card removed");
    c.need (! tmpOld.exists(), "700 s temporary card removed");
    c.need (liveCard.existsAsFile() && liveInbox.isDirectory(), "100 s card and its inbox kept");
    c.need (orphanNew.isDirectory() && hubNew.existsAsFile() && tmpNew.existsAsFile(), "100 s inbox, hub file and temporary card kept");
    c.need (notes.existsAsFile(), "a file that is not part of the protocol is never touched");
    c.need (p.link.card().existsAsFile(), "the new card is written");
    check (c.ok, "stale_cleanup: on start, files older than 600 s go and younger ones stay");
}

void testHeartbeat()
{
    useFolder ("heartbeat");
    TestProcessor p;
    const auto card = p.link.card();
    const auto m0 = mtimeNs (card);
    const auto seq0 = seqOf (readJson (card));
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    double movedAfter = -1.0;
    while (juce::Time::getMillisecondCounterHiRes() - t0 < 1000.0)
    {
        juce::Thread::sleep (20);
        p.link.pollNow();
        if (mtimeNs (card) != m0)
        {
            movedAfter = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            break;
        }
    }
    Checks c;
    c.need (movedAfter >= 0.0, "mtime moved within 1 s");
    c.need (seqOf (readJson (card)) == seq0, "with nothing changed the card is touched, not rewritten");
    check (c.ok, "heartbeat: the card's mtime moves with nothing changed, after", movedAfter, "s");
}

void testAtomicUnderLoad()
{
    useFolder ("atomic_under_load");
    TestProcessor p;
    double now = 3000.0;
    p.link.clock = [&] { return now; };
    const auto card = p.link.card();
    std::atomic<bool> started { false }, writing { true };
    std::atomic<int> failures { 0 }, parses { 0 }, parsesDuringWrites { 0 };
    std::thread reader ([&]
    {
        started = true;
        juce::int64 lastSeq = 0;
        for (int i = 0; i < 10000; ++i)
        {
            juce::var v;
            const auto text = readText (card);
            const bool ok = juce::JSON::parse (text, v).wasOk() && v.isObject() && v["params"].getArray() != nullptr
                            && v["params"].getArray()->size() == 5 && seqOf (v) >= lastSeq;
            if (! ok)
                ++failures;
            else
                lastSeq = seqOf (v);
            ++parses;
            if (writing)
                ++parsesDuringWrites;
        }
    });
    while (! started)
        std::this_thread::yield();
    const auto seq0 = seqOf (readJson (card));
    for (int i = 0; i < 500; ++i)
    {
        now += 0.3;
        p.params.gain->setValueNotifyingHost ((float) (i % 100) / 100.0f);
        p.link.pollNow();
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }
    writing = false;
    reader.join();
    const auto writes = seqOf (readJson (card)) - seq0;
    info ("parses while the writer was writing", parsesDuringWrites.load());
    Checks c;
    c.need (writes == 500, "500 writes (made " + juce::String (writes) + ")");
    c.need (parses == 10000, "10,000 parses");
    c.need (parsesDuringWrites > 1000, "most parses ran during the writes");
    check (c.ok && failures == 0, "atomic_under_load: a reader never sees a missing, partial or older card, failures", failures.load());
}

void testTwoInstances()
{
    const auto dir = useFolder ("two_instances");
    TestProcessor a, b;
    Checks c;
    c.need (isHexId (a.link.id()) && isHexId (b.link.id()) && a.link.id() != b.link.id(), "two distinct 16 hex ids");
    c.need (readJson (a.link.card())["id"].toString() == a.link.id() && readJson (b.link.card())["id"].toString() == b.link.id(), "two cards, each with its own id");
    c.need (a.link.inbox() != b.link.inbox() && a.link.inbox().isDirectory() && b.link.inbox().isDirectory(), "two inboxes");
    check (c.ok, "two_instances: distinct ids, cards and inboxes");
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

void testDisabledWithoutDir()
{
    using W = juce::AudioProcessor::WrapperType;
    const auto saved = juce::String (std::getenv ("SMOOTH_LINK_DIR"));
    const auto real = juce::File (realHome() + "/Library/Application Support/SmoothPlugins/link");
    Checks c;

    unsetenv ("SMOOTH_LINK_DIR");
    c.need (smooth::Link::folder() == real, "no SMOOTH_LINK_DIR: the folder is ~/Library/Application Support/SmoothPlugins/link");
    c.need (smooth::Link::enabledFor (W::wrapperType_AudioUnit), "no SMOOTH_LINK_DIR: AudioUnit yes");
    for (auto w : { W::wrapperType_Undefined, W::wrapperType_VST3, W::wrapperType_Standalone, W::wrapperType_AudioUnitv3, W::wrapperType_LV2 })
        c.need (! smooth::Link::enabledFor (w), "no SMOOTH_LINK_DIR: wrapper " + juce::String ((int) w) + " no");
    // Built only when the table says no, so a broken link can never write the real folder from a test.
    if (! smooth::Link::enabledFor (W::wrapperType_Undefined))
    {
        TestProcessor p;
        c.need (! p.link.enabled(), "a plug-in test without SMOOTH_LINK_DIR has the link off");
        c.need (! p.link.card().exists() && ! p.link.inbox().exists(), "and writes nothing");
        p.link.pollNow();
        c.need (! p.link.card().exists(), "not even on a poll");
    }

    setenv ("SMOOTH_LINK_DIR", "relative/folder", 1);
    c.need (smooth::Link::folder() == real && ! smooth::Link::enabledFor (W::wrapperType_Undefined) && smooth::Link::enabledFor (W::wrapperType_AudioUnit),
            "a relative SMOOTH_LINK_DIR counts as not set");
    setenv ("SMOOTH_LINK_DIR", "", 1);
    c.need (smooth::Link::folder() == real && ! smooth::Link::enabledFor (W::wrapperType_Undefined), "an empty SMOOTH_LINK_DIR counts as not set");

    const auto dir = base.getChildFile ("disabled_without_dir");
    setenv ("SMOOTH_LINK_DIR", dir.getFullPathName().toRawUTF8(), 1);
    c.need (smooth::Link::folder() == dir, "an absolute SMOOTH_LINK_DIR is the folder");
    for (auto w : { W::wrapperType_Undefined, W::wrapperType_VST3, W::wrapperType_Standalone, W::wrapperType_AudioUnit })
        c.need (smooth::Link::enabledFor (w), "SMOOTH_LINK_DIR set: wrapper " + juce::String ((int) w) + " yes");

    setenv ("SMOOTH_LINK_DIR", saved.toRawUTF8(), 1);
    check (c.ok, "disabled_without_dir: AudioUnit always, anything else only with SMOOTH_LINK_DIR");
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

    testCardAtConstruction();
    testChangeFromOtherThread();
    testDebounce();
    testCmdValueWithGesture();
    testCmdText();
    testCmdBad();
    testDragGesture();
    testOrderAnd64();
    testTrackName();
    testDestructionCleans();
    testStaleCleanup();
    testHeartbeat();
    testAtomicUnderLoad();
    testTwoInstances();
    testDisabledWithoutDir();

    setenv ("SMOOTH_LINK_DIR", dir, 1);
    return finish();
}
