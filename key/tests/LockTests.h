#pragma once
// Lock, the runner up, the close call, and what key.json gained in 1.7.0. Included by PluginTest.cpp after WindowTests.h.
// PluginTest --lock runs only these.
#include "KeyPublish.h"
#include "editor/Controls.h"
#include "editor/KeyViews.h"

// Limits for these tests, written before they were first run. Never loosen one to make a run pass.
namespace locklimits
{
    constexpr double lockFileSeconds = 1.5;        // switching Lock on reaches key.json within this, as a Manual Key change does
    constexpr int followerBufferBytes = 2047;      // what every follower reads of the file at most (KeyFollower.h, char text[2048])
    constexpr double followerMaxAgeSeconds = 600;  // a follower ignores a file older than this
    constexpr double refreshAtMostSeconds = 300;   // so a held key must be written again at least this often
    constexpr double followerActsFrom = 0.5;       // KeyFollower::minConfidence
    constexpr int existingParameters = 15;         // in 1.6.0
}

namespace locktests
{
using smoothkey::KeySource;
using smoothkey::KeyState;
using smoothkey::Publisher;
using smoothkey::Reading;
using smoothkey::ShowControls;

// ---- the follower's reading of the file -----------------------------------------------------------------------------
// Copied on 2026-11-11 from harmony/src/KeyFollower.h, which is the same code in Tune, Tune Pro and Glitter. If the line
// Key writes ever stops parsing here, it has stopped parsing in every plug-in that follows the key.
struct FollowerData { int key = 0; bool minor = false; double confidence = 0.0, time = 0.0; bool listening = false; };

inline const char* followerFindField (const char* text, const char* name) noexcept
{
    char quoted[32];
    std::snprintf (quoted, sizeof quoted, "\"%s\"", name);
    const char* at = std::strstr (text, quoted);
    if (at == nullptr) return nullptr;
    at += std::strlen (quoted);
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') ++at;
    if (*at != ':') return nullptr;
    ++at;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') ++at;
    return at;
}

inline bool followerParse (const char* text, FollowerData& d) noexcept
{
    const char* k = followerFindField (text, "key");
    const char* s = followerFindField (text, "scale");
    const char* c = followerFindField (text, "confidence");
    const char* t = followerFindField (text, "time");
    if (k == nullptr || s == nullptr || c == nullptr || t == nullptr || *k != '"' || *s != '"')
        return false;

    ++k;
    static constexpr const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static constexpr const char* flats[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    int key = -1;
    for (int i = 0; i < 12 && key < 0; ++i)
    {
        const size_t a = std::strlen (names[i]), b = std::strlen (flats[i]);
        if ((std::strncmp (k, names[i], a) == 0 && k[a] == '"') || (std::strncmp (k, flats[i], b) == 0 && k[b] == '"'))
            key = i;
    }
    if (key < 0)
        return false;

    ++s;
    bool minor;
    if (std::strncmp (s, "major\"", 6) == 0) minor = false;
    else if (std::strncmp (s, "minor\"", 6) == 0) minor = true;
    else return false;

    char* end = nullptr;
    const double confidence = std::strtod (c, &end);
    if (end == c || ! std::isfinite (confidence) || confidence < 0.0 || confidence > 1.0)
        return false;
    const double time = std::strtod (t, &end);
    if (end == t || ! std::isfinite (time) || time <= 0.0)
        return false;

    d.key = key;
    d.minor = minor;
    d.confidence = confidence;
    d.time = time;
    const char* l = followerFindField (text, "listening");
    d.listening = l != nullptr && std::strncmp (l, "true", 4) == 0;
    return true;
}

// The line exactly as 1.6.0 wrote it, to hold the front of the new line against.
inline std::string lineOf160 (const KeyState& s, double unixSeconds)
{
    char out[256];
    const int hundredths = (int) std::lround (std::fmin (1.0, std::fmax (0.0, s.confidence)) * 100.0);
    const long long tenths = std::llround (unixSeconds * 10.0);
    std::snprintf (out, sizeof out, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":%d.%02d,\"time\":%lld.%lld,\"listening\":%s}",
                   smoothkey::keyNames[((s.key % 12) + 12) % 12], s.minor ? "minor" : "major",
                   hundredths / 100, hundredths % 100, tenths / 10, tenths % 10, s.listening ? "true" : "false");
    return out;
}

inline std::string lineOf (const KeyState& s, double unixSeconds)
{
    char out[smoothkey::KeyFile::lineSize];
    smoothkey::KeyFile::format (out, sizeof out, s, unixSeconds);
    return out;
}

inline bool waitFor (const std::function<bool()>& done, int ms)
{
    for (int t = 0; t < ms; t += 10)
    {
        if (done()) return true;
        sleepMs (10);
    }
    return done();
}

inline void setLock (PluginProcessor& p, bool on)
{
    p.getLockParameter().setValueNotifyingHost (on ? 1.0f : 0.0f);
}

inline Reading reading (int key, bool minor, float confidence, int altKey = -1, bool altMinor = false, float altConfidence = 0.0f, float margin = 1.0f, bool unsure = false)
{
    Reading r;
    r.key = key;
    r.minor = minor;
    r.confidence = confidence;
    r.altKey = altKey;
    r.altMinor = altMinor;
    r.altConfidence = altConfidence;
    r.margin = margin;
    r.unsure = unsure;
    r.signal = true;
    return r;
}

inline juce::String readingText (PluginProcessor& p, const char* name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64) == name)
            return prm->getCurrentValueAsText();
    return {};
}

struct ParamWatch
{
    explicit ParamWatch (juce::AudioProcessorParameter& p) : param (p) { param.addListener (&count); }
    ~ParamWatch() { param.removeListener (&count); }
    bool oneGesture() const { return count.begins == 1 && count.ends == 1 && count.changes >= 1; }
    juce::AudioProcessorParameter& param;
    windowtests::GestureCount count;
};
}

// ---- the parameters: nothing that was there has moved --------------------------------------------------------------

static void testLockParameter()
{
    using namespace locktests;
    PluginProcessor p (tempJson ("lock-params.json"));

    // Every parameter of 1.6.0, in its order, with its ID, name and the number an Audio Unit host knows it by. That number
    // is a hash of the ID with the top bit cleared (JUCE's generateAUParameterID). Sessions and automation are saved by it.
    struct Pin { const char* id; const char* name; unsigned auId; };
    constexpr Pin pins[locklimits::existingParameters] = {
        { "listening", "Listening", 928768187u },       { "reset", "Reset", 108404047u },
        { "response", "Response", 1807160385u },        { "source", "Source", 1250977819u },
        { "mode", "Mode", 3357091u },                   { "manualKey", "Manual Key", 93511865u },
        { "manualScale", "Manual Scale", 1825381668u }, { "swapRelative", "Relative Swap", 2075078015u },
        { "tempoScale", "Tempo Scale", 1376459503u },   { "bypass", "Bypass", 773352680u },
        { "detectedKey", "Detected Key", 1977706621u }, { "detectedScale", "Detected Scale", 60343272u },
        { "confidence", "Confidence", 829251210u },     { "referencePitch", "Reference Pitch", 233335477u },
        { "detectedTempo", "Detected Tempo", 61338041u } };

    const auto& params = p.getParameters();
    const auto auIdOf = [] (const juce::String& id) { return (unsigned) id.hashCode() & 0x7fffffffu; };
    bool same = params.size() >= locklimits::existingParameters;
    for (int i = 0; same && i < locklimits::existingParameters; ++i)
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i]);
        const bool ok = withId != nullptr && withId->paramID == pins[i].id && params[i]->getName (64) == pins[i].name
                        && auIdOf (withId->paramID) == pins[i].auId && params[i]->getVersionHint() == 1 && params[i]->getParameterIndex() == i;
        if (! ok) std::printf ("  parameter %d is not %s any more\n", i, pins[i].id);
        same = same && ok;
    }
    check (same, "the 15 parameters of 1.6.0 keep their order, IDs, names, version hints and Audio Unit numbers");

    check (params.size() == locklimits::existingParameters + 1, "one parameter was added, count", params.size());
    auto* added = params.size() > locklimits::existingParameters ? dynamic_cast<juce::AudioProcessorParameterWithID*> (params.getLast()) : nullptr;
    check (added != nullptr && added->paramID == "lock" && added->getName (64) == "Lock" && added->isBoolean() && added->isAutomatable()
               && added->getDefaultValue() == 0.0f && added->getValue() == 0.0f && added->getParameterIndex() == locklimits::existingParameters,
           "the added parameter is Lock, an on or off control that starts off, and it is the last one");

    // The order an Audio Unit host is given: by version hint, and by Audio Unit number within one hint
    // (JuceAU::GetParameterList). "lock" has the smallest number of all, so only a higher hint keeps it from going first.
    constexpr const char* auOrder160[locklimits::existingParameters] = { "mode", "detectedScale", "detectedTempo", "manualKey", "reset", "referencePitch", "bypass", "confidence",
                                                                        "listening", "source", "tempoScale", "response", "manualScale", "detectedKey", "swapRelative" };
    struct Entry { unsigned id; int hint; juce::String name; };
    std::vector<Entry> list;
    for (auto* prm : params)
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            list.push_back ({ auIdOf (withId->paramID), prm->getVersionHint(), withId->paramID });
    std::sort (list.begin(), list.end(), [] (const Entry& a, const Entry& b) { return a.id < b.id; });
    std::stable_sort (list.begin(), list.end(), [] (const Entry& a, const Entry& b) { return a.hint < b.hint; });
    bool auSame = list.size() == (size_t) locklimits::existingParameters + 1;
    for (size_t i = 0; auSame && i < (size_t) locklimits::existingParameters; ++i)
        auSame = list[i].name == auOrder160[i];
    check (auSame && list.back().name == "lock", "an Audio Unit host is given the 15 old parameters in their 1.6.0 order, then Lock");
    std::set<unsigned> unique;
    for (auto& e : list) unique.insert (e.id);
    check (unique.size() == list.size(), "no two parameters share an Audio Unit number");

    // The saved parameters are the ten they were. Lock is not among them: it is saved beside them, and only when on.
    juce::StringArray saved;
    for (int i = 0; i < p.apvts.state.getNumChildren(); ++i)
        saved.add (p.apvts.state.getChild (i).getProperty ("id").toString());
    saved.sort (false);
    check (saved.joinIntoString (",") == "bypass,listening,manualKey,manualScale,mode,reset,response,source,swapRelative,tempoScale",
           "the saved parameter list is the ten of 1.6.0: " + saved.joinIntoString (",").toStdString());
}

// ---- what is shown and published, with no threads -----------------------------------------------------------------

static void testLockRules()
{
    using namespace locktests;
    const Reading none;
    const Reading eMajor = reading (4, false, 0.8f, 9, false, 0.3f, 0.6f);
    const Reading gMinor = reading (7, true, 0.7f, 10, false, 0.2f, 0.7f);
    ShowControls listen;

    {
        Publisher pub;
        const auto s = pub.update (eMajor, listen);
        check (s.key == 4 && ! s.minor && s.confidence == 0.8f && s.altKey == 9 && ! s.altMinor && s.altConfidence == 0.3f && s.margin == 0.6f
                   && ! s.unsure && s.source == KeySource::heard && ! s.lockWaiting && s.heardKey == 4,
               "Lock off: the reading and its runner up pass through as heard");
        const auto empty = pub.update (none, listen);
        check (empty.key == -1 && empty.altKey == -1 && empty.confidence == 0.0f && ! empty.unsure, "Lock off: no reading shows no key and no runner up");
    }
    {
        // Relative Swap names the same notes the other way, for the runner up too. C major over A minor must not become A minor over A minor.
        Publisher pub;
        ShowControls swap;
        swap.swapRelative = true;
        const auto s = pub.update (reading (0, false, 0.8f, 9, true, 0.5f, 0.2f), swap);
        check (s.key == 9 && s.minor && s.altKey == 0 && ! s.altMinor, "Relative Swap swaps the runner up with the key: A minor over C major");
        const auto t = pub.update (reading (0, false, 0.8f, 7, false, 0.3f, 0.5f), swap);
        check (t.key == 9 && t.minor && t.altKey == 4 && t.altMinor, "Relative Swap: C major over G major shows A minor over E minor");
        const auto same = pub.update (reading (0, false, 0.8f, 0, false, 0.3f, 0.5f), listen);
        check (same.altKey == -1 && same.margin == 1.0f && same.altConfidence == 0.0f, "a runner up that is the key itself is not a runner up");
    }
    {
        // The promise to followers: an unsure reading never reaches the confidence they act on, whatever the analyser sent.
        Publisher pub;
        const auto s = pub.update (reading (4, false, 0.9f, 9, false, 0.85f, 0.02f, true), listen);
        check (s.unsure && s.confidence < (float) locklimits::followerActsFrom && s.key == 4 && s.altKey == 9, "unsure: both keys named and the confidence held under 0.5", s.confidence);
        const auto alone = pub.update (reading (4, false, 0.9f, -1, false, 0.0f, 1.0f, true), listen);
        check (! alone.unsure && alone.confidence == 0.9f, "unsure with no second key to name is not shown as unsure");
    }
    {
        Publisher pub;
        ShowControls manual;
        manual.manual = true;
        manual.manualKey = 6;
        const auto s = pub.update (eMajor, manual);
        check (s.key == 6 && ! s.minor && s.confidence == 1.0f && s.source == KeySource::manual && s.altKey == -1 && ! s.unsure && s.heardKey == -1,
               "Manual: the manual key at confidence 1, with no runner up");
    }
    {
        // Lock, switched on with a key on show.
        Publisher pub;
        ShowControls locked;
        locked.lock = true;
        pub.update (eMajor, listen);
        auto s = pub.update (gMinor, locked);
        check (s.key == 4 && ! s.minor && s.confidence == 1.0f && s.source == KeySource::locked && ! s.lockWaiting,
               "Lock on: the key that was on show is held at confidence 1, not the one that arrived with the click");
        check (s.altKey == -1 && ! s.unsure && s.margin == 1.0f && s.heardKey == 7 && s.heardMinor && s.heardConfidence == 0.7f,
               "Lock on: no runner up is published, and what it hears now is still there for the window");
        s = pub.update (none, locked);
        const bool afterReset = s.key == 4 && s.confidence == 1.0f && s.source == KeySource::locked && s.heardKey == -1;
        Reading silent = gMinor;
        silent.confidence = 0.0f;
        silent.signal = false;
        s = pub.update (silent, locked);
        check (afterReset && s.key == 4 && ! s.minor && s.confidence == 1.0f, "Lock on: the held key stays through a reading that starts again from nothing, and through silence");
        ShowControls lockedManual = locked;
        lockedManual.manual = true;
        lockedManual.manualKey = 1;
        s = pub.update (gMinor, lockedManual);
        check (s.key == 4 && s.source == KeySource::locked, "Lock on: switching to Manual does not move the held key");
        s = pub.update (gMinor, listen);
        check (s.key == 7 && s.minor && s.confidence == 0.7f && s.source == KeySource::heard && s.altKey == 10, "Lock off again: back to what it hears");
        s = pub.update (eMajor, locked);
        check (s.key == 7 && s.minor && s.source == KeySource::locked, "Lock on a second time holds the key on show then, not the one from before");
    }
    {
        // Luke sees a close call, picks the key on show and locks it. That is his choice, so it holds, sure or not.
        Publisher pub;
        ShowControls locked;
        locked.lock = true;
        pub.update (reading (4, false, 0.4f, 9, false, 0.38f, 0.03f, true), listen);
        const auto s = pub.update (reading (4, false, 0.4f, 9, false, 0.38f, 0.03f, true), locked);
        check (s.key == 4 && s.confidence == 1.0f && ! s.unsure && s.source == KeySource::locked, "Lock on during a close call holds the key on show at confidence 1");
    }
    {
        // Held with Relative Swap as shown.
        Publisher pub;
        ShowControls swap, swapLocked, locked;
        swap.swapRelative = swapLocked.swapRelative = true;
        swapLocked.lock = locked.lock = true;
        pub.update (reading (0, false, 0.8f), swap);
        auto s = pub.update (reading (0, false, 0.8f), swapLocked);
        const bool heldSwapped = s.key == 9 && s.minor && s.source == KeySource::locked;
        s = pub.update (reading (0, false, 0.8f), locked);
        check (heldSwapped && s.key == 9 && s.minor && s.heardKey == 0 && ! s.heardMinor, "Lock holds the key as shown with Relative Swap, and Swap no longer moves it");
    }
    {
        // Lock, switched on with nothing on show: it waits for a key a follower would act on.
        Publisher pub;
        ShowControls locked;
        locked.lock = true;
        auto s = pub.update (none, locked);
        const bool waits = s.lockWaiting && s.key == -1 && s.source == KeySource::heard;
        s = pub.update (reading (0, false, 0.3f), locked);
        const bool notYet = s.lockWaiting && s.key == 0 && s.confidence == 0.3f && s.source == KeySource::heard;
        s = pub.update (reading (0, false, 0.9f, 7, false, 0.8f, 0.02f, true), locked);
        const bool notUnsure = s.lockWaiting && s.unsure && s.source == KeySource::heard;
        check (waits && notYet && notUnsure, "Lock on with no key yet: it waits, and a weak or unsure reading is shown but not held");
        s = pub.update (reading (2, false, 0.5f), locked);
        const bool took = ! s.lockWaiting && s.key == 2 && s.confidence == 1.0f && s.source == KeySource::locked;
        s = pub.update (gMinor, locked);
        check (took && s.key == 2 && ! s.minor && s.source == KeySource::locked, "a waiting Lock holds the first reading at confidence 0.5 or more, and keeps it");
    }
    {
        // Lock in Manual holds the manual key.
        Publisher pub;
        ShowControls manual, manualLocked;
        manual.manual = manualLocked.manual = true;
        manual.manualKey = manualLocked.manualKey = 6;
        manualLocked.lock = true;
        pub.update (none, manual);
        auto s = pub.update (none, manualLocked);
        const bool held = s.key == 6 && s.source == KeySource::locked && s.confidence == 1.0f;
        manualLocked.manualKey = 2;
        manualLocked.manualMinor = true;
        s = pub.update (none, manualLocked);
        const bool stays = s.key == 6 && ! s.minor;
        manual.manualKey = 2;
        manual.manualMinor = true;
        s = pub.update (none, manual);
        check (held && stays && s.key == 2 && s.minor && s.source == KeySource::manual, "Lock in Manual holds the manual key until Lock is off");

        Publisher fresh;
        ShowControls waitingManual;
        waitingManual.lock = true;
        fresh.update (none, waitingManual);
        waitingManual.manual = true;
        waitingManual.manualKey = 3;
        s = fresh.update (none, waitingManual);
        check (s.key == 3 && s.source == KeySource::locked, "a waiting Lock takes a key set by hand at once");
    }
    {
        // A loaded session.
        Publisher pub;
        ShowControls locked;
        locked.lock = true;
        pub.restore (10, true);
        auto s = pub.update (none, locked);
        check (s.key == 10 && s.minor && s.confidence == 1.0f && s.source == KeySource::locked && pub.heldKey() == 10 && pub.heldIsMinor(),
               "a held key from a session is on show at once, with nothing heard yet");
        Publisher off;
        off.restore (10, true);
        s = off.update (none, listen);
        check (s.key == -1 && off.heldKey() == -1, "a held key with Lock off holds nothing");
    }
}

// ---- key.json: the line itself -----------------------------------------------------------------------------------

static void testLockFileFormat()
{
    using namespace locktests;
    const double when = 1794011292.5;

    KeyState heard { 4, false, 0.78, true };
    heard.altKey = 9;
    heard.altConfidence = 0.41;
    heard.margin = 0.62;
    KeyState unsure { 4, false, 0.44, true };
    unsure.altKey = 9;
    unsure.altConfidence = 0.43;
    unsure.margin = 0.03;
    unsure.unsure = true;
    KeyState manual { 6, true, 1.0, false };
    manual.source = 1;
    KeyState locked { 4, false, 1.0, true };
    locked.locked = true;
    locked.source = 2;
    const KeyState plain { 1, true, 0.87, true };

    const auto heardLine = lineOf (heard, when), unsureLine = lineOf (unsure, when), manualLine = lineOf (manual, when), lockedLine = lineOf (locked, when), plainLine = lineOf (plain, when);
    check (heardLine == "{\"key\":\"E\",\"scale\":\"major\",\"confidence\":0.78,\"time\":1794011292.5,\"listening\":true,"
                        "\"altKey\":\"A\",\"altScale\":\"major\",\"altConfidence\":0.41,\"margin\":0.62,\"unsure\":false,\"locked\":false,\"source\":\"heard\"}",
           "exact format, heard with a runner up: " + heardLine);
    check (unsureLine == "{\"key\":\"E\",\"scale\":\"major\",\"confidence\":0.44,\"time\":1794011292.5,\"listening\":true,"
                         "\"altKey\":\"A\",\"altScale\":\"major\",\"altConfidence\":0.43,\"margin\":0.03,\"unsure\":true,\"locked\":false,\"source\":\"heard\"}",
           "exact format, a close call: " + unsureLine);
    check (manualLine == "{\"key\":\"F#\",\"scale\":\"minor\",\"confidence\":1.00,\"time\":1794011292.5,\"listening\":false,"
                         "\"altKey\":null,\"altScale\":null,\"altConfidence\":0.00,\"margin\":1.00,\"unsure\":false,\"locked\":false,\"source\":\"manual\"}",
           "exact format, set by hand: " + manualLine);
    check (lockedLine == "{\"key\":\"E\",\"scale\":\"major\",\"confidence\":1.00,\"time\":1794011292.5,\"listening\":true,"
                         "\"altKey\":null,\"altScale\":null,\"altConfidence\":0.00,\"margin\":1.00,\"unsure\":false,\"locked\":true,\"source\":\"locked\"}",
           "exact format, locked: " + lockedLine);
    check (plainLine == "{\"key\":\"C#\",\"scale\":\"minor\",\"confidence\":0.87,\"time\":1794011292.5,\"listening\":true,"
                        "\"altKey\":null,\"altScale\":null,\"altConfidence\":0.00,\"margin\":1.00,\"unsure\":false,\"locked\":false,\"source\":\"heard\"}",
           "exact format, heard with no runner up, which is written as null: " + plainLine);

    // No runner up is written the same way whatever was left in the other fields.
    KeyState leftovers = plain;
    leftovers.altMinor = true;
    leftovers.altConfidence = 0.7;
    leftovers.margin = 0.2;
    check (lineOf (leftovers, when) == plainLine, "no runner up is always null, 0.00 and margin 1.00, whatever the other fields held");

    // The front of every line is the 1.6.0 line, byte for byte, up to its closing brace.
    bool frontSame = true, followerSame = true, jsonRight = true;
    size_t longest = 0;
    for (int key = 0; key < 12; ++key)
        for (int minor = 0; minor < 2; ++minor)
            for (int alt = -1; alt < 12; ++alt)
                for (int flags = 0; flags < 12; ++flags)
                {
                    KeyState s { key, minor == 1, (key * 7 + alt + 1) % 101 / 100.0, (flags & 1) != 0 };
                    s.altKey = alt;
                    s.altMinor = (flags & 2) != 0;
                    s.altConfidence = (alt + 3) % 101 / 100.0;
                    s.margin = (key + 5) % 101 / 100.0;
                    s.unsure = alt >= 0 && (flags & 2) != 0;
                    s.source = flags / 4;
                    s.locked = s.source == 2;
                    const auto line = lineOf (s, when + key);
                    const auto old = lineOf160 (s, when + key);
                    longest = std::max (longest, line.size());
                    frontSame = frontSame && line.compare (0, old.size() - 1, old, 0, old.size() - 1) == 0 && line[old.size() - 1] == ',' && line.back() == '}';

                    FollowerData was, now;
                    const bool parsed = followerParse (old.c_str(), was) && followerParse (line.c_str(), now);
                    followerSame = followerSame && parsed && was.key == now.key && was.minor == now.minor && was.confidence == now.confidence
                                   && was.time == now.time && was.listening == now.listening && now.key == key && now.minor == (minor == 1);

                    const auto v = juce::JSON::parse (juce::String (line));
                    const auto* o = v.getDynamicObject();
                    const bool ok = o != nullptr && o->getProperties().size() == 12
                                    && (alt >= 0 ? v["altKey"].toString() == smoothkey::keyNames[alt] && v["altScale"].toString() == (s.altMinor ? "minor" : "major")
                                                 : v["altKey"].isVoid() && v["altScale"].isVoid() && (double) v["altConfidence"] == 0.0 && (double) v["margin"] == 1.0)
                                    && v["unsure"].isBool() && (bool) v["unsure"] == s.unsure && v["locked"].isBool() && (bool) v["locked"] == s.locked
                                    && v["source"].toString() == smoothkey::sourceNames[s.source];
                    jsonRight = jsonRight && ok;
                }
    check (frontSame, "every line begins with the 1.6.0 line byte for byte, and the new fields follow its last one");
    check (followerSame, "the follower's parser reads the same key, scale, confidence, time and listening from the new line as from the old");
    check (jsonRight, "every line is valid JSON with 12 fields, and the new ones hold what was set");

    // The follower finds a field by the first place its quoted name appears. None of the names that were added, and nothing
    // they can hold, may be a quoted name an old or new reader looks for earlier in the line.
    bool firstIsField = true;
    for (auto* line : { &heardLine, &unsureLine, &manualLine, &lockedLine, &plainLine })
        for (auto* name : { "key", "scale", "confidence", "time", "listening", "altKey", "altScale", "altConfidence", "margin", "unsure", "locked", "source" })
            firstIsField = firstIsField && followerFindField (line->c_str(), name) != nullptr;
    check (firstIsField, "each of the 12 names is found as a field by a reader that takes the first place the name appears");

    // The longest line there can be: every longest word, and a time far past any real one.
    KeyState worst { 1, true, 1.0, false };
    worst.altKey = 3;
    worst.altMinor = true;
    worst.altConfidence = 1.0;
    worst.margin = 1.0;
    worst.unsure = true;
    worst.source = 1;
    char line[smoothkey::KeyFile::lineSize];
    const int length = smoothkey::KeyFile::format (line, sizeof line, worst, 9.0e17);
    info ("longest line among all combinations at a real time", (double) longest, "bytes");
    check (length > 0 && length < (int) smoothkey::KeyFile::lineSize && length <= locklimits::followerBufferBytes && juce::JSON::parse (juce::String (line)).isObject(),
           "the longest possible line fits the writer's buffer and a follower's read, bytes", length);
    {
        const auto path = tempJson ("lock-longest.json");
        smoothkey::KeyFile file;
        file.setPath (path.toRawUTF8(), 0.0);
        file.flush (worst, 9.0e17);
        check (! file.hasFailed() && juce::File (path).loadFileAsString().toStdString() == std::string (line), "the longest possible line is really written whole");
    }

    // A host running with comma decimals: the new numbers keep their points too.
    const std::string oldLocale = std::setlocale (LC_NUMERIC, nullptr);
    std::setlocale (LC_NUMERIC, "de_DE.UTF-8");
    const auto comma = lineOf (heard, when);
    std::setlocale (LC_NUMERIC, oldLocale.c_str());
    check (comma == heardLine, "the new fields keep decimal points under a comma locale");

    // When it writes. A runner up that changes is a change. A runner up that only wobbles is not.
    {
        const auto path = tempJson ("lock-rules.json");
        smoothkey::KeyFile file;
        file.setPath (path.toRawUTF8(), 0.5);
        file.update (heard, 0.0, when);
        KeyState wobble = heard;
        wobble.altConfidence += 0.03;
        wobble.margin -= 0.03;
        for (int i = 1; i <= 100; ++i) file.update (wobble, i * 0.1, when);
        const long afterWobble = file.writeCount();
        KeyState other = heard;
        other.altKey = 11;
        file.update (other, 20.0, when);
        const long afterOther = file.writeCount();
        KeyState close = other;
        close.unsure = true;
        file.update (close, 21.0, when);
        const long afterUnsure = file.writeCount();
        KeyState held = other;
        held.locked = true;
        held.source = 2;
        held.altKey = -1;
        held.confidence = 1.0;
        file.update (held, 22.0, when);
        check (afterWobble == 1 && afterOther == 2 && afterUnsure == 3 && file.writeCount() == 4,
               "a new runner up, a close call and a lock are each written once, and a runner up moving by 0.03 is not, writes", (double) file.writeCount());

        // A held key never changes, and a follower ignores a file older than ten minutes. So a held key is written again in time.
        long before = file.writeCount();
        double firstRefresh = 0.0;
        for (double t = 22.5; t <= 22.0 + 2.0 * locklimits::followerMaxAgeSeconds; t += 0.5)
        {
            file.update (held, t, when + t);
            if (firstRefresh == 0.0 && file.writeCount() > before) firstRefresh = t - 22.0;
        }
        const long refreshes = file.writeCount() - before;
        check (firstRefresh > 60.0 && firstRefresh <= locklimits::refreshAtMostSeconds, "a held key is written again, so a follower added later finds a fresh file, after", firstRefresh, "s");
        check (refreshes >= 4 && refreshes <= 20, "and no more often than that needs: writes in 20 minutes of a held key", (double) refreshes);
        const auto v = juce::JSON::parse (juce::File (path).loadFileAsString());
        check ((bool) v["locked"] && (double) v["time"] > when + 600.0, "the time in the file moves on with each of those writes");

        // With Lock off nothing is written again, exactly as before.
        file.update (other, 2000.0, when);
        before = file.writeCount();
        for (double t = 2000.5; t <= 2000.0 + 2.0 * locklimits::followerMaxAgeSeconds; t += 0.5) file.update (other, t, when + t);
        check (file.writeCount() == before, "with Lock off an unchanged key is never written again, writes in 20 minutes", (double) (file.writeCount() - before));
    }
}

// ---- the real processor, its real thread and the real file ---------------------------------------------------------

static void testLockProcessor()
{
    using namespace locktests;
    const Reading eMajor = reading (4, false, 0.8f, 9, false, 0.3f, 0.6f);
    const Reading eOrA = reading (4, false, 0.7f, 9, false, 0.45f, 0.03f, true);   // sent too high on purpose: the plug-in must hold it down
    const Reading gMinor = reading (7, true, 0.7f, 10, false, 0.2f, 0.7f);
    const Reading weak = reading (0, false, 0.3f);
    const Reading sure = reading (2, false, 0.7f);
    const auto path = tempJson ("lock.json");
    juce::File (path).deleteFile();
    const auto json = [] (const juce::String& file, const std::function<bool (const juce::var&)>& want, int ms = 2000)
    {
        return waitFor ([&] { const auto v = readJson (file); return v.isObject() && want (v); }, ms);
    };
    juce::MemoryBlock lockedState;

    {
        PluginProcessor p (path, 0.05);
        prepare (p, 2, 48000.0, 512);

        // The runner up reaches the window's data and the file.
        p.setReadingForTests (&eMajor);
        check (waitFor ([&] { const auto d = p.getDisplay(); return d.key == 4 && d.altKey == 9; }, 1000), "a reading with a runner up reaches what the window reads");
        auto d = p.getDisplay();
        check (! d.minor && ! d.altMinor && std::abs (d.altConfidence - 0.3f) < 1.0e-6f && std::abs (d.margin - 0.6f) < 1.0e-6f && ! d.unsure && ! d.locked && ! d.lockOn && d.heardKey == 4,
               "with its confidence and its margin, and nothing locked");
        check (json (path, [] (const juce::var& v) { return v["key"].toString() == "E" && v["altKey"].toString() == "A" && v["altScale"].toString() == "major"
                                                           && std::abs ((double) v["altConfidence"] - 0.3) < 0.011 && std::abs ((double) v["margin"] - 0.6) < 0.011
                                                           && ! (bool) v["unsure"] && ! (bool) v["locked"] && v["source"].toString() == "heard"; }),
               "key.json names the runner up: " + juce::File (path).loadFileAsString().toStdString());
        check (validRecord (readJson (path)), "that line passes the same checks as every other key.json line in these tests");

        // A close call.
        p.setReadingForTests (&eOrA);
        check (waitFor ([&] { return p.getDisplay().unsure; }, 1000) && p.getDisplay().confidence < (float) locklimits::followerActsFrom,
               "a close call reaches the window's data with its confidence under 0.5", p.getDisplay().confidence);
        check (json (path, [] (const juce::var& v) { return (bool) v["unsure"] && (double) v["confidence"] < locklimits::followerActsFrom && v["altKey"].toString() == "A"; }),
               "key.json says unsure, under the confidence a follower acts on: " + juce::File (path).loadFileAsString().toStdString());
        {
            FollowerData f;
            const auto text = juce::File (path).loadFileAsString();
            check (followerParse (text.toRawUTF8(), f) && f.confidence < locklimits::followerActsFrom, "so a follower reads that line and does not act on it");
        }
        check (readingText (p, "Confidence").getFloatValue() < 50.0f, "and the Confidence reading is under 50 too", readingText (p, "Confidence").getFloatValue());

        // Lock, while E major is on show in that close call.
        const auto started = juce::Time::getMillisecondCounterHiRes();
        setLock (p, true);
        check (waitFor ([&] { return p.getDisplay().locked; }, 1000), "Lock on holds a key");
        d = p.getDisplay();
        check (d.key == 4 && ! d.minor && d.confidence == 1.0f && d.lockOn && ! d.unsure && d.altKey == -1, "Lock on: E major at confidence 1, no runner up, not unsure");
        check (json (path, [] (const juce::var& v) { return v["key"].toString() == "E" && v["scale"].toString() == "major" && (double) v["confidence"] == 1.0
                                                           && (bool) v["locked"] && v["source"].toString() == "locked" && v["altKey"].isVoid() && ! (bool) v["unsure"]; }),
               "key.json says locked, E major, confidence 1.00: " + juce::File (path).loadFileAsString().toStdString());
        const double took = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
        check (took <= locklimits::lockFileSeconds, "Lock reaches key.json within", took, "s");
        {
            FollowerData f;
            const auto text = juce::File (path).loadFileAsString();
            check (followerParse (text.toRawUTF8(), f) && f.key == 4 && ! f.minor && f.confidence >= locklimits::followerActsFrom, "a follower reads the locked key and acts on it");
        }
        check (readingText (p, "Detected Key") == "E" && readingText (p, "Detected Scale") == "Major" && readingText (p, "Confidence").getFloatValue() == 100.0f,
               "the readings say the locked key: " + (readingText (p, "Detected Key") + " " + readingText (p, "Detected Scale") + " " + readingText (p, "Confidence")).toStdString());

        // Other music, and then everything that used to start the reading again from nothing.
        p.setReadingForTests (&gMinor);
        check (waitFor ([&] { return p.getDisplay().heardKey == 7; }, 1000), "locked: what it hears now still reaches the window's data");
        d = p.getDisplay();
        check (d.key == 4 && ! d.minor && d.confidence == 1.0f && d.locked && d.heardMinor, "locked: another key playing does not move the key on show");

        p.setReadingForTests (nullptr);   // the real analyser from here: it has heard nothing, as after Reset
        const auto stillHeld = [&] { const auto now = p.getDisplay(); return now.key == 4 && ! now.minor && now.confidence == 1.0f && now.locked; };
        check (waitFor ([&] { return p.getDisplay().heardKey == -1; }, 1000) && stillHeld(), "locked: the reading underneath back at nothing, and the key stays");
        setParam (p, "reset", 1.0f);
        sleepMs (150);
        setParam (p, "reset", 0.0f);
        p.requestReset();
        sleepMs (150);
        check (stillHeld(), "locked: Reset does not drop the key");
        prepare (p, 2, 44100.0, 256);
        sleepMs (150);
        prepare (p, 2, 96000.0, 1024);
        sleepMs (150);
        check (stillHeld(), "locked: prepareToPlay at two other rates does not drop the key");
        {
            const std::vector<float> silence (48000 * 12, 0.0f);
            feed (p, silence, 96000.0, 24.0);
            sleepMs (900);   // and the host not calling at all, which the plug-in treats as more silence
        }
        check (stillHeld(), "locked: a long silence, and the host stopping its calls, do not drop the key");
        {
            const auto v = readJson (path);
            check (v["key"].toString() == "E" && (double) v["confidence"] == 1.0 && (bool) v["locked"], "key.json still says E major, locked, confidence 1.00 after all of that");
        }
        check (readingText (p, "Detected Key") == "E" && readingText (p, "Confidence").getFloatValue() == 100.0f, "and so do the readings");

        // Saved with the session.
        p.getStateInformation (lockedState);
        auto xml = juce::AudioProcessor::getXmlFromBinary (lockedState.getData(), (int) lockedState.getSize());
        check (xml != nullptr && xml->getIntAttribute ("lock", -1) == 1 && xml->getIntAttribute ("lockedKey", -1) == 4 && xml->getIntAttribute ("lockedScale", -1) == 0,
               "the session holds Lock on and the held key and scale");

        // Unlocked: live again.
        p.setReadingForTests (&gMinor);
        setLock (p, false);
        check (waitFor ([&] { const auto now = p.getDisplay(); return ! now.locked && now.key == 7 && now.minor; }, 1000) && std::abs (p.getDisplay().confidence - 0.7f) < 1.0e-6f
                   && p.getDisplay().altKey == 10,
               "Lock off: back to what it hears, with its own confidence and runner up");
        check (json (path, [] (const juce::var& v) { return v["key"].toString() == "G" && v["scale"].toString() == "minor" && ! (bool) v["locked"] && v["source"].toString() == "heard"; }),
               "Lock off: key.json says heard again");
        juce::MemoryBlock unlocked;
        p.getStateInformation (unlocked);
        xml = juce::AudioProcessor::getXmlFromBinary (unlocked.getData(), (int) unlocked.getSize());
        check (xml != nullptr && xml->getNumAttributes() == 2 && ! xml->hasAttribute ("lock") && ! xml->hasAttribute ("lockedKey"),
               "with Lock off the session is saved exactly as before, with nothing about Lock in it");
        p.setReadingForTests (nullptr);
    }

    // The session comes back: the key is on show and in the file before any audio has been heard.
    {
        const auto path2 = tempJson ("lock-restored.json");
        juce::File (path2).deleteFile();
        PluginProcessor q (path2, 0.05);
        q.setStateInformation (lockedState.getData(), (int) lockedState.getSize());
        juce::MemoryBlock again;
        q.getStateInformation (again);   // straight away, before the background thread has seen it
        const auto xml = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
        check (xml != nullptr && xml->getIntAttribute ("lock", -1) == 1 && xml->getIntAttribute ("lockedKey", -1) == 4 && xml->getIntAttribute ("lockedScale", -1) == 0,
               "a session saved again straight after loading still holds the key");
        prepare (q, 2, 48000.0, 512);
        check (waitFor ([&] { const auto d = q.getDisplay(); return d.locked && d.key == 4 && ! d.minor && d.confidence == 1.0f && d.lockOn; }, 1000),
               "a loaded session shows the held key at confidence 1 with nothing heard");
        check (json (path2, [] (const juce::var& v) { return v["key"].toString() == "E" && v["scale"].toString() == "major" && (double) v["confidence"] == 1.0
                                                            && (bool) v["locked"] && v["source"].toString() == "locked"; }),
               "and writes it to key.json: " + juce::File (path2).loadFileAsString().toStdString());
        q.setReadingForTests (&gMinor);
        sleepMs (150);
        check (q.getDisplay().key == 4 && q.getDisplay().locked, "and keeps it when music in another key plays");
        q.setReadingForTests (nullptr);

        // Minor comes back as minor.
        auto minorXml = juce::AudioProcessor::getXmlFromBinary (lockedState.getData(), (int) lockedState.getSize());
        minorXml->setAttribute ("lockedKey", 10);
        minorXml->setAttribute ("lockedScale", 1);
        juce::MemoryBlock minorState;
        juce::AudioProcessor::copyXmlToBinary (*minorXml, minorState);
        q.setStateInformation (minorState.getData(), (int) minorState.getSize());
        check (waitFor ([&] { const auto d = q.getDisplay(); return d.locked && d.key == 10 && d.minor; }, 1000), "loading a session that held A# minor into a plug-in holding E major shows A# minor");

        // And a session with Lock off, loaded into a locked plug-in, unlocks it.
        juce::MemoryBlock off;
        {
            PluginProcessor plain (tempJson ("lock-plain.json"));
            plain.getStateInformation (off);
        }
        q.setStateInformation (off.getData(), (int) off.getSize());
        check (waitFor ([&] { const auto d = q.getDisplay(); return ! d.locked && ! d.lockOn && d.key == -1; }, 1000), "loading a session with Lock off into a locked plug-in unlocks it");
    }

    // A session saved by 1.6.0. This is the text 1.6.0 wrote, with every control off its default.
    {
        const char* text160 = "<STATE windowWidth=\"1280\" advancedOpen=\"1\">"
                              "<PARAM id=\"bypass\" value=\"0.0\"/><PARAM id=\"listening\" value=\"0.0\"/><PARAM id=\"manualKey\" value=\"7.0\"/>"
                              "<PARAM id=\"manualScale\" value=\"1.0\"/><PARAM id=\"mode\" value=\"1.0\"/><PARAM id=\"reset\" value=\"0.0\"/>"
                              "<PARAM id=\"response\" value=\"0.0\"/><PARAM id=\"source\" value=\"1.0\"/><PARAM id=\"swapRelative\" value=\"1.0\"/>"
                              "<PARAM id=\"tempoScale\" value=\"2.0\"/></STATE>";
        const auto xml = juce::parseXML (text160);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        const auto path3 = tempJson ("lock-160.json");
        juce::File (path3).deleteFile();
        PluginProcessor r (path3, 0.05);
        r.setStateInformation (old.getData(), (int) old.getSize());
        const auto plainOf = [&r] (const char* id) { return juce::roundToInt (r.apvts.getParameter (id)->convertFrom0to1 (r.apvts.getParameter (id)->getValue())); };
        check (plainOf ("bypass") == 0 && plainOf ("listening") == 0 && plainOf ("manualKey") == 7 && plainOf ("manualScale") == 1 && plainOf ("mode") == 1 && plainOf ("reset") == 0
                   && plainOf ("response") == 0 && plainOf ("source") == 1 && plainOf ("swapRelative") == 1 && plainOf ("tempoScale") == 2
                   && r.windowWidth.load() == 1280 && r.advancedOpen.load(),
               "a 1.6.0 session loads every control and the window as it was");
        check (r.getLockParameter().getValue() == 0.0f, "a 1.6.0 session loads with Lock off");
        prepare (r, 2, 48000.0, 512);
        check (waitFor ([&] { const auto d = r.getDisplay(); return d.key == 7 && d.minor && d.manual && d.confidence == 1.0f; }, 1000) && ! r.getDisplay().locked && ! r.getDisplay().lockOn,
               "and shows what 1.6.0 showed: G minor, set by hand, nothing locked");
        check (json (path3, [] (const juce::var& v) { return v["key"].toString() == "G" && v["scale"].toString() == "minor" && (double) v["confidence"] == 1.0
                                                            && ! (bool) v["listening"] && ! (bool) v["locked"] && v["source"].toString() == "manual"; }),
               "and writes what 1.6.0 wrote, then the new fields: " + juce::File (path3).loadFileAsString().toStdString());
        juce::MemoryBlock back;
        r.getStateInformation (back);
        const auto saved = juce::AudioProcessor::getXmlFromBinary (back.getData(), (int) back.getSize());
        bool sameParams = saved != nullptr && saved->getNumChildElements() == xml->getNumChildElements() && saved->getNumAttributes() == 2;
        if (sameParams)
            for (auto* e : xml->getChildIterator())
            {
                auto* match = saved->getChildByAttribute ("id", e->getStringAttribute ("id"));
                sameParams = sameParams && match != nullptr && match->getDoubleAttribute ("value") == e->getDoubleAttribute ("value");
            }
        check (sameParams, "saved again, it holds the same ten values and nothing about Lock");
    }

    // A damaged session cannot hold a key that is not a key, or switch Lock on with a value that is not 1.
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (lockedState.getData(), (int) lockedState.getSize());
        bool safe = true;
        for (const char* bad : { "99", "-3", "nan", "garbage", "1e30", "12", "4.5", "" })
        {
            xml->setAttribute ("lockedKey", bad);
            juce::MemoryBlock damaged;
            juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
            PluginProcessor s (tempJson ("lock-damaged.json"), 0.05);
            s.setStateInformation (damaged.getData(), (int) damaged.getSize());
            prepare (s, 2, 48000.0, 512);
            sleepMs (120);
            const auto d = s.getDisplay();
            const bool ok = d.lockOn && ! d.locked && d.key == -1;
            if (! ok) std::printf ("  lockedKey %s gave key %d locked %d\n", bad, d.key, (int) d.locked);
            safe = safe && ok;
        }
        check (safe, "a damaged held key in a session holds nothing: Lock is on and waits");

        xml->setAttribute ("lockedKey", 4);
        bool offUnlessOne = true;
        for (const char* bad : { "garbage", "nan", "2", "-1", "" })
        {
            xml->setAttribute ("lock", bad);
            juce::MemoryBlock damaged;
            juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
            PluginProcessor s (tempJson ("lock-damaged.json"), 0.05);
            s.setStateInformation (damaged.getData(), (int) damaged.getSize());
            prepare (s, 2, 48000.0, 512);
            sleepMs (120);
            offUnlessOne = offUnlessOne && s.getLockParameter().getValue() == 0.0f && ! s.getDisplay().locked && s.getDisplay().key == -1;
        }
        check (offUnlessOne, "a damaged Lock value in a session leaves Lock off and nothing held");
    }

    // Lock switched on before there is a key.
    {
        const auto path4 = tempJson ("lock-waiting.json");
        juce::File (path4).deleteFile();
        PluginProcessor w (path4, 0.05);
        prepare (w, 2, 48000.0, 512);
        setLock (w, true);
        sleepMs (200);
        auto d = w.getDisplay();
        check (d.lockOn && ! d.locked && d.key == -1 && ! juce::File (path4).exists(), "Lock on with no key yet: nothing is held and nothing is written");
        juce::MemoryBlock waitingState;
        w.getStateInformation (waitingState);
        const auto xml = juce::AudioProcessor::getXmlFromBinary (waitingState.getData(), (int) waitingState.getSize());
        check (xml != nullptr && xml->getIntAttribute ("lock", -1) == 1 && ! xml->hasAttribute ("lockedKey"), "a waiting Lock is saved as on, with no key");
        w.setReadingForTests (&weak);
        const bool shown = waitFor ([&] { return w.getDisplay().key == 0; }, 1000);
        sleepMs (150);
        d = w.getDisplay();
        check (shown && ! d.locked && std::abs (d.confidence - 0.3f) < 1.0e-6f, "a weak first reading is shown as it is, and not held");
        w.setReadingForTests (&eOrA);
        const bool unsureShown = waitFor ([&] { return w.getDisplay().unsure; }, 1000);
        sleepMs (150);
        check (unsureShown && ! w.getDisplay().locked, "a close call is shown as it is, and not held");
        w.setReadingForTests (&sure);
        check (waitFor ([&] { const auto now = w.getDisplay(); return now.locked && now.key == 2 && ! now.minor && now.confidence == 1.0f; }, 1000), "the first sure reading is held");
        w.setReadingForTests (&gMinor);
        sleepMs (150);
        check (w.getDisplay().key == 2 && w.getDisplay().locked, "and stays held when the music moves on");
        check (json (path4, [] (const juce::var& v) { return v["key"].toString() == "D" && (bool) v["locked"] && (double) v["confidence"] == 1.0; }), "key.json holds that key, locked");
        w.setReadingForTests (nullptr);
    }

    // Lock in Manual, and with Relative Swap.
    {
        const auto path5 = tempJson ("lock-manual.json");
        PluginProcessor m (path5, 0.05);
        prepare (m, 2, 48000.0, 512);
        setParam (m, "mode", 1.0f);
        setParam (m, "manualKey", 6.0f);
        check (waitFor ([&] { return m.getDisplay().key == 6; }, 1000), "Manual shows F# major");
        setLock (m, true);
        check (waitFor ([&] { return m.getDisplay().locked; }, 1000), "Lock in Manual holds the manual key");
        setParam (m, "manualKey", 2.0f);
        setParam (m, "manualScale", 1.0f);
        sleepMs (200);
        check (m.getDisplay().key == 6 && ! m.getDisplay().minor, "and a new Manual Key does not move it while locked");
        check (json (path5, [] (const juce::var& v) { return v["key"].toString() == "F#" && v["scale"].toString() == "major" && (bool) v["locked"] && v["source"].toString() == "locked"; }),
               "key.json says F# major, locked");
        setLock (m, false);
        check (waitFor ([&] { const auto d = m.getDisplay(); return d.key == 2 && d.minor && ! d.locked; }, 1000)
                   && json (path5, [] (const juce::var& v) { return v["key"].toString() == "D" && v["scale"].toString() == "minor" && ! (bool) v["locked"] && v["source"].toString() == "manual"; }),
               "Lock off in Manual: the Manual Key as it now is, in the window's data and in key.json");

        const Reading cMajor = reading (0, false, 0.8f, 9, true, 0.5f, 0.2f);
        setParam (m, "mode", 0.0f);
        setParam (m, "swapRelative", 1.0f);
        m.setReadingForTests (&cMajor);
        check (waitFor ([&] { const auto d = m.getDisplay(); return d.key == 9 && d.minor && d.altKey == 0 && ! d.altMinor; }, 1000), "Relative Swap shows A minor, with C major as the runner up");
        setLock (m, true);
        const bool heldSwapped = waitFor ([&] { const auto d = m.getDisplay(); return d.locked && d.key == 9 && d.minor; }, 1000);
        setParam (m, "swapRelative", 0.0f);
        sleepMs (200);
        check (heldSwapped && m.getDisplay().key == 9 && m.getDisplay().minor && m.getDisplay().heardKey == 0, "Lock holds A minor as shown, and turning Swap off does not move it");
        m.setReadingForTests (nullptr);
    }
}

// ---- the window ----------------------------------------------------------------------------------------------------

static void testLockWindow()
{
    using namespace locktests;
    using namespace windowtests;
    using namespace keyui;

    const Reading eMajor = reading (4, false, 0.8f, 9, false, 0.3f, 0.6f);
    const Reading eOrA = reading (4, false, 0.44f, 9, false, 0.43f, 0.03f, true);
    const Reading gMinor = reading (7, true, 0.62f, 10, false, 0.2f, 0.7f);
    // The widest two keys there are, to see that nothing is cut off.
    int widest = 0, second = 1;
    {
        std::vector<std::pair<float, int>> widths;
        for (int k = 0; k < 24; ++k)
            widths.push_back ({ textWidth (juce::String (smoothkey::keyNames[k % 12]) + (k >= 12 ? " minor" : " major"), semi, 20.0f), k });
        std::sort (widths.rbegin(), widths.rend());
        widest = widths[0].second;
        second = widths[1].second;
    }
    const Reading wideClose = reading (widest % 12, widest >= 12, 0.44f, second % 12, second >= 12, 1.0f, 0.03f, true);
    const Reading wideRunner = reading (widest % 12, widest >= 12, 1.0f, second % 12, second >= 12, 1.0f, 0.1f);

    const auto path = tempJson ("lock-window.json");
    PluginProcessor p (path, 0.05);
    prepare (p, 2, 48000.0, 512);
    auto ed = openWindow (p);
    auto& panel = ed->getPanel();
    auto* line = findById<SecondLine> (panel, "second");
    auto* use = findById<juce::Button> (panel, "useRunnerUp");
    auto* lock = findById<Pill> (panel, "lock");
    auto* status = findById<Readout> (panel, "status");
    auto* centreSmall = findById<Readout> (panel, "centreSmall");
    auto* hero = findById<HeroView> (panel, "hero");
    auto* keyboardView = findById<KeyboardView> (panel, "keyboard");
    auto* circle = findById<CircleView> (panel, "circle");
    check (line != nullptr && use != nullptr && lock != nullptr && status != nullptr && centreSmall != nullptr && hero != nullptr && keyboardView != nullptr && circle != nullptr,
           "the window has a Lock control, a line for the runner up and a block to take it");
    if (line == nullptr || use == nullptr || lock == nullptr || status == nullptr || centreSmall == nullptr || hero == nullptr || keyboardView == nullptr || circle == nullptr)
        return;

    // Lock is on the top row, in sight without opening Advanced, and nothing was pushed off the page or onto anything else.
    ed->setAdvancedOpen (false, false);
    check (lock->isVisible() && panel.getLocalBounds().contains (lock->getBounds()) && lock->getY() < hero->getY(), "Lock is on the top row, with Advanced shut");
    bool apart = true;
    for (auto* added : { static_cast<juce::Component*> (lock), static_cast<juce::Component*> (line) })
        for (auto* other : panel.getChildren())
            if (other != added && other->isVisible() && added->getBounds().intersects (other->getBounds()))
            {
                apart = false;
                std::printf ("  %s overlaps %s\n", added->getComponentID().toRawUTF8(), other->getComponentID().toRawUTF8());
            }
    check (apart, "Lock and the runner up line overlap nothing else in the window");
    check (line->getY() >= hero->getBottom() && line->getBottom() <= keyboardView->getY() && line->getX() == hero->getX() && line->getRight() == hero->getRight(),
           "the runner up line sits between the reading and the keyboard, as wide as the reading");

    // Whether every word of a state is on screen whole: the status in its box, and the line with its block.
    const auto fits = [&] (const char* state)
    {
        const float statusNeeds = textWidth (ed->getStatusText(), medium, 17.0f) + 16.0f;   // the dot beside it takes 16
        const float lineNeeds = line->wordsWidth() + (line->actionWidth() > 0.0f ? 16.0f + line->actionWidth() : 0.0f);
        const float centreNeeds = textWidth (centreSmall->getText(), book, 15.0f);
        const bool ok = statusNeeds <= (float) status->getWidth() && lineNeeds <= (float) line->getWidth() && centreNeeds <= (float) centreSmall->getWidth()
                        && (! use->isVisible() || (line->getLocalBounds().contains (use->getBounds()) && (float) use->getX() >= line->wordsWidth() + 16.0f));
        if (! ok) std::printf ("  cut off in state %s: status %.0f of %d, line %.0f of %d, centre %.0f of %d\n", state, statusNeeds, status->getWidth(), lineNeeds, line->getWidth(), centreNeeds, centreSmall->getWidth());
        return ok;
    };
    const auto show = [&] (const Reading* r, const std::function<bool (const PluginProcessor::Display&)>& ready)
    {
        p.setReadingForTests (r);
        const bool ok = waitFor ([&] { return ready (p.getDisplay()); }, 1000);
        ed->refresh();
        return ok;
    };
    bool allFit = fits ("no key");

    // A runner up.
    check (show (&eMajor, [] (const PluginProcessor::Display& d) { return d.key == 4 && d.altKey == 9; }), "the window is given E major over A major");
    check (ed->getKeyText() == "E" && ed->getSecondLineText() == "Runner up A major 30%, well behind" && ed->getUseText() == "Use A major" && use->isVisible(),
           "the window names the runner up and how close it is: " + ed->getSecondLineText().toStdString() + " [" + ed->getUseText().toStdString() + "]");
    check (ed->getCentreText() == "80% sure" && ed->getStatusText() == "Hearing audio", "beside the key's own confidence: " + ed->getCentreText().toStdString());
    allFit = fits ("runner up") && allFit;
    {
        const Reading nearer = reading (4, false, 0.6f, 9, false, 0.5f, 0.3f), nearest = reading (4, false, 0.55f, 9, false, 0.52f, 0.1f);
        show (&nearer, [] (const PluginProcessor::Display& d) { return std::abs (d.margin - 0.3f) < 1.0e-6f; });
        const auto close = ed->getSecondLineText();
        show (&nearest, [] (const PluginProcessor::Display& d) { return std::abs (d.margin - 0.1f) < 1.0e-6f; });
        check (close == "Runner up A major 50%, close" && ed->getSecondLineText() == "Runner up A major 52%, very close", "a nearer runner up says so: " + close.toStdString() + " / " + ed->getSecondLineText().toStdString());
    }

    // A close call: both keys named, in red, and no confident answer.
    check (show (&eOrA, [] (const PluginProcessor::Display& d) { return d.unsure; }), "the window is given a close call");
    check (ed->getSecondLineText() == "Not sure E major or A major" && line->get().tagRed && ed->getUseText() == "Use A major",
           "a close call names both keys: " + ed->getSecondLineText().toStdString() + " [" + ed->getUseText().toStdString() + "]");
    check (ed->getCentreText() == "44% not sure" && ed->getStatusText() == "Hearing audio, not sure", "and says not sure in the circle and the status: " + ed->getCentreText().toStdString() + " / " + ed->getStatusText().toStdString());
    allFit = fits ("close call") && allFit;
    show (&wideClose, [&] (const PluginProcessor::Display& d) { return d.unsure && d.key == widest % 12; });
    info ("the widest close call the line can be asked to show, of the line's width", 100.0 * (line->wordsWidth() + 16.0f + line->actionWidth()) / (float) line->getWidth(), "%");
    allFit = fits ("widest close call") && allFit;
    show (&wideRunner, [&] (const PluginProcessor::Display& d) { return ! d.unsure && d.key == widest % 12; });
    allFit = fits ("widest runner up") && allFit;

    // Lock, from the window: one gesture on the real parameter, and a locked state that cannot be missed.
    show (&eOrA, [] (const PluginProcessor::Display& d) { return d.unsure && d.key == 4; });
    {
        ParamWatch w (p.getLockParameter());
        lock->setToggleState (true, juce::sendNotificationSync);
        check (w.oneGesture() && p.getLockParameter().getValue() == 1.0f, "clicking Lock sets the Lock parameter, as one gesture");
    }
    check (show (&gMinor, [] (const PluginProcessor::Display& d) { return d.locked && d.heardKey == 7; }), "the window is given a held key with other music under it");
    check (ed->getKeyText() == "E" && ed->getStatusText() == "Locked" && ed->getCentreText() == "Locked" && ed->getSecondLineText() == "Locked Hears G minor 62%"
               && ! line->get().tagRed && ed->getUseText().isEmpty() && ! use->isVisible() && lock->getToggleState(),
           "locked: the key stays, and the status, the circle and the line all say Locked: " + ed->getSecondLineText().toStdString());
    allFit = fits ("locked") && allFit;
    {
        // The reading is black, not faded, even with no audio: a held key is certain. Count the black in the letters.
        show (nullptr, [] (const PluginProcessor::Display& d) { return d.locked && d.heardKey == -1; });
        check (ed->getSecondLineText() == "Locked Holding this key" && ed->getStatusText() == "Locked", "locked with nothing playing: " + ed->getSecondLineText().toStdString());
        allFit = fits ("locked, silent") && allFit;
        const auto image = ed->createComponentSnapshot (ed->getLocalBounds());
        const float toWindow = (float) ed->getWidth() / (float) KeyEditor::designWidth;
        const auto letters = (ed->getKeyLetterBounds().toFloat() * toWindow).getSmallestIntegerContainer().getIntersection (image.getBounds());
        long black = 0;
        for (int y = letters.getY(); y < letters.getBottom(); y += 2)
            for (int x = letters.getX(); x < letters.getRight(); x += 2)
                if (image.getPixelAt (x, y).getBrightness() < 0.1f) ++black;
        check (black > 500, "locked with nothing playing: the key letters are still black, black samples", (double) black);

        // The Lock block itself is solid black with paper type, unlike any control that is merely on.
        const auto spot = lock->getBounds().toFloat() * toWindow;
        const auto corner = image.getPixelAt (juce::roundToInt (spot.getX() + 6.0f * toWindow), juce::roundToInt (spot.getCentreY()));
        check (corner.getBrightness() < 0.1f, "the Lock control is solid black when on, brightness", corner.getBrightness());
    }
    {
        // Automation moves the control, and the window follows.
        setLock (p, false);
        show (&eMajor, [] (const PluginProcessor::Display& d) { return ! d.locked && d.key == 4; });
        const bool off = ! lock->getToggleState() && ed->getStatusText() == "Hearing audio";
        setLock (p, true);
        show (&eMajor, [] (const PluginProcessor::Display& d) { return d.locked; });
        check (off && lock->getToggleState() && ed->getStatusText() == "Locked", "automation moves Lock and the window follows");
        ParamWatch w (p.getLockParameter());
        lock->mouseDoubleClick (mouseAt (*lock, { 3.0f, 3.0f }, 2));
        check (w.oneGesture() && p.getLockParameter().getValue() == 0.0f, "double-click returns Lock to off");
        show (&eMajor, [] (const PluginProcessor::Display& d) { return ! d.locked && d.key == 4; });
    }

    // Use the runner up: three real parameters, a gesture each, and the other plug-ins get it.
    {
        Watch k (p, "manualKey"), s (p, "manualScale"), m (p, "mode");
        setParam (p, "manualScale", 1.0f);   // so that taking A major has a scale to change
        k.count = {};
        s.count = {};
        use->onClick();
        check (k.oneGesture() && s.oneGesture() && m.oneGesture() && (int) plain (p, "manualKey") == 9 && (int) plain (p, "manualScale") == 0 && (int) plain (p, "mode") == 1,
               "clicking Use A major sets Manual Key, Manual Scale and Mode, as one gesture each");
    }
    check (waitFor ([&] { const auto v = readJson (path); return v.isObject() && v["key"].toString() == "A" && v["scale"].toString() == "major" && (double) v["confidence"] == 1.0
                                                                 && v["source"].toString() == "manual" && v["altKey"].isVoid(); }, 2000),
           "and key.json then says A major, set by hand, at confidence 1.00: " + juce::File (path).loadFileAsString().toStdString());
    ed->refresh();
    check (ed->getKeyText() == "A" && ed->getStatusText() == "Set by hand" && ed->getSecondLineText().isEmpty() && ! use->isVisible(), "the window shows A, set by hand, and offers nothing more");
    allFit = fits ("set by hand") && allFit;

    // Picking a key by hand in the window, start to finish, with no host: Mode, then the circle, and it is in key.json.
    setParam (p, "mode", 0.0f);
    sleepMs (80);
    ed->refresh();
    {
        auto* mode = findById<Segments> (panel, "mode");
        mode->mouseDown (mouseAt (*mode, { mode->getWidth() * 0.75f, 5.0f }));
        const bool manualNow = (int) plain (p, "mode") == 1;
        sleepMs (80);
        ed->refresh();
        circle->mouseDown (mouseAt (*circle, circle->pointForKey (12 + 1)));
        check (manualNow && (int) plain (p, "manualKey") == 1 && (int) plain (p, "manualScale") == 1, "in the window: click Manual, then C# minor on the circle");
        check (waitFor ([&] { const auto v = readJson (path); return v.isObject() && v["key"].toString() == "C#" && v["scale"].toString() == "minor" && (double) v["confidence"] == 1.0
                                                                     && v["source"].toString() == "manual"; }, 2000),
               "and that key reaches key.json for the other plug-ins: " + juce::File (path).loadFileAsString().toStdString());
        keyboardView->mouseDown (mouseAt (*keyboardView, { (float) keyboardView->getWidth() * 4.5f / 7.0f, (float) keyboardView->getHeight() - 5.0f }));
        check ((int) plain (p, "manualKey") == 7, "a click on the keyboard's G sets it too");
    }

    // Locked in Manual: the pickers stop, so a click cannot seem to do nothing.
    {
        show (nullptr, [] (const PluginProcessor::Display& d) { return d.key == 7; });   // G is on show before Lock is clicked
        setLock (p, true);
        show (nullptr, [] (const PluginProcessor::Display& d) { return d.locked && d.key == 7; });
        circle->mouseDown (mouseAt (*circle, circle->pointForKey (3)));
        keyboardView->mouseDown (mouseAt (*keyboardView, { 5.0f, (float) keyboardView->getHeight() - 5.0f }));
        check ((int) plain (p, "manualKey") == 7 && ed->getKeyText() == "G" && ed->getStatusText() == "Locked",
               "locked in Manual: clicking the circle or the keyboard changes nothing: " + ed->getKeyText().toStdString() + ", " + ed->getStatusText().toStdString(), plain (p, "manualKey"));
        check (circle->hint.containsIgnoreCase ("Lock") && keyboardView->hint.containsIgnoreCase ("Lock") && hero->hint.containsIgnoreCase ("Lock"),
               "and the hints there say to turn Lock off: " + circle->hint.toStdString());
        allFit = fits ("locked in Manual") && allFit;
        setLock (p, false);
        setParam (p, "mode", 0.0f);
        show (nullptr, [] (const PluginProcessor::Display& d) { return ! d.locked && ! d.manual; });
    }

    // Waiting.
    {
        PluginProcessor w (tempJson ("lock-window-waiting.json"), 0.05);
        prepare (w, 2, 48000.0, 512);
        setLock (w, true);
        auto waitingEd = openWindow (w);
        sleepMs (120);
        waitingEd->refresh();
        check (waitingEd->getStatusText() == "Lock waiting for a key" && waitingEd->getSecondLineText() == "Lock is on. It will hold the first sure key." && waitingEd->getKeyText() == "--",
               "Lock on with no key yet: the window says it is waiting: " + waitingEd->getStatusText().toStdString() + " / " + waitingEd->getSecondLineText().toStdString());
        auto* waitingLine = findById<SecondLine> (waitingEd->getPanel(), "second");
        auto* waitingStatus = findById<Readout> (waitingEd->getPanel(), "status");
        allFit = allFit && waitingLine->wordsWidth() <= (float) waitingLine->getWidth() && textWidth (waitingEd->getStatusText(), medium, 17.0f) + 16.0f <= (float) waitingStatus->getWidth();
    }
    check (allFit, "no words are cut off in any of those states, at the widest keys there are");

    // The hints for the new parts fit the hint line, or they would silently fall back to the resting hint.
    {
        bool hintsShow = true;
        show (&eMajor, [] (const PluginProcessor::Display& d) { return d.key == 4 && d.altKey == 9 && ! d.locked; });
        for (auto* id : { "lock", "second", "useRunnerUp" })
        {
            auto* c = findById<juce::Component> (panel, id);
            auto* hinted = dynamic_cast<Hinted*> (c);
            const bool ok = hinted != nullptr && hinted->hint.isNotEmpty() && textWidth (hinted->hint, book, 15.0f) <= 846.0f;
            if (! ok) std::printf ("  hint missing or too long for %s\n", id);
            hintsShow = hintsShow && ok;
        }
        check (hintsShow, "Lock, the runner up line and its block each explain themselves in the hint line");
    }

    // The key letters keep their size limit at the smallest window, with the new line under them.
    ed->setSize (PluginProcessor::minWindowWidth, juce::roundToInt ((double) PluginProcessor::minWindowWidth * KeyEditor::designHeight / KeyEditor::designWidth));
    const float smallest = (float) ed->getKeyLetterBounds().getHeight() * (float) ed->getWidth() / (float) KeyEditor::designWidth;
    check (smallest >= 150.0f, "with the runner up line added, key letters are still at least 150 points tall at the smallest window", smallest, "pt");
    p.setReadingForTests (nullptr);
}

// Returns true when only these tests were asked for, with PluginTest --lock.
static bool runLockTests (int argc, char** argv)
{
    testLockParameter();
    testLockRules();
    testLockFileFormat();
    testLockProcessor();
    testLockWindow();
    return argc > 1 && juce::String (argv[1]) == "--lock";
}
