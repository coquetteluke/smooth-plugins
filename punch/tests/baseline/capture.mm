// Reads the state the installed Punch saves (kAudioUnitProperty_ClassInfo, key jucePluginState), loads it into a second instance
// and renders input through that. Copied from Duo's tests/baseline/capture.mm; only the AU code and the output (one channel) differ.
// Built and run once on 2026-11-13 against the installed 0.6.1, before 0.6.2 replaced it:
//   clang++ -std=c++17 -O1 -fobjc-arc capture.mm -framework AudioToolbox -framework Foundation -o capture
// Usage: capture <preset number or -1 for none> <input.f32 mono> <out.state> <out.f32 left channel; Punch puts out one gain, so right is the same>
//        capture -2 <input.f32 mono> <in.state> <out.f32>   plays a given state instead
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <vector>
#include <cstdio>
#include <unistd.h>

static std::vector<float> input;
static size_t readPos = 0;

static OSStatus feed (void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        float* d = (float*) io->mBuffers[b].mData;
        for (UInt32 i = 0; i < frames; ++i)
            d[i] = readPos + i < input.size() ? input[readPos + i] : 0.0f;
    }
    readPos += frames;
    return noErr;
}

#define CHECK(x) do { OSStatus e = (x); if (e != noErr) { fprintf (stderr, "%s failed: %d\n", #x, (int) e); return 1; } } while (0)

int main (int argc, char** argv)
{
    if (argc != 5) { fprintf (stderr, "usage\n"); return 2; }
    const int preset = atoi (argv[1]);
    {
        FILE* f = fopen (argv[2], "rb"); if (! f) return 3;
        fseek (f, 0, SEEK_END); long bytes = ftell (f); fseek (f, 0, SEEK_SET);
        input.resize ((size_t) bytes / 4); fread (input.data(), 4, input.size(), f); fclose (f);
    }
    AudioComponentDescription desc { kAudioUnitType_Effect, 'SPun', 'Luke', 0, 0 };
    AudioComponent comp = AudioComponentFindNext (nullptr, &desc);
    if (! comp) { fprintf (stderr, "not found\n"); return 4; }
    // A session load: the state goes into a new instance before it is initialised, as a host restores it, so nothing glides.
    AudioUnit source, au;
    CHECK (AudioComponentInstanceNew (comp, &source));
    CHECK (AudioComponentInstanceNew (comp, &au));
    AudioStreamBasicDescription fmt {};
    fmt.mSampleRate = 48000; fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = 4; fmt.mFramesPerPacket = 1; fmt.mBytesPerFrame = 4; fmt.mChannelsPerFrame = 2; fmt.mBitsPerChannel = 32;
    UInt32 maxFrames = 512;
    for (AudioUnit u : { source, au })
    {
        CHECK (AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof (fmt)));
        CHECK (AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fmt, sizeof (fmt)));
        CHECK (AudioUnitSetProperty (u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof (maxFrames)));
    }
    CHECK (AudioUnitInitialize (source));
    if (preset >= 0)
    {
        AUPreset p { preset, nullptr };
        CHECK (AudioUnitSetProperty (source, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof (p)));
    }
    CFPropertyListRef plist = nullptr; UInt32 size = sizeof (plist);
    CHECK (AudioUnitGetProperty (source, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size));
    CFDataRef state = (CFDataRef) CFDictionaryGetValue ((CFDictionaryRef) plist, CFSTR ("jucePluginState"));
    if (! state) { fprintf (stderr, "no jucePluginState\n"); return 5; }
    if (preset == -2)
    {
        // Play a state file made by hand from one read here, instead of writing one.
        NSData* given = [NSData dataWithContentsOfFile: [NSString stringWithUTF8String: argv[3]]];
        if (given == nil) return 6;
        CFMutableDictionaryRef copy = CFDictionaryCreateMutableCopy (kCFAllocatorDefault, 0, (CFDictionaryRef) plist);
        CFDictionarySetValue (copy, CFSTR ("jucePluginState"), (__bridge CFDataRef) given);
        CFRelease (plist);
        plist = copy;
    }
    else
    {
        FILE* f = fopen (argv[3], "wb"); fwrite (CFDataGetBytePtr (state), 1, (size_t) CFDataGetLength (state), f); fclose (f);
    }

    CHECK (AudioUnitSetProperty (au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof (plist)));
    AURenderCallbackStruct cb { feed, nullptr };
    CHECK (AudioUnitSetProperty (au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof (cb)));
    CHECK (AudioUnitInitialize (au));

    const size_t n = input.size();
    std::vector<float> L (n), R (n), bl (512), br (512);
    AudioTimeStamp ts {}; ts.mFlags = kAudioTimeStampSampleTimeValid;
    for (size_t pos = 0; pos < n; pos += 512)
    {
        const UInt32 frames = (UInt32) std::min<size_t> (512, n - pos);
        struct { AudioBufferList l; AudioBuffer extra; } abl {};
        abl.l.mNumberBuffers = 2;
        abl.l.mBuffers[0] = { 1, frames * 4, bl.data() };
        abl.l.mBuffers[1] = { 1, frames * 4, br.data() };
        AudioUnitRenderActionFlags flags = 0;
        ts.mSampleTime = (Float64) pos;
        CHECK (AudioUnitRender (au, &flags, &ts, 0, frames, &abl.l));
        std::copy_n (bl.data(), frames, L.data() + pos);
        std::copy_n (br.data(), frames, R.data() + pos);
    }
    size_t differ = 0;
    for (size_t i = 0; i < n; ++i) differ += L[i] != R[i] ? 1 : 0;
    fprintf (stderr, "left and right differ at %zu samples\n", differ);
    { FILE* f = fopen (argv[4], "wb"); fwrite (L.data(), 4, n, f); fclose (f); }
    // Everything is written. Tearing the instance down from a tool with no message loop traps in JUCE, so leave here.
    fprintf (stderr, "written\n");
    fflush (stderr);
    _exit (0);
}
