// Loads the installed component through plain AudioToolbox, the way a host does, and measures it. No JUCE.
// The shared ~/smooth-plugins/_shared/bin/HostCheck covers name, latency, parameters and presets. This one goes further:
// it sets controls by name, measures the reduction of a tone over noise, the side chain, and what Bypass does.
// Usage: build/HostCheck. Run after InstallAU. Exit 0 means every check passed.
#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

static constexpr double kRate = 48000.0;
static constexpr UInt32 kBlock = 512;

static int failures = 0;

static void check (bool ok, const std::string& what, double value, const char* unit)
{
    std::printf ("%s  %s = %.3f %s\n", ok ? "PASS" : "FAIL", what.c_str(), value, unit);
    failures += ok ? 0 : 1;
}

static OSType code (const char* s) { return ((OSType) s[0] << 24) | ((OSType) s[1] << 16) | ((OSType) s[2] << 8) | (OSType) s[3]; }

static std::string toString (CFStringRef s)
{
    char b[512] = {};
    if (s != nullptr) CFStringGetCString (s, b, sizeof b, kCFStringEncodingUTF8);
    return b;
}

// What the input callbacks play. Element 0 is the main input, element 1 the side chain.
struct Source
{
    std::vector<float> main, key;
    size_t position[2] = { 0, 0 };
};

static OSStatus inputCallback (void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32 bus, UInt32 frames, AudioBufferList* io)
{
    auto& source = *static_cast<Source*> (context);
    const auto& signal = bus == 0 ? source.main : source.key;
    auto& position = source.position[bus == 0 ? 0 : 1];

    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        auto* out = static_cast<float*> (io->mBuffers[b].mData);
        for (UInt32 i = 0; i < frames; ++i)
            out[i] = position + i < signal.size() ? signal[position + i] : 0.0f;
    }

    position += frames;
    return noErr;
}

struct Unit
{
    AudioUnit au = nullptr;
    std::map<std::string, AudioUnitParameterID> ids;
    Source source;
    double latencySamples = 0.0;

    bool open (bool withKey)
    {
        AudioComponentDescription d {};
        d.componentType = code ("aufx");
        d.componentSubType = code ("SmRs");
        d.componentManufacturer = code ("Luke");
        AudioComponent c = AudioComponentFindNext (nullptr, &d);

        if (c == nullptr || AudioComponentInstanceNew (c, &au) != noErr)
            return false;

        AudioStreamBasicDescription f {};
        f.mSampleRate = kRate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
        f.mBytesPerPacket = f.mBytesPerFrame = 4;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = 2;
        f.mBitsPerChannel = 32;

        UInt32 maxFrames = kBlock;
        AudioUnitSetProperty (au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);

        UInt32 buses = 0, size = sizeof buses;
        AudioUnitGetProperty (au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &buses, &size);
        AudioUnitSetProperty (au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);

        for (UInt32 e = 0; e < std::min<UInt32> (buses, withKey ? 2 : 1); ++e)
        {
            AudioUnitSetProperty (au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, e, &f, sizeof f);
            AURenderCallbackStruct cb { inputCallback, &source };
            AudioUnitSetProperty (au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, e, &cb, sizeof cb);
        }

        if (AudioUnitInitialize (au) != noErr)
            return false;

        Float64 latency = 0;
        size = sizeof latency;
        AudioUnitGetProperty (au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency, &size);
        latencySamples = latency * kRate;

        size = 0;
        AudioUnitGetPropertyInfo (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr);
        std::vector<AudioUnitParameterID> list (size / sizeof (AudioUnitParameterID));
        AudioUnitGetProperty (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, list.data(), &size);

        for (auto id : list)
        {
            AudioUnitParameterInfo info {};
            size = sizeof info;
            AudioUnitGetProperty (au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &size);
            ids[(info.flags & kAudioUnitParameterFlag_HasCFNameString) ? toString (info.cfNameString) : std::string (info.name)] = id;
        }

        return true;
    }

    // JUCE exposes every parameter to the host as 0 to 1, so these values are normalised.
    void set (const std::string& name, float value) const
    {
        if (ids.count (name) == 0) { std::printf ("FAIL  no parameter called %s\n", name.c_str()); ++failures; return; }
        AudioUnitSetParameter (au, ids.at (name), kAudioUnitScope_Global, 0, value, 0);
    }

    float get (const std::string& name) const
    {
        AudioUnitParameterValue v = 0;
        if (ids.count (name) > 0) AudioUnitGetParameter (au, ids.at (name), kAudioUnitScope_Global, 0, &v);
        return v;
    }

    /** Renders one channel of the whole source plus the latency, and returns it with the latency removed. */
    std::vector<float> render()
    {
        source.position[0] = source.position[1] = 0;
        const auto extra = (size_t) std::lround (latencySamples);
        std::vector<float> out (source.main.size() + extra, 0.0f), right (kBlock);
        struct { AudioBufferList list; AudioBuffer second; } ab;
        AudioTimeStamp ts {};
        ts.mFlags = kAudioTimeStampSampleTimeValid;

        for (size_t pos = 0; pos + kBlock <= out.size(); pos += kBlock)
        {
            ab.list.mNumberBuffers = 2;
            ab.list.mBuffers[0] = { 1, kBlock * 4, out.data() + pos };
            ab.list.mBuffers[1] = { 1, kBlock * 4, right.data() };
            AudioUnitRenderActionFlags flags = 0;

            if (AudioUnitRender (au, &flags, &ts, 0, kBlock, &ab.list) != noErr)
            {
                std::printf ("FAIL  render error\n");
                ++failures;
                break;
            }

            ts.mSampleTime += kBlock;
        }

        out.erase (out.begin(), out.begin() + (long) extra);
        return out;
    }

    void close()
    {
        if (au != nullptr)
        {
            AudioUnitUninitialize (au);
            AudioComponentInstanceDispose (au);
            au = nullptr;
        }
    }
};

static double toneDb (const std::vector<float>& x, size_t from, size_t to, double hz)
{
    const double w = 2.0 * M_PI * hz / kRate;
    double re = 0, im = 0, win = 0;
    const auto n = (double) (to - from);

    for (size_t i = from; i < to; ++i)
    {
        const double h = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) (i - from) / (n - 1.0));
        re += h * x[i] * std::cos (w * (double) i);
        im += h * x[i] * std::sin (w * (double) i);
        win += h;
    }

    return 20.0 * std::log10 (std::max (2.0 * std::sqrt (re * re + im * im) / win, 1.0e-12));
}

static std::vector<float> toneOverNoise (double seconds, double hz, double toneLevel, double noiseLevel, unsigned seed)
{
    std::vector<float> x ((size_t) (seconds * kRate));
    std::mt19937 g (seed);
    std::normal_distribution<float> d (0.0f, (float) noiseLevel);

    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (toneLevel * std::sin (2.0 * M_PI * hz * (double) i / kRate)) + d (g);

    return x;
}

int main()
{
    Unit unit;

    if (! unit.open (false))
    {
        std::puts ("FAIL  could not open aufx SmRs Luke");
        return 1;
    }

    check (unit.latencySamples == 2048.0, "reported latency", unit.latencySamples, "samples");
    check (unit.ids.size() == 60, "parameters found by name", (double) unit.ids.size(), "");   // 59 until 0.6.0 added Held Notes
    // Up to 0.7.1 this read "and is off when the plug-in loads" (0.0). Luke said yes on 2026-11-11 to the Held Notes switch being
    // ON by default, so from 0.8.0 a new instance has it on (1.0).
    check (unit.ids.count ("Held Notes") == 1 && unit.get ("Held Notes") == 1.0f, "Held Notes is one of them, and is on when the plug-in loads", (double) unit.get ("Held Notes"), "");

    const auto input = toneOverNoise (4.0, 2500.0, 0.25, 0.003, 1);
    unit.source.main = input;
    const auto level = [&] (const std::vector<float>& x) { return toneDb (x, 96000, 180000, 2500.0); };

    for (float depth : { 6.0f, 12.0f, 24.0f })
    {
        unit.set ("Depth", depth / 24.0f);
        const auto out = unit.render();
        const double change = level (out) - level (input);
        check (std::abs (change + depth) < 0.5, "tone over noise, Depth " + std::to_string ((int) depth) + " through the real AU", change, "dB");
        const double reading = (1.0 - unit.get ("Max Reduction")) * 24.0;
        check (reading > depth * 0.5 && reading <= depth + 0.1, "Max Reduction reading through the AU, just after the signal ends", reading, "dB");
    }

    unit.set ("Depth", 0.5f);
    unit.set ("Delta", 1.0f);
    {
        const auto out = unit.render();
        const double change = level (out) - level (input);
        // Removing 12 dB leaves 75% of the tone gone, so Delta plays it at about -2.5 dB.
        check (std::abs (change + 2.5) < 0.5, "Delta plays what is removed: tone at", change, "dB");
    }
    unit.set ("Delta", 0.0f);

    // Bypass through the host property: untouched audio, same delay.
    UInt32 on = 1;
    const auto status = AudioUnitSetProperty (unit.au, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &on, sizeof on);
    check (status == noErr, "kAudioUnitProperty_BypassEffect accepted", (double) status, "");
    {
        const auto out = unit.render();
        double worst = 0.0;
        for (size_t i = 48000; i < input.size() - 4096; ++i)
            worst = std::max (worst, (double) std::abs (out[i] - input[i]));
        check (worst < 1.0e-4, "bypassed: audio identical after the same 2048 sample delay, largest difference", worst, "");
    }
    UInt32 off = 0;
    AudioUnitSetProperty (unit.au, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &off, sizeof off);

    Float64 latencyAfter = 0;
    UInt32 size = sizeof latencyAfter;
    AudioUnitGetProperty (unit.au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latencyAfter, &size);
    check (latencyAfter * kRate == 2048.0, "latency unchanged by Bypass", latencyAfter * kRate, "samples");
    unit.close();

    // Side chain through the second input element: noise in the main input, a tone in the key.
    Unit keyed;

    if (! keyed.open (true))
    {
        std::puts ("FAIL  could not open with a side chain");
        return 1;
    }

    keyed.source.main = toneOverNoise (4.0, 3000.0, 0.01, 0.0001, 2);
    keyed.source.key  = toneOverNoise (4.0, 3000.0, 0.25, 0.001, 3);
    keyed.set ("Depth", 0.5f);
    const auto mainLevel = [&] (const std::vector<float>& x) { return toneDb (x, 96000, 180000, 3000.0); };

    keyed.set ("Side Chain", 1.0f);
    const auto carved = keyed.render();

    const double carve = mainLevel (carved) - 20.0 * std::log10 (0.01);
    check (std::abs (carve + 12.0) < 1.0, "Side Chain on: main carved by a tone in the key, Depth 12", carve, "dB");
    keyed.close();

    std::printf ("%s  %d failures\n", failures == 0 ? "RESULT PASS" : "RESULT FAIL", failures);
    return failures == 0 ? 0 : 1;
}
