// LinkCheck: the black-box check of an installed plug-in, through the system Audio Unit API the way a host loads it. No JUCE.
//
// Usage: LinkCheck <aufx|aumf|aumu> <subtype> <manufacturer> [--render-sums] [--scratch DIR] [--load BUNDLE] [--keep]
//
// The link check (default) follows switchboard/PROTOCOL.md from outside: the card appears within 3 s; the ContextName
// "LinkCheck Track" reaches the card; the card's parameters are the AU's (count, names, and normalized values within 1e-4 of
// AudioUnitGetParameter mapped through the AU's own min and max); for four parameters (first continuous, first discrete, first
// main, last) a command moves the AU parameter while an AUEventListener sees gesture begin, value, gesture end in that order, and a
// second command restores it; after dispose, card and inbox are gone. One PASS or FAIL line per check, then LINKCHECK PASS or
// LINKCHECK FAIL, exit 0 or 1.
//
// --render-sums: the default state and every factory preset, 8 s at 48 kHz stereo in 512 frame blocks, from a fixed input (a sine
// sweep, LCG noise, a chord of sines and silences) and, for aumf and aumu, a fixed MIDI chord sequence. Each is rendered twice, in
// fresh instances, and prints "SUM <preset> <hash>" (FNV-1a 64 over the output's float bytes) or "NONDET <preset>" when the two
// runs differ. Only SUM, NONDET and FAIL lines go to stdout, so the outputs of two versions compare with diff. The preset is set
// before AudioUnitInitialize; the run loop runs 0.1 s before the render, as a host's would.
//
// Both modes set SMOOTH_LINK_DIR and SMOOTH_KEY_JSON to a fresh scratch folder before any AU is loaded, so Luke's real link folder
// and key.json are never written (Key writes key.json and the tuners read theirs). The folder is made under --scratch DIR, else
// $TMPDIR, and removed afterwards unless --keep is given or the link check failed.
//
// --load BUNDLE registers the AU of a .component bundle in this process only (AudioComponentRegister), to check a build that is not
// installed, such as the Link Probe.
//
// JUCE timers inside a plug-in fire only while the host runs a run loop, so every wait here runs CFRunLoopRunInMode on the main
// thread.
//
// Build: target LinkCheck in switchboard/CMakeLists.txt. Installed copy: ~/smooth-plugins/_shared/bin/LinkCheck
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
// ---- small helpers ------------------------------------------------------------------------------------------------------------

OSType code (const char* s) { return ((OSType) (unsigned char) s[0] << 24) | ((OSType) (unsigned char) s[1] << 16) | ((OSType) (unsigned char) s[2] << 8) | (OSType) (unsigned char) s[3]; }

std::string fourCC (OSType v)
{
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8)
        s += (char) ((v >> shift) & 0xff);
    return s;
}

std::string str (CFStringRef s)
{
    if (s == nullptr)
        return {};
    const CFIndex size = CFStringGetMaximumSizeForEncoding (CFStringGetLength (s), kCFStringEncodingUTF8) + 1;
    std::vector<char> b ((size_t) size, 0);
    CFStringGetCString (s, b.data(), size, kCFStringEncodingUTF8);
    return b.data();
}

double monotonic()
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec / 1.0e9;
}

std::string fmt (const char* f, double v)
{
    char b[64];
    std::snprintf (b, sizeof b, f, v);
    return b;
}

// Runs the main run loop, where the plug-in's JUCE message queue lives. With no source in the loop yet, it just sleeps.
void pump (double seconds)
{
    if (CFRunLoopRunInMode (kCFRunLoopDefaultMode, seconds, false) == kCFRunLoopRunFinished)
        usleep ((useconds_t) (seconds * 1.0e6));
}

bool waitFor (double timeout, const std::function<bool()>& done, double* took = nullptr)
{
    const double start = monotonic();
    for (;;)
    {
        if (done())
        {
            if (took != nullptr)
                *took = monotonic() - start;
            return true;
        }
        if (monotonic() - start >= timeout)
            return false;
        pump (0.01);
    }
}

bool exists (const std::string& path)
{
    struct stat s;
    return lstat (path.c_str(), &s) == 0;
}

// Through one descriptor to the end: a card can be replaced by rename at any moment.
bool readFile (const std::string& path, std::string& out)
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
    }
    close (fd);
    return ok;
}

std::vector<std::string> listDir (const std::string& path)
{
    std::vector<std::string> names;
    if (DIR* d = opendir (path.c_str()))
    {
        while (auto* e = readdir (d))
            if (std::strcmp (e->d_name, ".") != 0 && std::strcmp (e->d_name, "..") != 0)
                names.emplace_back (e->d_name);
        closedir (d);
    }
    std::sort (names.begin(), names.end());
    return names;
}

void removeTree (const std::string& path)
{
    struct stat s;
    if (lstat (path.c_str(), &s) != 0)
        return;
    if (S_ISDIR (s.st_mode))
    {
        for (const auto& n : listDir (path))
            removeTree (path + "/" + n);
        rmdir (path.c_str());
    }
    else
    {
        unlink (path.c_str());
    }
}

bool isHex16 (const std::string& s, size_t at)
{
    if (s.size() < at + 16)
        return false;
    for (size_t i = at; i < at + 16; ++i)
        if (! ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}

// Member cards only: ^[0-9a-f]{16}\.json$
std::vector<std::string> listCards (const std::string& dir)
{
    std::vector<std::string> ids;
    for (const auto& n : listDir (dir))
        if (n.size() == 21 && isHex16 (n, 0) && n.compare (16, 5, ".json") == 0)
            ids.push_back (n.substr (0, 16));
    return ids;
}

// ---- a small JSON reader ------------------------------------------------------------------------------------------------------

struct Json
{
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    bool has (const char* key) const
    {
        for (const auto& kv : o)
            if (kv.first == key)
                return true;
        return false;
    }
    const Json& operator[] (const char* key) const
    {
        static const Json none;
        for (const auto& kv : o)
            if (kv.first == key)
                return kv.second;
        return none;
    }
};

class JsonParser
{
public:
    explicit JsonParser (const std::string& text) : p (text.data()), end (text.data() + text.size()) {}

    bool parse (Json& out)
    {
        ok = true;
        out = value();
        space();
        return ok && p == end;
    }

private:
    void space()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool literal (const char* word)
    {
        const size_t len = std::strlen (word);
        if ((size_t) (end - p) < len || std::strncmp (p, word, len) != 0)
            return false;
        p += len;
        return true;
    }

    static void utf8 (std::string& out, uint32_t cp)
    {
        if (cp < 0x80) out += (char) cp;
        else if (cp < 0x800) { out += (char) (0xc0 | (cp >> 6)); out += (char) (0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) { out += (char) (0xe0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3f)); out += (char) (0x80 | (cp & 0x3f)); }
        else { out += (char) (0xf0 | (cp >> 18)); out += (char) (0x80 | ((cp >> 12) & 0x3f)); out += (char) (0x80 | ((cp >> 6) & 0x3f)); out += (char) (0x80 | (cp & 0x3f)); }
    }

    bool hex4 (uint32_t& v)
    {
        if (end - p < 4)
            return false;
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t) (c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t) (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t) (c - 'A' + 10);
            else return false;
        }
        return true;
    }

    std::string string()
    {
        std::string out;
        ++p;   // the opening quote
        while (p < end && *p != '"')
        {
            if (*p != '\\')
            {
                out += *p++;
                continue;
            }
            if (++p >= end) break;
            const char c = *p++;
            switch (c)
            {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    uint32_t cp = 0;
                    if (! hex4 (cp)) { ok = false; return out; }
                    if (cp >= 0xd800 && cp < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                    {
                        p += 2;
                        uint32_t low = 0;
                        if (! hex4 (low)) { ok = false; return out; }
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    }
                    utf8 (out, cp);
                    break;
                }
                default: ok = false; return out;
            }
        }
        if (p >= end) { ok = false; return out; }
        ++p;   // the closing quote
        return out;
    }

    Json value()
    {
        Json v;
        space();
        if (p >= end) { ok = false; return v; }
        if (*p == '{')
        {
            v.type = Json::Object;
            ++p;
            space();
            if (p < end && *p == '}') { ++p; return v; }
            while (ok)
            {
                space();
                if (p >= end || *p != '"') { ok = false; break; }
                auto key = string();
                space();
                if (p >= end || *p != ':') { ok = false; break; }
                ++p;
                v.o.emplace_back (std::move (key), value());
                space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; break; }
                ok = false;
            }
        }
        else if (*p == '[')
        {
            v.type = Json::Array;
            ++p;
            space();
            if (p < end && *p == ']') { ++p; return v; }
            while (ok)
            {
                v.a.push_back (value());
                space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; break; }
                ok = false;
            }
        }
        else if (*p == '"') { v.type = Json::String; v.s = string(); }
        else if (literal ("true")) { v.type = Json::Bool; v.b = true; }
        else if (literal ("false")) { v.type = Json::Bool; v.b = false; }
        else if (literal ("null")) { v.type = Json::Null; }
        else
        {
            char* stop = nullptr;
            const std::string rest (p, (size_t) std::min<ptrdiff_t> (end - p, 64));
            v.n = std::strtod (rest.c_str(), &stop);
            if (stop == rest.c_str()) { ok = false; return v; }
            v.type = Json::Number;
            p += stop - rest.c_str();
        }
        return v;
    }

    const char* p;
    const char* end;
    bool ok = true;
};

// ---- the card -------------------------------------------------------------------------------------------------------------------

struct CardParam
{
    std::string id, name, text, label;
    int index = 0, steps = 0;
    double value = 0.0, def = 0.0;
    bool automatable = false, main = false;
};

struct Card
{
    int link = 0;
    std::string id, plugin, type, subtype, manufacturer, version, track;
    bool trackNull = true;
    double seq = 0;
    std::vector<CardParam> params;
    Json done;
};

bool loadCard (const std::string& path, Card& c)
{
    std::string text;
    Json j;
    if (! readFile (path, text) || ! JsonParser (text).parse (j) || j.type != Json::Object)
        return false;
    c = Card();
    c.link = (int) j["link"].n;
    c.id = j["id"].s;
    c.plugin = j["plugin"].s;
    c.type = j["type"].s;
    c.subtype = j["subtype"].s;
    c.manufacturer = j["manufacturer"].s;
    c.version = j["version"].s;
    c.trackNull = j["track"].type != Json::String;
    c.track = j["track"].s;
    c.seq = j["seq"].n;
    for (const auto& p : j["params"].a)
    {
        CardParam cp;
        cp.id = p["id"].s;
        cp.name = p["name"].s;
        cp.text = p["text"].s;
        cp.label = p["label"].s;
        cp.index = (int) p["index"].n;
        cp.steps = (int) p["steps"].n;
        cp.value = p["value"].n;
        cp.def = p["default"].n;
        cp.automatable = p["automatable"].b;
        cp.main = p["main"].b;
        c.params.push_back (cp);
    }
    c.done = j["done"];
    return j.has ("params") && j["params"].type == Json::Array;
}

bool findDone (const Card& c, const std::string& name, Json& out)
{
    for (const auto& d : c.done.a)
        if (d["cmd"].s == name)
        {
            out = d;
            return true;
        }
    return false;
}

std::string jsonQuote (const std::string& s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char) c < 0x20) { char b[8]; std::snprintf (b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
    return out + "\"";
}

std::string commandName()
{
    struct timeval tv;
    gettimeofday (&tv, nullptr);
    char b[64];
    std::snprintf (b, sizeof b, "%lld-test-%06x", (long long) tv.tv_sec * 1000LL + tv.tv_usec / 1000, arc4random_uniform (0x1000000));
    return b;
}

// The writer's way from PROTOCOL.md: a hidden temporary name in the inbox, then a rename.
bool sendCommand (const std::string& inbox, const std::string& name, const std::string& json)
{
    const auto tmp = inbox + "/." + name + ".tmp", final = inbox + "/" + name + ".json";
    FILE* f = std::fopen (tmp.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool wrote = std::fwrite (json.data(), 1, json.size(), f) == json.size();
    const bool closed = std::fclose (f) == 0;
    if (! wrote || ! closed || std::rename (tmp.c_str(), final.c_str()) != 0)
    {
        unlink (tmp.c_str());
        return false;
    }
    return true;
}

// ---- the AU side ----------------------------------------------------------------------------------------------------------------

struct AuParam
{
    AudioUnitParameterID id = 0;
    std::string name;
    float min = 0, max = 1;
    UInt32 flags = 0;
};

std::vector<AuParam> auParams (AudioUnit u)
{
    UInt32 sz = 0;
    AudioUnitGetPropertyInfo (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &sz, nullptr);
    std::vector<AudioUnitParameterID> ids (sz / sizeof (AudioUnitParameterID));
    if (! ids.empty())
        AudioUnitGetProperty (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &sz);
    std::vector<AuParam> out;
    for (auto id : ids)
    {
        AudioUnitParameterInfo pi {};
        sz = sizeof pi;
        AudioUnitGetProperty (u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &pi, &sz);
        AuParam p;
        p.id = id;
        p.name = (pi.flags & kAudioUnitParameterFlag_HasCFNameString) ? str (pi.cfNameString) : std::string (pi.name);
        if ((pi.flags & kAudioUnitParameterFlag_HasCFNameString) && (pi.flags & kAudioUnitParameterFlag_CFNameRelease) && pi.cfNameString != nullptr)
            CFRelease (pi.cfNameString);
        p.min = pi.minValue;
        p.max = pi.maxValue;
        p.flags = pi.flags;
        out.push_back (p);
    }
    return out;
}

// The AU's own value, mapped to 0..1 through its min and max.
double auNorm (AudioUnit u, const AuParam& p)
{
    AudioUnitParameterValue v = 0;
    AudioUnitGetParameter (u, p.id, kAudioUnitScope_Global, 0, &v);
    return p.max > p.min ? ((double) v - p.min) / ((double) p.max - p.min) : 0.0;
}

// JUCE's AU parameter number for a parameter ID: String::hashCode of the ID, top bit cleared (JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS).
uint32_t juceHash (const std::string& id)
{
    uint32_t h = 0;
    for (size_t i = 0; i < id.size();)
    {
        const auto c = (unsigned char) id[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xf0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xe0) { cp = c & 0x0f; len = 3; }
        else if (c >= 0xc0) { cp = c & 0x1f; len = 2; }
        for (size_t k = 1; k < len && i + k < id.size(); ++k)
            cp = (cp << 6) | ((unsigned char) id[i + k] & 0x3f);
        i += len;
        h = 31u * h + cp;
    }
    return h;
}

struct Recorder
{
    std::string kinds;
    std::vector<float> values;
};

void eventProc (void* refCon, void*, const AudioUnitEvent* e, UInt64, AudioUnitParameterValue v)
{
    auto* r = static_cast<Recorder*> (refCon);
    switch (e->mEventType)
    {
        case kAudioUnitEvent_BeginParameterChangeGesture: r->kinds += 'b'; break;
        case kAudioUnitEvent_EndParameterChangeGesture: r->kinds += 'e'; break;
        case kAudioUnitEvent_ParameterValueChange: r->kinds += 'v'; r->values.push_back (v); break;
        default: break;
    }
}

// begin, one or more values, end, and nothing else.
bool gesturePattern (const std::string& k)
{
    return k.size() >= 3 && k.front() == 'b' && k.back() == 'e' && k.find_first_not_of ('v', 1) == k.size() - 1;
}

// ---- hosting ----------------------------------------------------------------------------------------------------------------------

constexpr double sampleRate = 48000.0;
constexpr UInt32 blockFrames = 512;
constexpr int renderSeconds = 8;
constexpr long totalFrames = (long) sampleRate * renderSeconds;

struct Input
{
    std::vector<float> l, r;
    long pos = 0;   // the first frame of the block being rendered
};

// Fills every buffer of every input bus: even channels from the left signal, odd ones from the right.
OSStatus inputCallback (void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io)
{
    const auto* in = static_cast<const Input*> (refCon);
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        auto* d = static_cast<float*> (io->mBuffers[b].mData);
        if (d == nullptr)
            continue;
        const auto& src = (b % 2 == 0) ? in->l : in->r;
        const UInt32 n = std::min<UInt32> (frames, io->mBuffers[b].mDataByteSize / sizeof (float));
        for (UInt32 i = 0; i < n; ++i)
        {
            const long at = in->pos + (long) i;
            d[i] = at < (long) src.size() ? src[(size_t) at] : 0.0f;
        }
    }
    return noErr;
}

AudioStreamBasicDescription stereoFloat()
{
    AudioStreamBasicDescription f {};
    f.mSampleRate = sampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    return f;
}

// Formats, block size and input callbacks, as HostCheck does. Returns false with a reason.
bool setUp (AudioUnit u, Input* input, std::string& why)
{
    auto f = stereoFloat();
    UInt32 maxFrames = blockFrames;
    AudioUnitSetProperty (u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    Float64 sr = sampleRate;
    AudioUnitSetProperty (u, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sr, sizeof sr);
    if (const OSStatus st = AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f); st != noErr)
    {
        why = "output format refused, " + std::to_string ((int) st);
        return false;
    }
    UInt32 inBuses = 0, sz = sizeof inBuses;
    AudioUnitGetProperty (u, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &inBuses, &sz);
    for (UInt32 e = 0; e < inBuses; ++e)
    {
        AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, e, &f, sizeof f);
        AURenderCallbackStruct cb { inputCallback, input };
        AudioUnitSetProperty (u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, e, &cb, sizeof cb);
    }
    return true;
}

// ---- the link check ---------------------------------------------------------------------------------------------------------------

struct Report
{
    int fails = 0;
    void line (bool ok, const std::string& what)
    {
        std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        std::fflush (stdout);
        if (! ok)
            ++fails;
    }
    void info (const std::string& what)
    {
        std::printf ("INFO  %s\n", what.c_str());
        std::fflush (stdout);
    }
};

struct CommandResult
{
    bool ok = false;
    std::string note;
    double after = 0.0;
};

// One value command: written, confirmed in done within 3 s, then the AU's own value and the host's events checked.
CommandResult command (AudioUnit u, const AuParam& ap, const std::string& paramId, double value, const std::string& cardPath,
                       const std::string& inbox, Recorder& rec)
{
    CommandResult r;
    rec.kinds.clear();
    rec.values.clear();
    const auto name = commandName();
    char v[64];
    std::snprintf (v, sizeof v, "%.9g", value);
    if (! sendCommand (inbox, name, "{ \"param\": " + jsonQuote (paramId) + ", \"value\": " + v + ", \"gesture\": \"tap\", \"from\": \"test\" }"))
    {
        r.note = "could not write the command";
        return r;
    }
    Json done;
    if (! waitFor (3.0, [&] { Card c; return loadCard (cardPath, c) && findDone (c, name, done); }))
    {
        r.note = "no confirmation in done within 3 s";
        return r;
    }
    pump (0.15);   // the AU's events reach the listener through this run loop
    r.after = auNorm (u, ap);
    const bool confirmed = done["ok"].b && done["value"].type == Json::Number;
    const bool matches = confirmed && std::abs (r.after - done["value"].n) <= 1.0e-4;
    const bool leftover = exists (inbox + "/" + name + ".json");
    r.ok = confirmed && matches && gesturePattern (rec.kinds) && ! leftover;
    if (! confirmed) r.note = "done says ok false: " + done["error"].s;
    else if (! matches) r.note = "AU value " + fmt ("%.6f", r.after) + " but done says " + fmt ("%.6f", done["value"].n);
    else if (! gesturePattern (rec.kinds)) r.note = "host saw '" + rec.kinds + "', not begin, value, end";
    else if (leftover) r.note = "the command file was not deleted";
    return r;
}

int linkCheck (AudioComponent comp, const AudioComponentDescription& desc, const std::string& linkDir)
{
    Report rep;
    AudioUnit u = nullptr;
    if (const OSStatus st = AudioComponentInstanceNew (comp, &u); st != noErr || u == nullptr)
    {
        rep.line (false, "instantiate, error " + std::to_string ((int) st));
        return rep.fails;
    }
    Input silence;
    std::string why;
    if (! setUp (u, &silence, why))
        rep.info (why);
    if (const OSStatus st = AudioUnitInitialize (u); st != noErr)
        rep.info ("initialize returned " + std::to_string ((int) st));

    std::string cardId;
    double took = 0.0;
    const bool appeared = waitFor (3.0, [&] { const auto ids = listCards (linkDir); if (ids.empty()) return false; cardId = ids.front(); return true; }, &took);
    rep.line (appeared, appeared ? "card appears within 3 s (after " + fmt ("%.3f", took) + " s)" : "card appears within 3 s (none in " + linkDir + ")");
    if (! appeared)
    {
        AudioUnitUninitialize (u);
        AudioComponentInstanceDispose (u);
        return rep.fails;
    }
    const auto cardPath = linkDir + "/" + cardId + ".json", inbox = linkDir + "/" + cardId + ".in";
    rep.line (listCards (linkDir).size() == 1, "one card for one instance");

    Card card;
    const bool parsed = loadCard (cardPath, card);
    const bool names = parsed && card.link == 1 && card.id == cardId && card.type == fourCC (desc.componentType)
                       && card.subtype == fourCC (desc.componentSubType) && card.manufacturer == fourCC (desc.componentManufacturer);
    rep.line (names, "card is link 1 and names this AU: " + card.plugin + " " + card.version + ", " + card.type + " " + card.subtype + " " + card.manufacturer);

    CFStringRef context = CFSTR ("LinkCheck Track");
    const OSStatus ctxStatus = AudioUnitSetProperty (u, kAudioUnitProperty_ContextName, kAudioUnitScope_Global, 0, &context, sizeof context);
    const bool named = ctxStatus == noErr && waitFor (3.0, [&] { Card c; return loadCard (cardPath, c) && ! c.trackNull && c.track == "LinkCheck Track"; }, &took);
    rep.line (named, named ? "ContextName \"LinkCheck Track\" reaches the card (after " + fmt ("%.3f", took) + " s)"
                           : "ContextName \"LinkCheck Track\" reaches the card (set returned " + std::to_string ((int) ctxStatus) + ")");

    const auto aps = auParams (u);
    loadCard (cardPath, card);
    rep.line (card.params.size() == aps.size(), "card lists every AU parameter: card " + std::to_string (card.params.size()) + ", AU " + std::to_string (aps.size()));

    // Card parameter to AU parameter: by JUCE's number for the ID, else by name (an old style parameter "#<index>").
    std::vector<int> map (card.params.size(), -1);
    std::vector<bool> used (aps.size(), false);
    std::string nameProblem;
    for (size_t i = 0; i < card.params.size(); ++i)
    {
        const auto& cp = card.params[i];
        int k = -1;
        if (! cp.id.empty() && cp.id[0] != '#')
        {
            const uint32_t h = juceHash (cp.id);
            for (size_t j = 0; j < aps.size() && k < 0; ++j)
                if (aps[j].id == (h & 0x7fffffffu) || aps[j].id == h)
                    k = (int) j;
        }
        if (k < 0 && ! cp.id.empty() && cp.id[0] != '#')
            rep.info ("no AU parameter number for the ID " + cp.id + ", matched by name");
        for (size_t j = 0; j < aps.size() && k < 0; ++j)
            if (! used[j] && aps[j].name == cp.name)
                k = (int) j;
        if (k < 0 || used[(size_t) k])
        {
            if (nameProblem.empty()) nameProblem = "no AU parameter for " + cp.id + " (" + cp.name + ")";
            continue;
        }
        if (aps[(size_t) k].name != cp.name && nameProblem.empty())
            nameProblem = cp.id + " is \"" + cp.name + "\" on the card but \"" + aps[(size_t) k].name + "\" in the AU";
        used[(size_t) k] = true;
        map[i] = k;
    }
    rep.line (nameProblem.empty() && ! card.params.empty(), nameProblem.empty() ? "every card parameter is the AU parameter of that ID, with the same name" : nameProblem);

    double worst = 0.0;
    std::string worstName;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        loadCard (cardPath, card);
        worst = 0.0;
        for (size_t i = 0; i < card.params.size() && i < map.size(); ++i)
            if (map[i] >= 0)
            {
                const double d = std::abs (auNorm (u, aps[(size_t) map[i]]) - card.params[i].value);
                if (d > worst) { worst = d; worstName = card.params[i].name; }
            }
        if (worst <= 1.0e-4)
            break;
        pump (0.4);
    }
    rep.line (worst <= 1.0e-4, "card values are the AU's, mapped through its min and max: worst difference " + fmt ("%.2g", worst) + (worst > 0 ? " (" + worstName + ")" : ""));

    // Four controls: first continuous, first discrete, first main, last. Only those a host can automate and write.
    int firstContinuous = -1, firstDiscrete = -1, firstMain = -1, last = -1;
    for (size_t i = 0; i < card.params.size(); ++i)
    {
        if (map[i] < 0 || ! card.params[i].automatable || ! (aps[(size_t) map[i]].flags & kAudioUnitParameterFlag_IsWritable))
            continue;
        if (card.params[i].steps == 0 && firstContinuous < 0) firstContinuous = (int) i;
        if (card.params[i].steps > 0 && firstDiscrete < 0) firstDiscrete = (int) i;
        if (card.params[i].main && firstMain < 0) firstMain = (int) i;
        last = (int) i;
    }
    rep.line (firstMain >= 0, firstMain >= 0 ? "the card names main controls" : "the card names no main control");
    std::vector<std::pair<int, std::string>> picks;
    const std::pair<int, const char*> wanted[] = { { firstContinuous, "first continuous" }, { firstDiscrete, "first discrete" }, { firstMain, "first main" }, { last, "last" } };
    for (const auto& w : wanted)
    {
        if (w.first < 0)
        {
            rep.info (std::string ("no ") + w.second + " control to command");
            continue;
        }
        auto found = std::find_if (picks.begin(), picks.end(), [&] (const auto& p) { return p.first == w.first; });
        if (found != picks.end())
            found->second += std::string (", ") + w.second;
        else
            picks.emplace_back (w.first, w.second);
    }

    Recorder rec;
    AUEventListenerRef listener = nullptr;
    const OSStatus ls = AUEventListenerCreate (eventProc, &rec, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, 0.0f, 0.0f, &listener);
    if (ls != noErr)
        rep.line (false, "AUEventListenerCreate, error " + std::to_string ((int) ls));
    for (const auto& pick : picks)
    {
        if (listener == nullptr)
            break;
        loadCard (cardPath, card);
        const auto& cp = card.params[(size_t) pick.first];
        const auto& ap = aps[(size_t) map[(size_t) pick.first]];
        AudioUnitEvent events[3] {};
        const AudioUnitEventType types[3] = { kAudioUnitEvent_BeginParameterChangeGesture, kAudioUnitEvent_ParameterValueChange, kAudioUnitEvent_EndParameterChangeGesture };
        for (int k = 0; k < 3; ++k)
        {
            events[k].mEventType = types[k];
            events[k].mArgument.mParameter = { u, ap.id, kAudioUnitScope_Global, 0 };
            AUEventListenerAddEventType (listener, nullptr, &events[k]);
        }
        const double original = cp.value;
        double target = original < 0.5 ? 0.75 : 0.25;
        if (cp.steps > 1)
        {
            const int step = (int) std::lround (original * (cp.steps - 1));
            target = (double) ((step + 1) % cp.steps) / (double) (cp.steps - 1);
        }
        const auto moved = command (u, ap, cp.id, target, cardPath, inbox, rec);
        const auto movedEvents = rec.kinds;
        const bool didMove = moved.ok && std::abs (moved.after - original) > 1.0e-4;
        const auto back = command (u, ap, cp.id, original, cardPath, inbox, rec);
        const bool restored = back.ok && std::abs (back.after - original) <= 1.0e-4;
        std::string what = "command moves " + cp.name + " (" + pick.second + "): " + fmt ("%.4f", original) + " to " + fmt ("%.4f", moved.after)
                           + " and back to " + fmt ("%.4f", back.after) + ", host saw '" + movedEvents + "' then '" + rec.kinds + "'";
        if (! moved.ok) what += "; " + moved.note;
        else if (! didMove) what += "; it did not move";
        if (! back.ok) what += "; restore: " + back.note;
        else if (! restored) what += "; it did not return";
        rep.line (didMove && restored, what);
        for (int k = 0; k < 3; ++k)
            AUEventListenerRemoveEventType (listener, nullptr, &events[k]);
    }
    if (listener != nullptr)
        AUListenerDispose (listener);

    AudioUnitUninitialize (u);
    AudioComponentInstanceDispose (u);
    const bool gone = ! exists (cardPath) && ! exists (inbox);
    rep.line (gone, gone ? "after dispose, card and inbox are gone" : "after dispose, card or inbox is still there");
    for (const auto& n : listDir (linkDir))
        rep.info ("left in the folder: " + n);
    return rep.fails;
}

// ---- render sums ------------------------------------------------------------------------------------------------------------------

// A sweep, silence, LCG noise, silence, a chord of sines, silence. The same every run, from the sample index alone.
void makeInput (Input& in)
{
    in.l.assign ((size_t) totalFrames, 0.0f);
    in.r.assign ((size_t) totalFrames, 0.0f);
    const double twoPi = 6.283185307179586;
    double phaseL = 0.0, phaseR = 0.0;
    const long sweepEnd = 96000, noiseStart = 144000, noiseEnd = 240000, chordStart = 264000, chordEnd = 360000;
    for (long n = 0; n < sweepEnd; ++n)
    {
        const double t = (double) n / (double) sweepEnd;
        const double f = 40.0 * std::pow (12000.0 / 40.0, t);
        phaseL += twoPi * f / sampleRate;
        phaseR += twoPi * f * 1.003 / sampleRate;
        in.l[(size_t) n] = (float) (0.35 * std::sin (phaseL));
        in.r[(size_t) n] = (float) (0.30 * std::sin (phaseR));
    }
    uint32_t xl = 12345u, xr = 67890u;
    for (long n = noiseStart; n < noiseEnd; ++n)
    {
        xl = xl * 1664525u + 1013904223u;
        xr = xr * 1664525u + 1013904223u;
        in.l[(size_t) n] = (float) (((double) (xl >> 8) / 16777216.0 * 2.0 - 1.0) * 0.25);
        in.r[(size_t) n] = (float) (((double) (xr >> 8) / 16777216.0 * 2.0 - 1.0) * 0.25);
    }
    const double chord[] = { 220.0, 277.1826, 329.6276 };
    for (long n = chordStart; n < chordEnd; ++n)
    {
        const double t = (double) (n - chordStart) / sampleRate;
        double l = 0.0, r = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            l += 0.12 * std::sin (twoPi * chord[k] * t);
            r += 0.11 * std::sin (twoPi * chord[k] * t + 0.5 * k);
        }
        in.l[(size_t) n] = (float) l;
        in.r[(size_t) n] = (float) r;
    }
}

struct MidiEvent
{
    long frame;
    UInt32 status, data1, data2;
};

// For aumf and aumu: one chord a second, each held for 0.8 s.
std::vector<MidiEvent> chordSequence()
{
    const std::vector<std::vector<UInt32>> chords = { { 60, 64, 67 }, { 57, 60, 64 }, { 53, 57, 60 }, { 55, 59, 62 },
                                                       { 48, 60, 64, 67 }, { 52, 55, 59 }, { 50, 53, 57 }, { 55, 59, 62, 65 } };
    std::vector<MidiEvent> ev;
    for (size_t c = 0; c < chords.size(); ++c)
    {
        const long on = (long) (c * 48000), off = on + 38400;
        for (const auto note : chords[c])
            ev.push_back ({ on, 0x90, note, 100 });
        for (const auto note : chords[c])
            ev.push_back ({ off, 0x80, note, 0 });
    }
    std::stable_sort (ev.begin(), ev.end(), [] (const MidiEvent& a, const MidiEvent& b) { return a.frame < b.frame; });
    return ev;
}

struct Fnv
{
    uint64_t h = 1469598103934665603ull;
    void add (const void* data, size_t bytes)
    {
        const auto* p = static_cast<const unsigned char*> (data);
        for (size_t i = 0; i < bytes; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
};

struct Preset
{
    int number = -1;   // -1: the default state
    std::string label;
};

std::vector<Preset> factoryPresets (AudioUnit u)
{
    std::vector<Preset> out;
    CFArrayRef presets = nullptr;
    UInt32 sz = sizeof presets;
    if (AudioUnitGetProperty (u, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &sz) == noErr && presets != nullptr)
    {
        for (CFIndex i = 0; i < CFArrayGetCount (presets); ++i)
        {
            const auto* p = static_cast<const AUPreset*> (CFArrayGetValueAtIndex (presets, i));
            out.push_back ({ (int) p->presetNumber, std::to_string ((int) p->presetNumber) + ":" + str (p->presetName) });
        }
        CFRelease (presets);
    }
    return out;
}

// One fresh instance, one preset, 8 s. False with a reason.
bool renderOnce (AudioComponent comp, bool midi, int presetNumber, Input& in, const std::vector<MidiEvent>& events, uint64_t& hash, std::string& why)
{
    AudioUnit u = nullptr;
    if (const OSStatus st = AudioComponentInstanceNew (comp, &u); st != noErr || u == nullptr)
    {
        why = "instantiate, error " + std::to_string ((int) st);
        return false;
    }
    in.pos = 0;
    bool ok = setUp (u, &in, why);
    if (ok && presetNumber >= 0)
    {
        CFArrayRef presets = nullptr;
        UInt32 sz = sizeof presets;
        ok = false;
        if (AudioUnitGetProperty (u, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &sz) == noErr && presets != nullptr)
        {
            for (CFIndex i = 0; i < CFArrayGetCount (presets) && ! ok; ++i)
            {
                const auto* p = static_cast<const AUPreset*> (CFArrayGetValueAtIndex (presets, i));
                if ((int) p->presetNumber == presetNumber)
                {
                    AUPreset chosen = *p;
                    ok = AudioUnitSetProperty (u, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &chosen, sizeof chosen) == noErr;
                }
            }
            CFRelease (presets);
        }
        if (! ok)
            why = "could not set preset " + std::to_string (presetNumber);
    }
    if (ok)
        if (const OSStatus st = AudioUnitInitialize (u); st != noErr)
        {
            why = "initialize, error " + std::to_string ((int) st);
            ok = false;
        }
    if (ok)
    {
        pump (0.1);
        std::vector<float> l (blockFrames), r (blockFrames);
        struct { AudioBufferList list; AudioBuffer extra; } ab {};
        AudioTimeStamp ts {};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        Fnv fnv;
        size_t next = 0;
        for (long start = 0; start < totalFrames && ok; start += blockFrames)
        {
            in.pos = start;
            while (midi && next < events.size() && events[next].frame < start + (long) blockFrames)
            {
                const auto& e = events[next++];
                MusicDeviceMIDIEvent (u, e.status, e.data1, e.data2, (UInt32) (e.frame - start));
            }
            ab.list.mNumberBuffers = 2;
            ab.list.mBuffers[0] = { 1, blockFrames * 4, l.data() };
            ab.list.mBuffers[1] = { 1, blockFrames * 4, r.data() };
            AudioUnitRenderActionFlags flags = 0;
            ts.mSampleTime = (Float64) start;
            if (const OSStatus st = AudioUnitRender (u, &flags, &ts, 0, blockFrames, &ab.list); st != noErr)
            {
                why = "render error " + std::to_string ((int) st) + " at frame " + std::to_string (start);
                ok = false;
                break;
            }
            for (UInt32 b = 0; b < 2; ++b)
                fnv.add (ab.list.mBuffers[b].mData, blockFrames * sizeof (float));
        }
        hash = fnv.h;
        AudioUnitUninitialize (u);
    }
    AudioComponentInstanceDispose (u);
    return ok;
}

int renderSums (AudioComponent comp, const AudioComponentDescription& desc)
{
    Input in;
    makeInput (in);
    Fnv input;
    for (long start = 0; start < totalFrames; start += blockFrames)
    {
        input.add (in.l.data() + start, blockFrames * sizeof (float));
        input.add (in.r.data() + start, blockFrames * sizeof (float));
    }
    std::fprintf (stderr, "INPUT %016llx\n", (unsigned long long) input.h);

    const bool midi = desc.componentType == code ("aumf") || desc.componentType == code ("aumu");
    const auto events = chordSequence();
    std::vector<Preset> presets { { -1, "default" } };
    {
        AudioUnit u = nullptr;
        if (AudioComponentInstanceNew (comp, &u) != noErr || u == nullptr)
        {
            std::printf ("FAIL  instantiate\n");
            return 1;
        }
        const auto factory = factoryPresets (u);
        presets.insert (presets.end(), factory.begin(), factory.end());
        AudioComponentInstanceDispose (u);
    }
    int fails = 0;
    for (const auto& p : presets)
    {
        uint64_t a = 0, b = 0;
        std::string why;
        if (! renderOnce (comp, midi, p.number, in, events, a, why) || ! renderOnce (comp, midi, p.number, in, events, b, why))
        {
            std::printf ("FAIL  %s %s\n", p.label.c_str(), why.c_str());
            ++fails;
            continue;
        }
        if (a == b)
            std::printf ("SUM %s %016llx\n", p.label.c_str(), (unsigned long long) a);
        else
        {
            std::printf ("NONDET %s\n", p.label.c_str());
            std::fprintf (stderr, "NONDET %s: %016llx then %016llx\n", p.label.c_str(), (unsigned long long) a, (unsigned long long) b);
        }
        std::fflush (stdout);
    }
    return fails;
}

// ---- in-process registration of a bundle (--load) ----------------------------------------------------------------------------------

bool registerBundle (const std::string& path, std::string& why)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.c_str(), (CFIndex) path.size(), true);
    CFBundleRef bundle = url != nullptr ? CFBundleCreate (nullptr, url) : nullptr;
    if (url != nullptr)
        CFRelease (url);
    if (bundle == nullptr)
    {
        why = "not a bundle: " + path;
        return false;
    }
    auto* list = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (bundle, CFSTR ("AudioComponents"));
    if (list == nullptr || CFGetTypeID (list) != CFArrayGetTypeID() || CFArrayGetCount (list) < 1)
    {
        why = "no AudioComponents in Info.plist";
        return false;
    }
    auto* entry = (CFDictionaryRef) CFArrayGetValueAtIndex (list, 0);
    const auto get = [&] (const char* key) { return str ((CFStringRef) CFDictionaryGetValue (entry, CFStringCreateWithCString (nullptr, key, kCFStringEncodingUTF8))); };
    const auto type = get ("type"), subtype = get ("subtype"), manu = get ("manufacturer"), factory = get ("factoryFunction"), name = get ("name");
    SInt32 version = 0;
    if (auto* num = (CFNumberRef) CFDictionaryGetValue (entry, CFSTR ("version")))
        CFNumberGetValue (num, kCFNumberSInt32Type, &version);
    if (type.size() != 4 || subtype.size() != 4 || manu.size() != 4 || factory.empty())
    {
        why = "incomplete AudioComponents entry";
        return false;
    }
    if (! CFBundleLoadExecutable (bundle))
    {
        why = "could not load the bundle's executable";
        return false;
    }
    CFStringRef factoryName = CFStringCreateWithCString (nullptr, factory.c_str(), kCFStringEncodingUTF8);
    auto fn = (AudioComponentFactoryFunction) CFBundleGetFunctionPointerForName (bundle, factoryName);
    CFRelease (factoryName);
    if (fn == nullptr)
    {
        why = "no factory function " + factory;
        return false;
    }
    AudioComponentDescription d {};
    d.componentType = code (type.c_str());
    d.componentSubType = code (subtype.c_str());
    d.componentManufacturer = code (manu.c_str());
    CFStringRef cfName = CFStringCreateWithCString (nullptr, name.c_str(), kCFStringEncodingUTF8);
    const AudioComponent c = AudioComponentRegister (&d, cfName, (UInt32) version, fn);
    CFRelease (cfName);
    if (c == nullptr)
    {
        why = "AudioComponentRegister failed";
        return false;
    }
    return true;
}

int usage()
{
    std::fprintf (stderr, "usage: LinkCheck <aufx|aumf|aumu> <subtype> <manufacturer> [--render-sums] [--scratch DIR] [--load BUNDLE] [--keep]\n");
    return 2;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 4 || std::strlen (argv[1]) != 4 || std::strlen (argv[2]) != 4 || std::strlen (argv[3]) != 4)
        return usage();
    bool sums = false, keep = false;
    std::string scratchBase, load;
    for (int i = 4; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--render-sums") sums = true;
        else if (a == "--keep") keep = true;
        else if (a == "--scratch" && i + 1 < argc) scratchBase = argv[++i];
        else if (a == "--load" && i + 1 < argc) load = argv[++i];
        else return usage();
    }
    if (scratchBase.empty())
    {
        const char* t = std::getenv ("TMPDIR");
        scratchBase = (t != nullptr && t[0] == '/') ? t : "/tmp";
    }
    while (scratchBase.size() > 1 && scratchBase.back() == '/')
        scratchBase.pop_back();
    if (scratchBase.empty() || scratchBase[0] != '/')
    {
        std::fprintf (stderr, "LinkCheck: --scratch needs an absolute folder\n");
        return 2;
    }
    mkdir (scratchBase.c_str(), 0755);
    std::string scratch = scratchBase + "/linkcheck.XXXXXX";
    if (mkdtemp (scratch.data()) == nullptr)
    {
        std::fprintf (stderr, "LinkCheck: could not make a scratch folder under %s\n", scratchBase.c_str());
        return 2;
    }
    const std::string linkDir = scratch + "/link", keyJson = scratch + "/key.json";
    // Before any AU is loaded: the plug-ins read both once, as they are made.
    setenv ("SMOOTH_LINK_DIR", linkDir.c_str(), 1);
    setenv ("SMOOTH_KEY_JSON", keyJson.c_str(), 1);
    std::fprintf (stderr, "LinkCheck: scratch %s\n", scratch.c_str());

    if (! load.empty())
    {
        std::string why;
        if (! registerBundle (load, why))
        {
            std::printf ("FAIL  --load: %s\n", why.c_str());
            return 1;
        }
    }

    AudioComponentDescription d {};
    d.componentType = code (argv[1]);
    d.componentSubType = code (argv[2]);
    d.componentManufacturer = code (argv[3]);
    AudioComponent comp = AudioComponentFindNext (nullptr, &d);
    if (comp == nullptr)
    {
        std::printf ("FAIL  component %s %s %s not found\n", argv[1], argv[2], argv[3]);
        if (! sums)
            std::printf ("LINKCHECK FAIL\n");
        removeTree (scratch);
        return 1;
    }
    CFStringRef nameRef = nullptr;
    AudioComponentCopyName (comp, &nameRef);
    std::fprintf (stderr, "LinkCheck: %s\n", str (nameRef).c_str());
    if (nameRef != nullptr)
        CFRelease (nameRef);

    int fails = 0;
    if (sums)
    {
        fails = renderSums (comp, d);
    }
    else
    {
        fails = linkCheck (comp, d, linkDir);
        std::printf ("LINKCHECK %s\n", fails == 0 ? "PASS" : "FAIL");
        std::fflush (stdout);
    }
    if (keep || (! sums && fails > 0))
        std::fprintf (stderr, "LinkCheck: kept %s\n", scratch.c_str());
    else
        removeTree (scratch);
    return fails == 0 ? 0 : 1;
}
