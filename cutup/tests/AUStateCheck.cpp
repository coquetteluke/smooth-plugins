// Plays a saved state with a sample in it through the real installed component, the way a host restores a session.
// The offline tests talk to the processor directly, and HostCheck has no sample, so this closes the gap.
// Build: clang++ -std=c++17 -O2 tests/AUStateCheck.cpp -framework AudioToolbox -framework CoreFoundation -o build/AUStateCheck
// State: SMOOTH_SLICE_WRITE_STATE=build/austate.bin build/PluginTest_artefacts/Release/PluginTest
// Run:   build/AUStateCheck build/austate.bin | tee tests/au-check.txt
#include <AudioToolbox/AudioToolbox.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what, double value = NAN)
{
    if (! ok) ++failures;
    if (std::isnan (value)) std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    else std::printf ("%s  %s = %.3f\n", ok ? "PASS" : "FAIL", what, value);
}

static OSType code (const char* s) { return ((OSType) s[0] << 24) | ((OSType) s[1] << 16) | ((OSType) s[2] << 8) | (OSType) s[3]; }
static const CFStringRef stateKey = CFSTR ("jucePluginState");

struct Host
{
    AudioUnit unit = nullptr;
    Float64 sampleTime = 0;
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

    void close() { AudioUnitUninitialize (unit); AudioComponentInstanceDispose (unit); unit = nullptr; }

    // Sets the state the way a host restoring a session does: the whole class info dictionary.
    bool setState (const std::vector<char>& blob)
    {
        CFPropertyListRef info = nullptr;
        UInt32 size = sizeof info;
        if (AudioUnitGetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info, &size) != noErr) return false;
        CFMutableDictionaryRef copy = CFDictionaryCreateMutableCopy (nullptr, 0, (CFDictionaryRef) info);
        CFDataRef data = CFDataCreate (nullptr, (const UInt8*) blob.data(), (CFIndex) blob.size());
        CFDictionarySetValue (copy, stateKey, data);
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
        if (auto data = (CFDataRef) CFDictionaryGetValue ((CFDictionaryRef) info, stateKey))
            result.assign ((const char*) CFDataGetBytePtr (data), (const char*) CFDataGetBytePtr (data) + CFDataGetLength (data));
        CFRelease (info);
        return result;
    }

    std::vector<float> render (int total, std::vector<float>* right = nullptr)
    {
        std::vector<float> out, l (frames), r (frames);
        struct { AudioBufferList list; AudioBuffer extra; } ab;
        AudioTimeStamp ts {};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        for (int done = 0; done < total; done += (int) frames)
        {
            ts.mSampleTime = sampleTime;
            ab.list.mNumberBuffers = 2;
            ab.list.mBuffers[0] = { 1, frames * 4, l.data() };
            ab.list.mBuffers[1] = { 1, frames * 4, r.data() };
            AudioUnitRenderActionFlags flags = 0;
            if (AudioUnitRender (unit, &flags, &ts, 0, frames, &ab.list) != noErr) { ++failures; std::puts ("FAIL  render error"); break; }
            sampleTime += frames;
            out.insert (out.end(), l.begin(), l.end());
            if (right != nullptr) right->insert (right->end(), r.begin(), r.end());
        }
        return out;
    }

    void note (int n, bool on) { MusicDeviceMIDIEvent (unit, on ? 0x90 : 0x80, (UInt32) n, on ? 127 : 0, 0); }
};

static double frequency (const std::vector<float>& x, int from, int to)
{
    double first = -1, last = -1;
    int count = 0;
    for (int i = from + 1; i < to; ++i)
        if (x[(size_t) i - 1] < 0 && x[(size_t) i] >= 0)
        {
            const double at = (i - 1) + (double) -x[(size_t) i - 1] / ((double) x[(size_t) i] - x[(size_t) i - 1]);
            if (first < 0) first = at; else ++count;
            last = at;
        }
    return count > 0 ? count * 48000.0 / (last - first) : 0.0;
}
static double peakOf (const std::vector<float>& x) { double m = 0; for (auto v : x) { if (! std::isfinite (v)) return 1.0e9; m = std::max (m, (double) std::abs (v)); } return m; }

int main (int argc, char** argv)
{
    if (argc < 2) { std::puts ("usage: AUStateCheck state.bin"); return 2; }
    std::ifstream in (argv[1], std::ios::binary);
    std::vector<char> blob ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    check (blob.size() > 1000, "state file read, MB", (double) blob.size() / 1048576.0);

    Host h;
    check (h.open(), "real component opens in a plain host");

    h.note (60, true);
    check (peakOf (h.render (9600)) == 0.0, "with no sample it is silent through the real AU");
    h.note (60, false);

    check (h.setState (blob), "state with a sample restores through the class info");
    h.render (2048);
    h.note (61, true);
    auto out = h.render (24000);
    h.note (61, false);
    const double f = frequency (out, 4096, 20000);
    const double cents = 1200.0 * std::log2 (std::max (f, 1.0) / 470.0);
    check (std::abs (cents) <= 5.0, "note 61 plays slice 2 through the real AU, cents off", cents);
    check (peakOf (out) > 0.25 && peakOf (out) <= 0.51, "and at the file's level (velocity sens is 0 in this state), peak", peakOf (out));
    h.render (4096);

    const auto back = h.getState();
    check (back == blob, "the real AU saves the same bytes it was given, sample included");

    // A factory preset changes behaviours and keeps the sample.
    AUPreset preset { 3, CFSTR ("Stutter Loop") };
    check (AudioUnitSetProperty (h.unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &preset, sizeof preset) == noErr, "a factory preset selects");
    h.render (2048);
    h.note (60, true);
    const auto looped = h.render (96000); // 16 slices of a 4 s file is 250 ms each, so a Loop still sounds at 2 s
    h.note (60, false);
    std::vector<float> lateLoop (looped.begin() + 72000, looped.end());
    check (peakOf (lateLoop) > 0.25, "that preset keeps the sample and loops the slice, peak late on", peakOf (lateLoop));
    h.render (4096);

    // Damage: one flipped byte near the end of the audio.
    auto bad = blob;
    bad[bad.size() - 100] ^= 0x40;
    check (h.setState (bad), "a damaged state is accepted without an error");
    h.render (2048);
    h.note (60, true);
    check (peakOf (h.render (9600)) == 0.0, "and loads empty, silent, through the real AU");
    h.note (60, false);

    h.setState (blob);
    h.render (2048);
    h.note (60, true);
    check (peakOf (h.render (9600)) > 0.25, "a good state still loads after it");
    h.close();

    std::printf ("%s  %d failures\n", failures ? "RESULT FAIL" : "RESULT PASS", failures);
    return failures ? 1 : 0;
}
