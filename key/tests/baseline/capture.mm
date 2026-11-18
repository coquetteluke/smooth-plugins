// Reads the state the installed Key saves (kAudioUnitProperty_ClassInfo, key jucePluginState) after setting controls by name
// through the Audio Unit, as a host would. It also loads that state into a second instance of the same build and writes what that
// one saves back, so the test can tell what the installed version itself kept.
// Built and run once on 2026-11-13 against the installed 1.7.1, before 1.7.2 replaced it (after Duo's tests/baseline/capture.mm):
//   clang++ -std=c++17 -O1 -fobjc-arc capture.mm -framework AudioToolbox -framework Foundation -o capture
// Usage: capture <out folder> <name> [preset number or -1] [Name=value ...] [lock] [Name=value ...]
//        Controls are set in order; "lock" switches Lock on there, after a pause so the background thread has seen the rest.
// SMOOTH_KEY_JSON must point at a scratch file: the plug-in writes key.json while it runs.
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#define CHECK(x) do { OSStatus e = (x); if (e != noErr) { fprintf (stderr, "%s failed: %d\n", #x, (int) e); return 1; } } while (0)

static bool findParam (AudioUnit u, const char* name, AudioUnitParameterID& out)
{
    UInt32 size = 0;
    if (AudioUnitGetPropertyInfo (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr) != noErr) return false;
    std::vector<AudioUnitParameterID> ids (size / sizeof (AudioUnitParameterID));
    if (AudioUnitGetProperty (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size) != noErr) return false;
    for (auto id : ids)
    {
        AudioUnitParameterInfo info {};
        UInt32 s = sizeof (info);
        if (AudioUnitGetProperty (u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &s) != noErr) continue;
        char text[256] = {};
        if (info.cfNameString != nullptr) CFStringGetCString (info.cfNameString, text, sizeof text, kCFStringEncodingUTF8);
        if (std::strcmp (text, name) == 0) { out = id; return true; }
    }
    return false;
}

static void dumpParams (AudioUnit u, FILE* f)
{
    UInt32 size = 0;
    AudioUnitGetPropertyInfo (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr);
    std::vector<AudioUnitParameterID> ids (size / sizeof (AudioUnitParameterID));
    AudioUnitGetProperty (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size);
    for (auto id : ids)
    {
        AudioUnitParameterInfo info {};
        UInt32 s = sizeof (info);
        AudioUnitGetProperty (u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &s);
        char text[256] = {};
        if (info.cfNameString != nullptr) CFStringGetCString (info.cfNameString, text, sizeof text, kCFStringEncodingUTF8);
        AudioUnitParameterValue v = 0;
        AudioUnitGetParameter (u, id, kAudioUnitScope_Global, 0, &v);
        fprintf (f, "%s=%g\n", text, (double) v);
    }
}

static bool writeState (AudioUnit u, const std::string& path)
{
    CFPropertyListRef plist = nullptr; UInt32 size = sizeof (plist);
    if (AudioUnitGetProperty (u, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size) != noErr) return false;
    CFDataRef state = (CFDataRef) CFDictionaryGetValue ((CFDictionaryRef) plist, CFSTR ("jucePluginState"));
    if (state == nullptr) return false;
    FILE* f = fopen (path.c_str(), "wb");
    if (f == nullptr) return false;
    fwrite (CFDataGetBytePtr (state), 1, (size_t) CFDataGetLength (state), f);
    fclose (f);
    CFRelease (plist);
    return true;
}

static OSStatus silence (void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b) std::memset (io->mBuffers[b].mData, 0, frames * 4);
    return noErr;
}

static OSStatus setUp (AudioUnit u)
{
    AudioStreamBasicDescription fmt {};
    fmt.mSampleRate = 48000; fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = 4; fmt.mFramesPerPacket = 1; fmt.mBytesPerFrame = 4; fmt.mChannelsPerFrame = 2; fmt.mBitsPerChannel = 32;
    UInt32 maxFrames = 512;
    OSStatus e = AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof (fmt));
    if (e == noErr) e = AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fmt, sizeof (fmt));
    if (e == noErr) e = AudioUnitSetProperty (u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof (maxFrames));
    AURenderCallbackStruct cb { silence, nullptr };
    if (e == noErr) e = AudioUnitSetProperty (u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof (cb));
    return e;
}

int main (int argc, char** argv)
{
    if (argc < 3) { fprintf (stderr, "usage\n"); return 2; }
    const char* json = getenv ("SMOOTH_KEY_JSON");
    if (json == nullptr || json[0] != '/') { fprintf (stderr, "SMOOTH_KEY_JSON must be set to a scratch path\n"); return 2; }
    const std::string folder = argv[1], name = argv[2];

    AudioComponentDescription desc { kAudioUnitType_Effect, 'SKey', 'Luke', 0, 0 };
    AudioComponent comp = AudioComponentFindNext (nullptr, &desc);
    if (! comp) { fprintf (stderr, "not found\n"); return 4; }
    UInt32 version = 0;
    AudioComponentGetVersion (comp, &version);

    AudioUnit source, loaded;
    CHECK (AudioComponentInstanceNew (comp, &source));
    CHECK (setUp (source));
    CHECK (AudioUnitInitialize (source));
    usleep (200000);

    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "lock")
        {
            usleep (500000);
            AudioUnitParameterID id;
            if (! findParam (source, "Lock", id)) { fprintf (stderr, "no Lock\n"); return 5; }
            CHECK (AudioUnitSetParameter (source, id, kAudioUnitScope_Global, 0, 1.0f, 0));
            usleep (500000);
            continue;
        }
        const auto eq = a.find ('=');
        if (eq == std::string::npos)
        {
            const int preset = std::atoi (a.c_str());
            if (preset >= 0)
            {
                AUPreset p { preset, nullptr };
                CHECK (AudioUnitSetProperty (source, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof (p)));
            }
            continue;
        }
        AudioUnitParameterID id;
        if (! findParam (source, a.substr (0, eq).c_str(), id)) { fprintf (stderr, "no parameter %s\n", a.substr (0, eq).c_str()); return 5; }
        CHECK (AudioUnitSetParameter (source, id, kAudioUnitScope_Global, 0, (AudioUnitParameterValue) std::atof (a.substr (eq + 1).c_str()), 0));
    }
    usleep (500000);

    if (! writeState (source, folder + "/v1.7.1-" + name + ".state")) { fprintf (stderr, "no state\n"); return 6; }
    {
        FILE* f = fopen ((folder + "/v1.7.1-" + name + ".params").c_str(), "w");
        fprintf (f, "version=%u\n", (unsigned) version);
        dumpParams (source, f);
        fclose (f);
    }

    // The same build loads what it saved, as a session would, before it is initialised, and saves again once it has run.
    CFPropertyListRef plist = nullptr; UInt32 size = sizeof (plist);
    CHECK (AudioUnitGetProperty (source, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size));
    CHECK (AudioComponentInstanceNew (comp, &loaded));
    CHECK (setUp (loaded));
    CHECK (AudioUnitSetProperty (loaded, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof (plist)));
    CHECK (AudioUnitInitialize (loaded));
    usleep (500000);
    if (! writeState (loaded, folder + "/v1.7.1-" + name + ".reloaded.state")) { fprintf (stderr, "no reloaded state\n"); return 7; }
    {
        FILE* f = fopen ((folder + "/v1.7.1-" + name + ".reloaded.params").c_str(), "w");
        dumpParams (loaded, f);
        fclose (f);
    }
    fprintf (stderr, "written %s\n", name.c_str());
    fflush (stderr);
    // Tearing the instances down from a tool with no message loop traps in JUCE, so leave here.
    _exit (0);
}
