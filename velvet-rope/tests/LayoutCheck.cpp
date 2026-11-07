// The installed AU through the system API, the way a host does it, once per channel layout. Plain AudioToolbox, no JUCE.
// Proves what an offline test of the processor cannot: that the real wrapper maps Core Audio's channel order to the right
// channels, and that it finds the LFE from a real layout tag. Also Quality's latency, as a host sees and is told about it.
// Run after InstallAU: build/LayoutCheck
#include <AudioToolbox/AudioToolbox.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void check (bool ok, const std::string& what, double value = NAN, const char* unit = "")
{
    if (! ok) ++failures;
    if (std::isnan (value)) std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    else std::printf ("%s  %s = %.3f %s\n", ok ? "PASS" : "FAIL", what.c_str(), value, unit);
    std::fflush (stdout);
}

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;

struct Feed { int channels = 2; const std::vector<std::vector<float>>* in = nullptr; size_t pos = 0; };

static OSStatus inputCallback (void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io)
{
    auto& feed = *static_cast<Feed*> (ref);
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        auto* out = static_cast<float*> (io->mBuffers[b].mData);
        for (UInt32 i = 0; i < frames; ++i)
            out[i] = feed.pos + i < (*feed.in)[b].size() ? (*feed.in)[b][feed.pos + i] : 0.0f;
    }
    feed.pos += frames;
    return noErr;
}

static int notifiedLatency = 0;

static void latencyListener (void*, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement)
{
    ++notifiedLatency;
}

struct Unit
{
    AudioUnit au = nullptr;
    Feed feed;
    int channels = 2;

    ~Unit() { if (au != nullptr) { AudioUnitUninitialize (au); AudioComponentInstanceDispose (au); } }

    bool open (int numChannels, AudioChannelLayoutTag tag)
    {
        channels = numChannels;
        AudioComponentDescription d {};
        d.componentType = kAudioUnitType_Effect;
        d.componentSubType = 'SmRs';
        d.componentManufacturer = 'Luke';
        auto* c = AudioComponentFindNext (nullptr, &d);
        if (c == nullptr || AudioComponentInstanceNew (c, &au) != noErr)
            return false;

        UInt32 maxFrames = kBlock;
        AudioUnitSetProperty (au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);

        AudioStreamBasicDescription f {};
        f.mSampleRate = kRate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
        f.mBytesPerPacket = f.mBytesPerFrame = 4;
        f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = (UInt32) numChannels;
        f.mBitsPerChannel = 32;

        if (AudioUnitSetProperty (au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr
            || AudioUnitSetProperty (au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) != noErr)
            return false;

        AudioChannelLayout layout {};
        layout.mChannelLayoutTag = tag;

        if (AudioUnitSetProperty (au, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &layout, sizeof layout) != noErr
            || AudioUnitSetProperty (au, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, 0, &layout, sizeof layout) != noErr)
            return false;

        return true;
    }

    bool start (const std::vector<std::vector<float>>& in)
    {
        feed.channels = channels;
        feed.in = &in;
        feed.pos = 0;
        AURenderCallbackStruct cb { inputCallback, &feed };
        AudioUnitSetProperty (au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
        return AudioUnitInitialize (au) == noErr;
    }

    double latency() const
    {
        Float64 seconds = 0.0;
        UInt32 size = sizeof seconds;
        AudioUnitGetProperty (au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds, &size);
        return seconds * kRate;
    }

    bool setByName (const char* name, float value)
    {
        UInt32 size = 0;
        AudioUnitGetPropertyInfo (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr);
        std::vector<AudioUnitParameterID> ids (size / sizeof (AudioUnitParameterID));
        AudioUnitGetProperty (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size);
        for (auto id : ids)
        {
            AudioUnitParameterInfo info {};
            UInt32 infoSize = sizeof info;
            AudioUnitGetProperty (au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &infoSize);
            char text[128] {};
            if (info.cfNameString != nullptr)
                CFStringGetCString (info.cfNameString, text, sizeof text, kCFStringEncodingUTF8);
            if (std::string (text) == name)
                return AudioUnitSetParameter (au, id, kAudioUnitScope_Global, 0, value, 0) == noErr;
        }
        return false;
    }

    std::vector<std::vector<float>> render (int samples, double* cpuPercent = nullptr)
    {
        std::vector<std::vector<float>> out ((size_t) channels, std::vector<float> ((size_t) samples, 0.0f));
        std::vector<char> listMemory (sizeof (AudioBufferList) + sizeof (AudioBuffer) * (size_t) channels);
        auto* list = reinterpret_cast<AudioBufferList*> (listMemory.data());
        AudioTimeStamp ts {};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        double spent = 0.0;

        for (int pos = 0; pos + kBlock <= samples; pos += kBlock)
        {
            list->mNumberBuffers = (UInt32) channels;
            for (int ch = 0; ch < channels; ++ch)
                list->mBuffers[ch] = { 1, kBlock * 4, out[(size_t) ch].data() + pos };
            AudioUnitRenderActionFlags flags = 0;
            const auto a = std::chrono::steady_clock::now();
            const auto status = AudioUnitRender (au, &flags, &ts, 0, kBlock, list);
            spent += std::chrono::duration<double> (std::chrono::steady_clock::now() - a).count();
            if (status != noErr)
                return {};
            ts.mSampleTime += kBlock;
        }

        if (cpuPercent != nullptr)
            *cpuPercent = 100.0 * spent / ((double) samples / kRate);

        return out;
    }
};

static std::vector<float> tone (int samples, double hz, double level)
{
    std::vector<float> x ((size_t) samples);
    for (int i = 0; i < samples; ++i)
        x[(size_t) i] = (float) (level * std::sin (2.0 * M_PI * hz * i / kRate));
    return x;
}

static double levelAt (const std::vector<float>& x, double hz, int from)
{
    double re = 0.0, im = 0.0, window = 0.0;
    const int n = (int) x.size() - from;
    for (int i = 0; i < n; ++i)
    {
        const double h = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (n - 1));
        re += h * x[(size_t) (from + i)] * std::cos (2.0 * M_PI * hz * i / kRate);
        im += h * x[(size_t) (from + i)] * std::sin (2.0 * M_PI * hz * i / kRate);
        window += h;
    }
    return 20.0 * std::log10 (std::max (2.0 * std::sqrt (re * re + im * im) / window, 1.0e-12));
}

int main()
{
    struct Layout { const char* name; AudioChannelLayoutTag tag; int channels, lfe; };
    const Layout layouts[] = {
        { "mono",  kAudioChannelLayoutTag_Mono, 1, -1 },
        { "stereo", kAudioChannelLayoutTag_Stereo, 2, -1 },
        { "quad", kAudioChannelLayoutTag_Quadraphonic, 4, -1 },
        { "5.0", kAudioChannelLayoutTag_MPEG_5_0_A, 5, -1 },
        { "5.1, LFE fourth", kAudioChannelLayoutTag_MPEG_5_1_A, 6, 3 },
        { "6.0", kAudioChannelLayoutTag_AudioUnit_6_0, 6, -1 },
        { "7.0", kAudioChannelLayoutTag_AudioUnit_7_0, 7, -1 },
        { "7.1", kAudioChannelLayoutTag_MPEG_7_1_C, 8, 3 },
        { "5.1.2", kAudioChannelLayoutTag_Atmos_5_1_2, 8, 3 },
        { "7.1.2", kAudioChannelLayoutTag_Atmos_7_1_2, 10, 3 },
        { "5.1.4", kAudioChannelLayoutTag_Atmos_5_1_4, 10, 3 },
        { "7.1.4", kAudioChannelLayoutTag_Atmos_7_1_4, 12, 3 },
        { "9.1.6", kAudioChannelLayoutTag_Atmos_9_1_6, 16, 3 },
    };

    const int samples = 96000;

    for (const auto& l : layouts)
    {
        const std::string name = l.name;
        std::printf ("\n== %s, %d channels\n", l.name, l.channels);

        // Every channel with its own tone at Depth 0: each one has to come out of its own channel.
        {
            Unit u;
            if (! u.open (l.channels, l.tag)) { check (false, name + ": the AU accepts this layout's format and tag"); continue; }
            std::vector<std::vector<float>> in;
            for (int ch = 0; ch < l.channels; ++ch)
                in.push_back (tone (samples, ch == l.lfe ? 70.0 : 350.0 + 290.0 * ch, 0.1));
            check (u.start (in), name + ": initialises");
            check (u.setByName ("Depth", 0.0f), name + ": Depth is found by name");
            check ((int) std::lround (u.latency()) == 2048, name + ": latency property", u.latency(), "samples");
            double cpu = 0.0;
            const auto out = u.render (samples, &cpu);
            check (! out.empty(), name + ": renders");
            if (out.empty()) continue;
            double worstOwn = 0.0, worstOther = -300.0;
            for (int ch = 0; ch < l.channels; ++ch)
            {
                const double hz = ch == l.lfe ? 70.0 : 350.0 + 290.0 * ch;
                worstOwn = std::max (worstOwn, std::abs (levelAt (out[(size_t) ch], hz, 24000) + 20.0));
                for (int other = 0; other < l.channels; ++other)
                    if (other != ch)
                        worstOther = std::max (worstOther, levelAt (out[(size_t) ch], other == l.lfe ? 70.0 : 350.0 + 290.0 * other, 24000));
            }
            check (worstOwn < 0.2, name + ": each channel's tone comes out of the same channel (limit 0.2 dB)", worstOwn, "dB");
            check (worstOther < -90.0, name + ": and from no other (limit -90 dBFS)", worstOther, "dBFS");
        }

        // The same loud tone in every channel, with Delta on. Whatever the wrapper says the LFE is must lose nothing, and the rest must.
        {
            Unit u;
            u.open (l.channels, l.tag);
            std::vector<std::vector<float>> in;
            for (int ch = 0; ch < l.channels; ++ch)
                in.push_back (tone (samples, 3000.0, 0.5));
            u.start (in);
            u.setByName ("Depth", 12.0f);
            u.setByName ("Delta", 1.0f);
            double cpu = 0.0;
            const auto out = u.render (samples, &cpu);
            bool lfeRight = ! out.empty(), restRight = ! out.empty();
            for (int ch = 0; ch < (int) out.size(); ++ch)
            {
                double peak = 0.0;
                for (float v : out[(size_t) ch]) peak = std::max (peak, (double) std::abs (v));
                if (ch == l.lfe) lfeRight = lfeRight && peak == 0.0;
                else             restRight = restRight && peak > 0.1;
            }
            if (l.lfe >= 0)
                check (lfeRight, name + ": the channel the layout calls LFE, number " + std::to_string (l.lfe + 1) + ", has nothing taken out");
            check (restRight, name + ": every other channel does");
            check (cpu < 2.5 * (l.channels - (l.lfe >= 0 ? 1 : 0)), name + ": CPU through the real AU (limit 2.5% per treated channel)", cpu, "%");
        }
    }

    // Not a pass. JUCE's wrapper turns a layout tag into a set of speakers and back into the first tag with those speakers, so it
    // reads every 5.1 tag as L R C LFE Ls Rs. A host that really sends another 5.1 order gets the wrong channel left untreated.
    // JUCE is outside this repo and a plug-in never sees the tag, so this is reported here, and not fixed.
    std::puts ("\n== 5.1 with the LFE last, a tag JUCE does not keep");
    {
        Unit u;
        u.open (6, kAudioChannelLayoutTag_MPEG_5_1_C);
        std::vector<std::vector<float>> in;
        for (int ch = 0; ch < 6; ++ch)
            in.push_back (tone (samples, 3000.0, 0.5));
        u.start (in);
        u.setByName ("Depth", 12.0f);
        u.setByName ("Delta", 1.0f);
        const auto out = u.render (samples);
        for (int ch = 0; ch < (int) out.size(); ++ch)
        {
            double peak = 0.0;
            for (float v : out[(size_t) ch]) peak = std::max (peak, (double) std::abs (v));
            if (peak == 0.0)
                std::printf ("INFO  tag says the LFE is channel 6, the wrapper leaves channel %d untreated\n", ch + 1);
        }
    }

    // Quality as a host sees it.
    std::puts ("\n== Quality through the real AU");
    {
        Unit u;
        u.open (2, kAudioChannelLayoutTag_Stereo);
        std::vector<std::vector<float>> in { std::vector<float> (200000, 0.0f), std::vector<float> (200000, 0.0f) };
        in[0][120000] = in[1][120000] = 0.5f;
        u.start (in);
        AudioUnitAddPropertyListener (u.au, kAudioUnitProperty_Latency, latencyListener, nullptr);
        check ((int) std::lround (u.latency()) == 2048, "Normal", u.latency(), "samples");
        const struct { float value; int samples; const char* name; } modes[] = { { 0.0f, 512, "Low Latency" }, { 2.0f, 4096, "High" }, { 1.0f, 2048, "Normal again" } };
        for (const auto& m : modes)
        {
            notifiedLatency = 0;
            check (u.setByName ("Quality", m.value), std::string (m.name) + ": set by name");
            check ((int) std::lround (u.latency()) == m.samples, std::string (m.name) + ": the latency property follows at once", u.latency(), "samples");
            check (notifiedLatency > 0, std::string (m.name) + ": the host's latency listener is called", notifiedLatency);
        }

        // Rendered in Low Latency, the real sound arrives when the property says.
        u.setByName ("Quality", 0.0f);
        const auto out = u.render (160000);
        size_t top = 0;
        for (size_t i = 0; i < out[0].size(); ++i)
            if (std::abs (out[0][i]) > std::abs (out[0][top]))
                top = i;
        check ((int) top - 120000 == (int) std::lround (u.latency()), "Low Latency: the impulse arrives exactly when the property says", (double) top - 120000.0, "samples");

        AudioUnitRemovePropertyListenerWithUserData (u.au, kAudioUnitProperty_Latency, latencyListener, nullptr);
    }

    std::printf ("\n%s  %d failures\n", failures ? "RESULT FAIL" : "RESULT PASS", failures);
    return failures ? 1 : 0;
}
