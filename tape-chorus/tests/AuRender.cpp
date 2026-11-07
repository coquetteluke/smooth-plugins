// Renders speech through the installed AU with the system API and writes the raw float output. Used to null one build of the
// plug-in against another: run before and after a change, then compare. Not shipped.
// Build: clang++ -std=c++17 -O2 tests/AuRender.cpp -framework AudioToolbox -framework CoreFoundation -o build/AuRender
// Usage: AuRender <left.wav> <right.wav> <out.raw> [rate=48000] [block=128] [channels=2] [preset=N] [Parameter Name=normalised value]...
#include <AudioToolbox/AudioToolbox.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::vector<float> readWav (const char* path)
{
    std::ifstream f (path, std::ios::binary);
    std::vector<char> b ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    std::vector<float> out;
    for (size_t pos = 12; pos + 8 <= b.size();)
    {
        uint32_t size = 0;
        std::memcpy (&size, b.data() + pos + 4, 4);
        if (std::memcmp (b.data() + pos, "data", 4) == 0)
        {
            const size_t n = std::min ((size_t) size, b.size() - pos - 8) / 2;
            out.resize (n);
            for (size_t i = 0; i < n; ++i)
            {
                int16_t s;
                std::memcpy (&s, b.data() + pos + 8 + i * 2, 2);
                out[i] = (float) s / 32768.0f;
            }
            break;
        }
        pos += 8 + size + (size & 1);
    }
    return out;
}

static const std::vector<float>* gIn[2] {};
static size_t gAt = 0;

static OSStatus inputCb (void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        auto* d = (float*) io->mBuffers[b].mData;
        const auto& src = *gIn[b & 1];
        for (UInt32 i = 0; i < n; ++i)
            d[i] = gAt + i < src.size() ? src[gAt + i] : 0.0f;
    }
    gAt += n;
    return noErr;
}

int main (int argc, char** argv)
{
    if (argc < 4) { puts ("usage: AuRender left.wav right.wav out.raw [rate=] [block=] [channels=] [preset=] [Name=value]..."); return 2; }
    const auto left = readWav (argv[1]), right = readWav (argv[2]);
    gIn[0] = &left; gIn[1] = &right;
    if (left.empty() || right.empty()) { puts ("no audio"); return 1; }

    double rate = 48000.0;
    UInt32 block = 128, channels = 2;
    int preset = -1;
    std::vector<std::string> settings;
    for (int a = 4; a < argc; ++a)
    {
        const std::string arg = argv[a];
        if (arg.rfind ("rate=", 0) == 0) rate = std::stod (arg.substr (5));
        else if (arg.rfind ("block=", 0) == 0) block = (UInt32) std::stoi (arg.substr (6));
        else if (arg.rfind ("channels=", 0) == 0) channels = (UInt32) std::stoi (arg.substr (9));
        else if (arg.rfind ("preset=", 0) == 0) preset = std::stoi (arg.substr (7));
        else settings.push_back (arg);
    }

    AudioComponentDescription d {};
    d.componentType = kAudioUnitType_Effect; d.componentSubType = 'STch'; d.componentManufacturer = 'Luke';
    AudioComponent c = AudioComponentFindNext (nullptr, &d);
    AudioUnit u = nullptr;
    if (c == nullptr || AudioComponentInstanceNew (c, &u) != noErr) { puts ("cannot open aufx STch Luke"); return 1; }

    AudioStreamBasicDescription f {};
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mChannelsPerFrame = channels; f.mBitsPerChannel = 32;
    UInt32 maxFrames = block;
    AudioUnitSetProperty (u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
    AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb { inputCb, nullptr };
    AudioUnitSetProperty (u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize (u) != noErr) { puts ("init failed"); return 1; }

    if (preset >= 0)
    {
        AUPreset p { preset, nullptr };
        if (AudioUnitSetProperty (u, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof p) != noErr) { puts ("preset failed"); return 1; }
    }

    UInt32 listSize = 0;
    AudioUnitGetPropertyInfo (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &listSize, nullptr);
    std::vector<AudioUnitParameterID> ids (listSize / sizeof (AudioUnitParameterID));
    AudioUnitGetProperty (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &listSize);

    for (const auto& arg : settings)
    {
        const auto eq = arg.find ('=');
        bool found = false;
        for (auto id : ids)
        {
            AudioUnitParameterInfo info {};
            UInt32 sz = sizeof info;
            AudioUnitGetProperty (u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &sz);
            char name[128] {};
            if (info.cfNameString != nullptr)
                CFStringGetCString (info.cfNameString, name, sizeof name, kCFStringEncodingUTF8);
            if (arg.compare (0, eq, name) == 0)
            {
                AudioUnitSetParameter (u, id, kAudioUnitScope_Global, 0, std::stof (arg.substr (eq + 1)), 0);
                found = true;
            }
        }
        if (! found) { printf ("no parameter for %s\n", arg.c_str()); return 1; }
    }

    std::ofstream out (argv[3], std::ios::binary);
    std::vector<float> a (block), b (block);
    AudioTimeStamp ts {};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    const size_t total = left.size() + (size_t) rate;
    while (gAt < total)
    {
        struct { AudioBufferList list; AudioBuffer second; } buffers {};
        buffers.list.mNumberBuffers = channels;
        buffers.list.mBuffers[0] = { 1, block * 4, a.data() };
        buffers.second = { 1, block * 4, b.data() };
        AudioUnitRenderActionFlags flags = 0;
        if (AudioUnitRender (u, &flags, &ts, 0, block, &buffers.list) != noErr) { puts ("render failed"); return 1; }
        out.write ((const char*) a.data(), block * 4);
        if (channels > 1) out.write ((const char*) b.data(), block * 4);
        ts.mSampleTime += block;
    }
    AudioUnitUninitialize (u);
    AudioComponentInstanceDispose (u);
    return 0;
}
