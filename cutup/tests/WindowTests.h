#pragma once
// The window, tested without a screen. Included by PluginTest.cpp after its helpers.

namespace windowtests
{
    // Every output sample of a fixed performance, hashed bit for bit.
    inline std::uint64_t hashBlock (std::uint64_t h, const juce::AudioBuffer<float>& b, int n)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const auto* x = reinterpret_cast<const unsigned char*> (b.getReadPointer (ch));
            for (size_t i = 0; i < (size_t) n * sizeof (float); ++i)
            {
                h ^= x[i];
                h *= 1099511628211ull;
            }
        }
        return h;
    }

    inline std::vector<float> fingerprintAudio()
    {
        auto x = toneSegments (12, 0.20, 48000.0, 220.0, 95.0);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] *= (float) (0.35 + 0.65 * std::exp (-(double) (i % 9600) / 2400.0));
        return x;
    }

    // Plays one preset with some slots moved, through one real processor. With a window, it stays open and draws the whole way.
    inline std::uint64_t outputFingerprint (const juce::File& file, int preset, bool withWindow)
    {
        PluginProcessor p;
        load (p, file);
        p.setCurrentProgram (preset);
        setP (p, "s02_pitch", 3.0f);
        setP (p, "s03_level", -6.0f);
        setP (p, "s04_pan", -40.0f);
        setP (p, "s05_reverse", 1.0f);
        setP (p, "s06_attack", 40.0f);
        setP (p, "transpose", 0.0f);
        prepare (p, 2, 48000.0, 256);

        std::unique_ptr<SliceEditor> window (withWindow ? dynamic_cast<SliceEditor*> (p.createEditor()) : nullptr);

        constexpr int block = 256, total = 48000 * 3;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        std::uint64_t h = 14695981039346656037ull;
        int blockIndex = 0;

        for (int pos = 0; pos < total; pos += block, ++blockIndex)
        {
            buffer.clear();
            midi.clear();
            const int step = pos / 6000;
            if (pos % 6000 < block)
            {
                midi.addEvent (on (60 + (step * 5) % 12, 60 + (step * 13) % 67), pos % 6000);
                if (step > 0)
                    midi.addEvent (off (60 + ((step - 1) * 5) % 12), pos % 6000);
            }
            p.processBlock (buffer, midi);
            h = hashBlock (h, buffer, block);

            if (window != nullptr)
            {
                window->refresh();
                if (blockIndex % 16 == 0)
                    window->createComponentSnapshot (window->getLocalBounds());
            }
        }
        return h;
    }
}
