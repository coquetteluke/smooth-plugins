#pragma once
// The sound, bit for bit, on real speech and sung-style material. A hash of every output sample's bits, so one changed bit anywhere fails.
// The numbers in WindowTests.h came from `PluginTest fingerprints` run on the 0.2.0 source, at commit 8f9ef4a, before the window existed.
#include <juce_audio_formats/juce_audio_formats.h>

namespace nulltest
{
    inline std::vector<float> loadCorpus (const char* name)
    {
        juce::AudioFormatManager m;
        m.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (m.createReaderFor (juce::File (juce::String (DUO_CORPUS) + "/" + name + ".wav")));
        if (reader == nullptr)
            return {};
        juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
        reader->read (&b, 0, b.getNumSamples(), 0, true, false);
        return { b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples() };
    }

    struct Case
    {
        const char* name;
        int preset;   // -1 is the settings as first opened
        int inputs, outputs, block;
        double rate;
        bool moveControls;   // every control moves while it plays
    };

    inline constexpr Case cases[] = {
        { "as first opened",     -1, 2, 2, 256, 48000.0, false },
        { "Natural Pair",         0, 2, 2, 256, 48000.0, false },
        { "Tight Double",         1, 2, 2, 256, 48000.0, false },
        { "Quiet Thickener",      2, 2, 2, 256, 48000.0, false },
        { "Wide Stack",           3, 2, 2, 256, 48000.0, false },
        { "Loose Gang",           4, 2, 2, 256, 48000.0, false },
        { "Doubles Only Wide",    5, 2, 2, 256, 48000.0, false },
        { "Two Singers",          6, 2, 2, 256, 48000.0, false },
        { "Slapback Double",      7, 2, 2, 256, 48000.0, false },
        { "mono in, stereo out",  3, 1, 2, 256, 48000.0, false },
        { "mono in, mono out",    3, 1, 1, 128, 48000.0, false },
        { "44.1 kHz, small blocks", 4, 2, 2, 64, 44100.0, false },
        { "96 kHz, big blocks",   3, 2, 2, 1024, 96000.0, false },
        { "every control moving", 3, 2, 2, 256, 48000.0, true },
    };

    /** eachBlock runs after every block, which is where a test can draw a window. */
    template <typename EachBlock>
    std::uint64_t hashOf (PluginProcessor& p, const Case& c, EachBlock&& eachBlock)
    {
        const auto sung = loadCorpus ("sung_goodnews"), speech = loadCorpus ("speech_samantha");
        std::vector<float> input (sung);
        input.insert (input.end(), speech.begin(), speech.end());
        input.resize (juce::jmin (input.size(), (size_t) 48000 * 8));

        if (c.preset >= 0)
            p.setCurrentProgram (c.preset);

        p.setPlayConfigDetails (c.inputs, c.outputs, c.rate, c.block);
        p.prepareToPlay (c.rate, c.block);

        const juce::StringArray moving { "doubles", "tightness", "width", "lead", "double1", "double2", "double3", "timingamount", "pitchamount", "vibrato", "timbre", "leadpan", "doublespan", "extradelay", "range" };
        juce::AudioBuffer<float> buffer (juce::jmax (c.inputs, c.outputs), c.block);
        juce::MidiBuffer midi;
        std::uint64_t sum = 1469598103934665603ull;
        int block = 0;

        for (size_t at = 0; at + (size_t) c.block <= input.size(); at += (size_t) c.block, ++block)
        {
            if (c.moveControls && block % 40 == 0)
            {
                auto* prm = p.apvts.getParameter (moving[(block / 40) % moving.size()]);
                prm->setValueNotifyingHost (0.5f + 0.5f * (float) std::sin (block * 0.013));
            }

            buffer.clear();
            for (int ch = 0; ch < c.inputs; ++ch)
                juce::FloatVectorOperations::copyWithMultiply (buffer.getWritePointer (ch), input.data() + at, ch == 0 ? 1.0f : 0.8f, c.block);

            p.processBlock (buffer, midi);

            for (int ch = 0; ch < c.outputs; ++ch)
            {
                const auto* x = buffer.getReadPointer (ch);
                for (int i = 0; i < c.block; ++i)
                {
                    std::uint32_t bits;
                    std::memcpy (&bits, x + i, sizeof bits);
                    sum = (sum ^ bits) * 1099511628211ull;
                }
            }

            eachBlock (block);
        }

        return sum;
    }

    inline void printFingerprints()
    {
        for (const auto& c : cases)
        {
            PluginProcessor p;
            std::printf ("    %lluull, // %s\n", (unsigned long long) hashOf (p, c, [] (int) {}), c.name);
        }
    }
}
