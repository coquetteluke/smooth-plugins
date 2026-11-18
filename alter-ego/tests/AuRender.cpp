// Renders a WAV through the installed AU with the system API and writes the raw float output. Used to null one build of the
// plug-in against another: run before and after a change, then compare the files with cmp. Not shipped.
// Build: clang++ -std=c++17 -O2 tests/AuRender.cpp -framework AudioToolbox -framework CoreFoundation -o build/AuRender
// Usage: AuRender <in.wav> <out.raw> [Parameter Name=normalised value]...
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

static const std::vector<float>* gIn = nullptr;
static size_t gAt = 0;

static OSStatus inputCb (void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        auto* d = (float*) io->mBuffers[b].mData;
        for (UInt32 i = 0; i < n; ++i)
            d[i] = gAt + i < gIn->size() ? (*gIn)[gAt + i] : 0.0f;
    }
    gAt += n;
    return noErr;
}

int main (int argc, char** argv)
{
    if (argc < 3) { puts ("usage: AuRender in.wav out.raw [Name=value]..."); return 2; }
    const auto input = readWav (argv[1]);
    gIn = &input;
    if (input.empty()) { puts ("no audio"); return 1; }

    AudioComponentDescription d {};
    d.componentType = kAudioUnitType_Effect; d.componentSubType = 'SMet'; d.componentManufacturer = 'Luke';
    AudioComponent c = AudioComponentFindNext (nullptr, &d);
    AudioUnit u = nullptr;
    if (c == nullptr || AudioComponentInstanceNew (c, &u) != noErr) { puts ("cannot open aufx SMet Luke"); return 1; }

    AudioStreamBasicDescription f {};
    f.mSampleRate = 48000; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
    constexpr UInt32 block = 128;
    UInt32 maxFrames = block;
    AudioUnitSetProperty (u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
    AudioUnitSetProperty (u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb { inputCb, nullptr };
    AudioUnitSetProperty (u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize (u) != noErr) { puts ("init failed"); return 1; }

    UInt32 listSize = 0;
    AudioUnitGetPropertyInfo (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &listSize, nullptr);
    std::vector<AudioUnitParameterID> ids (listSize / sizeof (AudioUnitParameterID));
    AudioUnitGetProperty (u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &listSize);

    for (int a = 3; a < argc; ++a)
    {
        const std::string arg = argv[a];
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

    std::ofstream out (argv[2], std::ios::binary);
    std::vector<float> left (block), right (block);
    AudioTimeStamp ts {};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    while (gAt < input.size() + 48000)
    {
        struct { AudioBufferList list; AudioBuffer second; } buffers {};
        buffers.list.mNumberBuffers = 2;
        buffers.list.mBuffers[0] = { 1, block * 4, left.data() };
        buffers.second = { 1, block * 4, right.data() };
        AudioUnitRenderActionFlags flags = 0;
        if (AudioUnitRender (u, &flags, &ts, 0, block, &buffers.list) != noErr) { puts ("render failed"); return 1; }
        out.write ((const char*) left.data(), block * 4);
        out.write ((const char*) right.data(), block * 4);
        ts.mSampleTime += block;
    }
    AudioUnitUninitialize (u);
    AudioComponentInstanceDispose (u);
    printf ("rendered %zu samples of input\n", input.size());
    return 0;
}
