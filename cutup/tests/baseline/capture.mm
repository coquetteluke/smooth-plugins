// Captures sessions saved by the installed Cutup, and what each plays, for the Phase 2 test that new builds load them exactly.
// Each seed (written by `PluginTest seedstates <dir>` at 1.2.0) goes into the installed component through the class info, a factory
// preset is chosen there when asked, and the state the component then saves is kept. A second instance restores that state and plays
// the score in tests/Phase2Tests.h; the left then right channel are hashed the way that test hashes them.
// Built and run once on 2026-11-13 against the installed 1.2.0, before 1.2.1 replaced it:
//   clang++ -std=c++17 -O1 -fobjc-arc tests/baseline/capture.mm -framework AudioToolbox -framework Foundation -o build/capture
//   build/capture <seed dir> tests/baseline <dir for the raw renders>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

static OSType code (const char* s) { return ((OSType) s[0] << 24) | ((OSType) s[1] << 16) | ((OSType) s[2] << 8) | (OSType) s[3]; }

static std::vector<char> readFile (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    return std::vector<char> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
}

struct Host
{
    AudioUnit unit = nullptr;
    static constexpr UInt32 frames = 512;

    bool open()
    {
        AudioComponentDescription d {};
        d.componentType = code ("aumu"); d.componentSubType = code ("SSlc"); d.componentManufacturer = code ("Luke");
        AudioComponent c = AudioComponentFindNext (nullptr, &d);
        if (c == nullptr || AudioComponentInstanceNew (c, &unit) != noErr) return false;
        AudioStreamBasicDescription f {};
        f.mSampleRate = 48000; f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
        f.mBytesPerPacket = f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
        UInt32 maxFrames = frames;
        AudioUnitSetProperty (unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
        AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
        return AudioUnitInitialize (unit) == noErr;
    }

    bool setState (const std::vector<char>& blob)
    {
        CFPropertyListRef info = nullptr;
        UInt32 size = sizeof info;
        if (AudioUnitGetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info, &size) != noErr) return false;
        CFMutableDictionaryRef copy = CFDictionaryCreateMutableCopy (nullptr, 0, (CFDictionaryRef) info);
        CFDataRef data = CFDataCreate (nullptr, (const UInt8*) blob.data(), (CFIndex) blob.size());
        CFDictionarySetValue (copy, CFSTR ("jucePluginState"), data);
        CFPropertyListRef out = copy;
        const OSStatus st = AudioUnitSetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &out, sizeof out);
        CFRelease (data); CFRelease (copy); CFRelease (info);
        return st == noErr;
    }

    std::vector<char> getState()
    {
        CFPropertyListRef info = nullptr;
        UInt32 size = sizeof info;
        std::vector<char> result;
        if (AudioUnitGetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info, &size) != noErr) return result;
        if (auto data = (CFDataRef) CFDictionaryGetValue ((CFDictionaryRef) info, CFSTR ("jucePluginState")))
            result.assign ((const char*) CFDataGetBytePtr (data), (const char*) CFDataGetBytePtr (data) + CFDataGetLength (data));
        CFRelease (info);
        return result;
    }
};

static unsigned long long fnv (unsigned long long h, const std::vector<float>& x)
{
    for (float v : x)
    {
        unsigned int w;
        std::memcpy (&w, &v, 4);
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}

int main (int argc, char** argv)
{
    if (argc != 4) { std::fprintf (stderr, "usage: capture <seed dir> <out dir> <render dir>\n"); return 2; }
    const std::string seeds = argv[1], outDir = argv[2], renderDir = argv[3];
    struct Config { std::string name, seed; int preset; };
    std::vector<Config> configs { { "default", "seed-default.state", -1 }, { "manual", "seed-manual.state", -1 }, { "grid", "seed-grid.state", -1 } };
    for (int i = 0; i < 14; ++i) configs.push_back ({ "preset" + std::to_string (i), "seed-default.state", i });

    FILE* list = std::fopen ((outDir + "/v1.2.0-renders.txt").c_str(), "w");
    for (const auto& c : configs)
    {
        Host a;
        if (! a.open() || ! a.setState (readFile (seeds + "/" + c.seed))) { std::fprintf (stderr, "seed failed: %s\n", c.name.c_str()); return 3; }
        if (c.preset >= 0)
        {
            AUPreset p { c.preset, nullptr };
            if (AudioUnitSetProperty (a.unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof p) != noErr) return 4;
        }
        const auto state = a.getState();
        { std::ofstream o (outDir + "/v1.2.0-" + c.name + ".state", std::ios::binary); o.write (state.data(), (std::streamsize) state.size()); }

        Host b;
        if (! b.open() || ! b.setState (state)) return 5;
        std::vector<float> left, right, l (Host::frames), r (Host::frames);
        AudioTimeStamp ts {};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        for (int block = 0; block < 200; ++block)
        {
            for (int k = 0; k < 16; ++k)
            {
                if (block == 4 + k * 10)     MusicDeviceMIDIEvent (b.unit, 0x90, (UInt32) (60 + k), 100, 0);
                if (block == 4 + k * 10 + 6) MusicDeviceMIDIEvent (b.unit, 0x80, (UInt32) (60 + k), 0, 0);
            }
            struct { AudioBufferList list; AudioBuffer extra; } ab;
            ab.list.mNumberBuffers = 2;
            ab.list.mBuffers[0] = { 1, Host::frames * 4, l.data() };
            ab.list.mBuffers[1] = { 1, Host::frames * 4, r.data() };
            ts.mSampleTime = (Float64) block * Host::frames;
            AudioUnitRenderActionFlags flags = 0;
            if (AudioUnitRender (b.unit, &flags, &ts, 0, Host::frames, &ab.list) != noErr) return 6;
            left.insert (left.end(), l.begin(), l.end());
            right.insert (right.end(), r.begin(), r.end());
        }
        const unsigned long long h = fnv (fnv (14695981039346656037ull, left), right);
        float top = 0.0f;
        for (float v : left) top = std::max (top, std::abs (v));
        std::fprintf (list, "%s %llx\n", c.name.c_str(), h);
        std::fprintf (stderr, "%s: %zu bytes, peak %.3f, hash %llx\n", c.name.c_str(), state.size(), top, h);
        std::ofstream o (renderDir + "/v1.2.0-" + c.name + ".f32", std::ios::binary);
        o.write ((const char*) left.data(), (std::streamsize) (left.size() * 4));
        o.write ((const char*) right.data(), (std::streamsize) (right.size() * 4));
    }
    std::fclose (list);
    std::fflush (stderr);
    _exit (0);   // tearing the instances down from a tool with no message loop traps in JUCE, so leave here
}
