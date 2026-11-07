// The window must not change the sound. Run with --save once, on the build from before the window, to write tests/baseline.
// Run with no arguments to compare the current build against it.
//
// Saved checksums of renders from 0.2.1 at 48 kHz stereo on real speech from macOS say, for presets and one session with every control moved.
// Each is compared three ways, and all three must match bit for bit:
//  - with no window ever made,
//  - with the window open and drawing on the message thread while audio runs on another thread,
//  - with the window being opened and closed over and over while audio runs.
// tests/au_null.sh makes the same comparison through the real installed AU, against the installed 0.2.1.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"
#if __has_include("editor/TapeChorusEditor.h")
 #include "editor/TapeChorusEditor.h"
 #define HAS_WINDOW 1
#else
 #define HAS_WINDOW 0
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

#ifndef BASELINE_DIR
 #define BASELINE_DIR "tests/baseline"
#endif
#ifndef SPEECH_DIR
 #define SPEECH_DIR "tests/speech"
#endif

static constexpr double kRate = 48000.0;
static constexpr int kSamples = 96000, kBlock = 256;

struct Case { const char* name; int preset; };                       // preset -1 is the saved session
static const Case cases[] = { { "default", 0 }, { "worn-cassette", 3 }, { "eight-voices", 7 }, { "cassette-backing", 9 }, { "resonant-swirl", 12 }, { "session", -1 } };

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
        { "voices", 5.0f }, { "rate", 1.7f }, { "depth", 2.5f }, { "delay", 22.0f }, { "wow", 40.0f }, { "flutter", 35.0f }, { "saturation", 55.0f }, { "tone", -4.0f },
        { "spread", 90.0f }, { "mix", 60.0f }, { "feedback", 45.0f }, { "inputGain", 3.0f }, { "outputGain", -2.0f }, { "lowCut", 150.0f }, { "highCut", 8000.0f },
        { "wowRate", 1.1f }, { "flutterRate", 12.0f }, { "random", 35.0f }, { "randomRate", 2.5f }, { "unsync", 1.0f }, { "degrade", 40.0f }, { "degradeRate", 3.5f },
        { "noise", 30.0f }, { "noiseGate", 0.0f } };
    for (const auto& [id, v] : moves)
    {
        auto* prm = p.apvts.getParameter (id);
        prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    }
}

static juce::File stateFile() { return juce::File (juce::String (BASELINE_DIR)).getChildFile ("session.state"); }
static juce::File hashFile()  { return juce::File (juce::String (BASELINE_DIR)).getChildFile ("renders-0.2.1.txt"); }

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

static std::vector<float> renderAudio (PluginProcessor& p, const std::vector<float>& left, const std::vector<float>& right, int pauseMs = 0)
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
        if (pauseMs > 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (pauseMs));
    }
    return out;
}

static void prepare (PluginProcessor& p)
{
    p.setPlayConfigDetails (2, 2, kRate, kBlock);
    p.prepareToPlay (kRate, kBlock);
}

static juce::String hashOf (const std::vector<float>& v)
{
    juce::uint64 h = 14695981039346656037ull;
    const auto* bytes = (const juce::uint8*) v.data();
    for (size_t i = 0; i < v.size() * sizeof (float); ++i)
        h = (h ^ bytes[i]) * 1099511628211ull;
    return juce::String::toHexString ((juce::int64) h);
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
        {
            PluginProcessor p;
            moveEverything (p);
            juce::MemoryBlock state;
            p.getStateInformation (state);
            stateFile().replaceWithData (state.getData(), state.getSize());
        }
        juce::String lines;
        for (const auto& c : cases)
        {
            PluginProcessor q;
            configure (q, c);
            prepare (q);
            const auto out = renderAudio (q, left, right);
            double energy = 0.0;
            for (auto v : out) energy += (double) v * v;
            lines << c.name << " " << hashOf (out) << "\n";
            std::printf ("saved %s, %.1f dB rms\n", c.name, 10.0 * std::log10 (energy / (double) out.size() + 1.0e-30));
        }
        hashFile().replaceWithText (lines);
        return 0;
    }

    std::map<juce::String, juce::String> want;
    {
        juce::StringArray rows;
        hashFile().readLines (rows);
        for (const auto& row : rows)
            if (row.containsChar (' '))
                want[row.upToFirstOccurrenceOf (" ", false, false)] = row.fromFirstOccurrenceOf (" ", false, false);
    }

    const auto check = [&failures, &want] (const char* name, const char* how, const std::vector<float>& got)
    {
        const bool pass = want.count (name) > 0 && want[name] == hashOf (got);
        failures += pass ? 0 : 1;
        std::printf ("%s  %-17s %-17s %s\n", pass ? "PASS" : "FAIL", name, how, pass ? "exact, bit for bit against 0.2.1" : "differs from 0.2.1");
    };

    for (const auto& c : cases)
    {
        {
            PluginProcessor p;
            configure (p, c);
            prepare (p);
            check (c.name, "no window", renderAudio (p, left, right));
        }

       #if HAS_WINDOW
        // Window open and drawing, audio on its own thread as in a host. Paced, so that many frames land inside the render.
        {
            PluginProcessor p;
            configure (p, c);
            prepare (p);
            std::unique_ptr<TapeChorusEditor> window (dynamic_cast<TapeChorusEditor*> (p.createEditor()));
            window->setVisible (true);
            juce::MemoryBlock before;
            p.getStateInformation (before);

            std::vector<float> got;
            std::atomic<bool> done { false };
            std::thread audio ([&] { got = renderAudio (p, left, right, 2); done = true; });
            int frames = 0;
            while (! done.load())
            {
                window->tick (1.0 / 30.0);
                juce::Image canvas (juce::Image::RGB, 270, 160, false);
                juce::Graphics g (canvas);
                g.addTransform (juce::AffineTransform::scale (270.0f / (float) window->getWidth()));
                window->paintEntireComponent (g, true);
                ++frames;
            }
            audio.join();
            window.reset();
            check (c.name, "window drawing", got);

            juce::MemoryBlock after;
            p.getStateInformation (after);
            const bool same = before == after;
            failures += same ? 0 : 1;
            std::printf ("%s  %-17s %-17s %d frames drawn, saved state %s\n", same ? "PASS" : "FAIL", c.name, "window drawing", frames, same ? "unchanged" : "CHANGED");
        }

        // Opened and closed over and over during audio.
        {
            PluginProcessor p;
            configure (p, c);
            prepare (p);
            std::vector<float> got;
            std::atomic<bool> done { false };
            std::thread audio ([&] { got = renderAudio (p, left, right, 2); done = true; });
            int opened = 0;
            while (! done.load())
            {
                std::unique_ptr<juce::AudioProcessorEditor> window (p.createEditor());
                window->setVisible (true);
                window.reset();
                ++opened;
            }
            audio.join();
            check (c.name, "window reopening", got);
            std::printf ("INFO  %-17s opened and closed the window %d times during audio\n", c.name, opened);
        }
       #endif
    }

   #if HAS_WINDOW
    runWindowPromises (failures);
   #endif

    std::printf (failures == 0 ? "null test passed\n" : "%d null test failures\n", failures);
    return failures == 0 ? 0 : 1;
}
