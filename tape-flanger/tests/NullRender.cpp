// Proves the window changed no sound. Renders speech, a sweep and silence through the installed AU with the system API, for the
// default, every preset and random automation, at several rates, block sizes and channel counts, and writes the raw float output.
// Run it on one build, install the next, run it again, and compare the folders.
// Build: clang++ -std=c++17 -O2 tests/NullRender.cpp -framework AudioToolbox -framework CoreFoundation -o build/NullRender
// Usage: NullRender render <tests/baseline/voice.f32> <folder>
//        NullRender compare <folder a> <folder b>
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

static std::vector<float> gIn;
static size_t gAt = 0;

static std::vector<float> readFloats (const std::string& path)
{
    std::ifstream f (path, std::ios::binary | std::ios::ate);
    if (! f.good())
        return {};
    std::vector<float> x ((size_t) f.tellg() / sizeof (float));
    f.seekg (0);
    f.read ((char*) x.data(), (std::streamsize) (x.size() * sizeof (float)));
    return x;
}

static OSStatus inputCb (void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        auto* d = (float*) io->mBuffers[b].mData;
        for (UInt32 i = 0; i < n; ++i)
            d[i] = gAt + i < gIn.size() ? gIn[gAt + i] : 0.0f;
    }
    gAt += n;
    return noErr;
}

struct Config { const char* name; double rate; UInt32 block; UInt32 channels; };

// preset -1 is the default. autoSeed -1 is no automation.
static bool render (const Config& cfg, const std::string& path, int preset, int autoSeed, int* presetCount)
{
    AudioComponentDescription d {};
    d.componentType = kAudioUnitType_Effect; d.componentSubType = 'STfl'; d.componentManufacturer = 'Luke';
    AudioComponent c = AudioComponentFindNext (nullptr, &d);
    AudioUnit u = nullptr;
    if (c == nullptr || AudioComponentInstanceNew (c, &u) != noErr) { puts ("cannot open aufx STfl Luke"); return false; }

    AudioStreamBasicDescription f {};
    f.mSampleRate = cfg.rate; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mChannelsPerFrame = cfg.channels; f.mBitsPerChannel = 32;
    UInt32 maxFrames = cfg.block;
    AudioUnitSetProperty (u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
    AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb { inputCb, nullptr };
    AudioUnitSetProperty (u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize (u) != noErr) { puts ("init failed"); return false; }

    if (presetCount != nullptr)
    {
        CFArrayRef list = nullptr;
        UInt32 size = sizeof list;
        *presetCount = 0;
        if (AudioUnitGetProperty (u, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &list, &size) == noErr && list != nullptr)
        {
            *presetCount = (int) CFArrayGetCount (list);
            CFRelease (list);
        }
    }

    const auto setPreset = [&] (int index)
    {
        AUPreset p {};
        p.presetNumber = index;
        p.presetName = CFSTR ("");
        AudioUnitSetProperty (u, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof p);
    };

    if (preset >= 0)
        setPreset (preset);

    UInt32 listSize = 0;
    AudioUnitGetPropertyInfo (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &listSize, nullptr);
    std::vector<AudioUnitParameterID> ids (listSize / sizeof (AudioUnitParameterID));
    AudioUnitGetProperty (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &listSize);
    std::vector<AudioUnitParameterInfo> infos (ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
    {
        UInt32 sz = sizeof (AudioUnitParameterInfo);
        AudioUnitGetProperty (u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, ids[i], &infos[i], &sz);
    }

    std::mt19937 rng ((uint32_t) (autoSeed + 1));
    const auto unit = [&rng] { return (double) rng() / 4294967296.0; };

    gAt = 0;
    std::ofstream out (path, std::ios::binary);
    std::vector<std::vector<float>> channels (cfg.channels, std::vector<float> (cfg.block));
    AudioTimeStamp ts {};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    const size_t total = gIn.size() + (size_t) cfg.rate;
    int blockIndex = 0;
    const int moveEvery = std::max (1, (int) (0.05 * cfg.rate) / (int) cfg.block);

    while (gAt < total)
    {
        if (autoSeed >= 0 && blockIndex % moveEvery == 0)
        {
            for (int moves = 0; moves < 3; ++moves)
            {
                const size_t i = (size_t) (unit() * (double) ids.size());
                AudioUnitSetParameter (u, ids[i], kAudioUnitScope_Global, 0, (float) (infos[i].minValue + unit() * (infos[i].maxValue - infos[i].minValue)), 0);
            }
            if (unit() < 0.02)
                setPreset ((int) (unit() * 13.0));
        }

        struct { AudioBufferList list; AudioBuffer second; } buffers {};
        buffers.list.mNumberBuffers = cfg.channels;
        buffers.list.mBuffers[0] = { 1, cfg.block * 4, channels[0].data() };
        if (cfg.channels > 1)
            buffers.second = { 1, cfg.block * 4, channels[1].data() };
        AudioUnitRenderActionFlags flags = 0;
        if (AudioUnitRender (u, &flags, &ts, 0, cfg.block, &buffers.list) != noErr) { puts ("render failed"); return false; }
        for (auto& x : channels)
            out.write ((const char*) x.data(), cfg.block * 4);
        ts.mSampleTime += cfg.block;
        ++blockIndex;
    }

    AudioUnitUninitialize (u);
    AudioComponentInstanceDispose (u);
    return true;
}

static std::vector<std::string> rawFiles (const std::string& folder)
{
    std::vector<std::string> names;
    if (auto* dir = opendir (folder.c_str()))
    {
        while (auto* e = readdir (dir))
        {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare (n.size() - 4, 4, ".raw") == 0)
                names.push_back (n);
        }
        closedir (dir);
    }
    std::sort (names.begin(), names.end());
    return names;
}

static int compare (const std::string& a, const std::string& b)
{
    int identical = 0, over = 0, count = 0;
    double worst = -400.0;
    std::string worstName;
    for (const auto& name : rawFiles (a))
    {
        const auto x = readFloats (a + "/" + name), y = readFloats (b + "/" + name);
        if (x.empty() || x.size() != y.size())
        {
            std::printf ("%-36s different length\n", name.c_str());
            ++over;
            continue;
        }
        double peak = 0.0, sum = 0.0, signal = 0.0;
        bool same = true;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double delta = (double) x[i] - (double) y[i];
            same = same && std::memcmp (&x[i], &y[i], 4) == 0;
            peak = std::max (peak, std::abs (delta));
            sum += delta * delta;
            signal = std::max (signal, (double) std::abs (x[i]));
        }
        const double peakDb = peak > 0.0 ? 20.0 * std::log10 (peak) : -400.0;
        const double rmsDb = sum > 0.0 ? 10.0 * std::log10 (sum / (double) x.size()) : -400.0;
        if (same)
            std::printf ("%-36s bit identical  signal peak %.2f\n", name.c_str(), signal);
        else
            std::printf ("%-36s peak %7.1f dBFS  rms %7.1f dBFS  signal peak %.2f\n", name.c_str(), peakDb, rmsDb, signal);
        identical += same ? 1 : 0;
        over += peakDb > -114.0 ? 1 : 0;
        if (peakDb > worst)
        {
            worst = peakDb;
            worstName = name;
        }
        ++count;
    }
    std::printf ("files %d  bit identical %d  worst %s  over the -114 dBFS limit %d\n", count, identical,
                 worst <= -400.0 ? "none" : (std::to_string ((int) std::round (worst)) + " dBFS (" + worstName + ")").c_str(), over);
    return count > 0 && over == 0 ? 0 : 1;
}

int main (int argc, char** argv)
{
    if (argc == 4 && std::string (argv[1]) == "compare")
        return compare (argv[2], argv[3]);

    if (argc != 4 || std::string (argv[1]) != "render")
    {
        puts ("usage: NullRender render voice.f32 folder | NullRender compare a b");
        return 2;
    }

    gIn = readFloats (argv[2]);
    if (gIn.empty())
    {
        puts ("no input");
        return 1;
    }

    // After the speech: a sweep across the whole band, then silence, so hiss and exact silence are covered too.
    const double rate = 48000.0;
    double phase = 0.0;
    for (int i = 0; i < 2 * (int) rate; ++i)
    {
        phase += 2.0 * M_PI * 20.0 * std::pow (1000.0, (double) i / (2.0 * rate)) / rate;
        gIn.push_back ((float) (0.25 * std::sin (phase)));
    }
    gIn.resize (gIn.size() + (size_t) rate, 0.0f);

    const std::string folder = argv[3];
    mkdir (folder.c_str(), 0755);

    const Config configs[] = { { "s48b128", 48000.0, 128, 2 }, { "s44b64", 44100.0, 64, 2 }, { "s96b512", 96000.0, 512, 2 }, { "m48b256", 48000.0, 256, 1 } };
    int presets = 0;
    if (! render (configs[0], folder + "/s48b128_default.raw", -1, -1, &presets))
        return 1;
    if (presets != 13)
    {
        std::printf ("expected 13 presets, found %d\n", presets);
        return 1;
    }

    int files = 1;
    for (const auto& cfg : configs)
    {
        const std::string prefix = folder + "/" + cfg.name;
        if (std::string (cfg.name) != "s48b128" && ! render (cfg, prefix + "_default.raw", -1, -1, nullptr))
            return 1;
        files += std::string (cfg.name) != "s48b128" ? 1 : 0;
        for (int p = 0; p < presets; ++p, ++files)
            if (! render (cfg, prefix + "_preset" + std::to_string (p) + ".raw", p, -1, nullptr))
                return 1;
    }

    const Config autos[] = { configs[0], configs[0], configs[0], configs[0], configs[1], configs[2], configs[3] };
    int seed = 0;
    for (const auto& cfg : autos)
    {
        if (! render (cfg, folder + "/auto" + std::to_string (seed) + "_" + cfg.name + ".raw", -1, seed, nullptr))
            return 1;
        ++seed;
        ++files;
    }

    std::printf ("rendered %d files\n", files);
    return 0;
}
