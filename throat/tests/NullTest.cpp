// The window must not change the sound. Run with --save once, on the build from before the window, to write tests/baseline.
// Run with no arguments to compare the current build against it. Every comparison must null at -120 dBFS or better.
//
// Saved renders and one saved session from 0.2.0, at 48 kHz stereo, on real speech from macOS say. Each is compared three ways:
//  - with no window ever made,
//  - with the window open and drawing on the message thread while audio runs on another thread,
//  - with the window being opened and closed over and over while audio runs.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"
#if __has_include("editor/ThroatEditor.h")
 #include "editor/ThroatEditor.h"
 #define HAS_WINDOW 1
#else
 #define HAS_WINDOW 0
#endif
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#ifndef BASELINE_DIR
 #define BASELINE_DIR "tests/baseline"
#endif
#ifndef SPEECH_DIR
 #define SPEECH_DIR "tests/speech"
#endif

static constexpr double kRate = 48000.0;
static constexpr int kSamples = 72000, kBlock = 512;

struct Case { const char* name; int preset; };                       // preset -1 is the saved session
static const Case cases[] = { { "default", 0 }, { "big-chest", 3 }, { "pinched", 7 }, { "whisper", 10 }, { "giant", 11 }, { "blend", 13 }, { "session", -1 } };

static std::vector<float> readWav (const juce::String& file, int offset)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (juce::File (juce::String (SPEECH_DIR) + "/" + file).createInputStream().release(), true));
    std::vector<float> out ((size_t) kSamples, 0.0f);
    if (r == nullptr) { std::printf ("FAIL could not read %s\n", file.toRawUTF8()); return out; }
    juce::AudioBuffer<float> b (1, kSamples);
    r->read (&b, 0, kSamples, offset, true, false);
    std::copy (b.getReadPointer (0), b.getReadPointer (0) + kSamples, out.begin());
    return out;
}

static void moveEverything (PluginProcessor& p)
{
    const std::pair<const char*, float> moves[] = {
        { "length", 1.17f }, { "width", 1.2f }, { "width1", 0.9f }, { "width2", 1.3f }, { "width3", 0.8f }, { "width4", 1.15f }, { "width5", 0.7f },
        { "place2", 30.0f }, { "place3", 50.0f }, { "place4", 70.0f }, { "place5", 95.0f }, { "breath", 20.0f }, { "breathHp", 2500.0f },
        { "sourceVoice", 0.0f }, { "modelVoice", 2.0f }, { "pulse", 15.0f }, { "range", 2.0f }, { "precision", 2.0f }, { "mix", 80.0f },
        { "output", -2.5f }, { "levelMatch", 0.0f } };
    for (const auto& [id, v] : moves)
    {
        auto* prm = p.apvts.getParameter (id);
        prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    }
}

static juce::File stateFile() { return juce::File (juce::String (BASELINE_DIR)).getChildFile ("session.state"); }
static juce::File renderFile (const char* name) { return juce::File (juce::String (BASELINE_DIR)).getChildFile (juce::String (name) + ".f32"); }

static void configure (PluginProcessor& p, const Case& c)
{
    if (c.preset >= 0)
    {
        p.setCurrentProgram (c.preset);
        return;
    }
    juce::MemoryBlock saved;
    if (stateFile().loadFileAsData (saved))
        p.setStateInformation (saved.getData(), (int) saved.getSize());
    else
        moveEverything (p);
}

static std::vector<float> renderAudio (PluginProcessor& p, const std::vector<float>& left, const std::vector<float>& right)
{
    std::vector<float> out ((size_t) kSamples * 2);
    juce::AudioBuffer<float> b (2, kBlock);
    juce::MidiBuffer midi;
    for (int at = 0; at < kSamples; at += kBlock)
    {
        const int n = juce::jmin (kBlock, kSamples - at);
        b.setSize (2, n, true, false, true);
        std::copy (left.begin() + at, left.begin() + at + n, b.getWritePointer (0));
        std::copy (right.begin() + at, right.begin() + at + n, b.getWritePointer (1));
        p.processBlock (b, midi);
        for (int i = 0; i < n; ++i)
        {
            out[(size_t) (at + i) * 2] = b.getSample (0, i);
            out[(size_t) (at + i) * 2 + 1] = b.getSample (1, i);
        }
    }
    return out;
}

static void prepare (PluginProcessor& p)
{
    p.setPlayConfigDetails (2, 2, kRate, kBlock);
    p.prepareToPlay (kRate, kBlock);
}

// The size of the difference in dB against full scale, and how many samples are not bit for bit the same.
static double nullDb (const std::vector<float>& a, const std::vector<float>& b, int& differing)
{
    double biggest = 0.0;
    differing = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double d = std::abs ((double) a[i] - (double) b[i]);
        differing += d != 0.0 ? 1 : 0;
        biggest = juce::jmax (biggest, d);
    }
    return biggest > 0.0 ? 20.0 * std::log10 (biggest) : -400.0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const bool saving = argc > 1 && juce::String (argv[1]) == "--save";
    const auto left = readWav ("samantha.wav", 20000), right = readWav ("daniel.wav", 30000);
    int failures = 0;

    if (saving)
    {
        juce::File (juce::String (BASELINE_DIR)).createDirectory();
        PluginProcessor p;
        moveEverything (p);
        juce::MemoryBlock state;
        p.getStateInformation (state);
        stateFile().replaceWithData (state.getData(), state.getSize());

        for (const auto& c : cases)
        {
            PluginProcessor q;
            configure (q, c);
            prepare (q);
            const auto out = renderAudio (q, left, right);
            renderFile (c.name).replaceWithData (out.data(), out.size() * sizeof (float));
            double energy = 0.0;
            for (auto v : out) energy += (double) v * v;
            std::printf ("saved %s, %.1f dB rms\n", c.name, 10.0 * std::log10 (energy / (double) out.size() + 1.0e-30));
        }
        return 0;
    }

    const auto check = [&failures] (const char* name, const char* how, const std::vector<float>& got, const std::vector<float>& want)
    {
        int differing = 0;
        const double db = nullDb (got, want, differing);
        const bool pass = db <= -120.0;
        failures += pass ? 0 : 1;
        std::printf ("%s  %-10s %-18s null %s, %d of %d samples differ\n", pass ? "PASS" : "FAIL", name, how,
                     db <= -399.0 ? "exact" : (juce::String (db, 1) + " dBFS").toRawUTF8(), differing, (int) got.size());
    };

    for (const auto& c : cases)
    {
        juce::MemoryBlock data;
        if (! renderFile (c.name).loadFileAsData (data) || data.getSize() != (size_t) kSamples * 2 * sizeof (float))
        {
            std::printf ("FAIL  %s: no baseline, run --save on the old build\n", c.name);
            ++failures;
            continue;
        }
        std::vector<float> want ((size_t) kSamples * 2);
        std::memcpy (want.data(), data.getData(), data.getSize());

        {
            PluginProcessor p;
            configure (p, c);
            prepare (p);
            check (c.name, "no window", renderAudio (p, left, right), want);
        }

       #if HAS_WINDOW
        // Window open and drawing, audio on its own thread as in a host. Slowed down so that many frames land inside the render.
        {
            PluginProcessor p;
            configure (p, c);
            prepare (p);
            std::unique_ptr<ThroatEditor> window (dynamic_cast<ThroatEditor*> (p.createEditor()));
            window->setVisible (true);
            juce::MemoryBlock before;
            p.getStateInformation (before);

            std::vector<float> got;
            std::atomic<bool> done { false };
            std::thread audio ([&] { got = renderAudio (p, left, right); done = true; });
            int frames = 0;
            while (! done.load())
            {
                window->tick (1.0 / 30.0);
                juce::Image canvas (juce::Image::RGB, 300, 200, false);
                juce::Graphics g (canvas);
                g.addTransform (juce::AffineTransform::scale (300.0f / (float) window->getWidth()));
                window->paintEntireComponent (g, true);
                ++frames;
            }
            audio.join();
            window.reset();
            check (c.name, "window drawing", got, want);

            juce::MemoryBlock after;
            p.getStateInformation (after);
            const bool same = before == after;
            failures += same ? 0 : 1;
            std::printf ("%s  %-10s %-18s %d frames drawn, saved state %s\n", same ? "PASS" : "FAIL", c.name, "window drawing", frames, same ? "unchanged" : "CHANGED");
        }

        // Opened and closed over and over during audio.
        {
            PluginProcessor p;
            configure (p, c);
            prepare (p);
            std::vector<float> got;
            std::atomic<bool> done { false };
            std::thread audio ([&] { got = renderAudio (p, left, right); done = true; });
            int opened = 0;
            while (! done.load())
            {
                std::unique_ptr<juce::AudioProcessorEditor> window (p.createEditor());
                window->setVisible (true);
                window.reset();
                ++opened;
            }
            audio.join();
            check (c.name, "window reopening", got, want);
            std::printf ("INFO  %-10s opened and closed the window %d times during audio\n", c.name, opened);
        }
       #endif
    }

    std::printf (failures == 0 ? "null test passed\n" : "%d null test failures\n", failures);
    return failures == 0 ? 0 : 1;
}
