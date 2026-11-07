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
#include <chrono>
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
        // Window open and drawing, audio on its own thread as in a host. Paced, so that many frames land inside the render.
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
            std::thread audio ([&] { got = renderAudio (p, left, right, 5); done = true; });
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
            std::thread audio ([&] { got = renderAudio (p, left, right, 5); done = true; });
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

   #if HAS_WINDOW
    // The window's own promises. Every control is a real parameter that moves both ways, with gestures, and the window saves only its two values.
    {
        const auto expect = [&failures] (bool ok, const juce::String& what)
        {
            failures += ok ? 0 : 1;
            std::printf ("%s  window: %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
        };

        struct Counter final : public juce::AudioProcessorListener
        {
            int changes = 0, begins = 0, ends = 0;
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override  { ++changes; }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
            void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++begins; }
            void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override   { ++ends; }
        };

        PluginProcessor p;
        prepare (p);
        Counter counter;
        p.addListener (&counter);
        std::unique_ptr<ThroatEditor> window (dynamic_cast<ThroatEditor*> (p.createEditor()));
        window->setVisible (true);
        for (int i = 0; i < 20; ++i) window->tick (1.0 / 30.0);
        expect (counter.changes == 0 && counter.begins == 0, "opening and running it writes no parameter");

        auto& panel = window->getPanel();
        int missing = 0;
        for (auto* prm : p.getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
                if (withId->paramID != "outputLevel" && panel.findChildWithID (withId->paramID) == nullptr)
                {
                    std::printf ("      no control for %s\n", withId->paramID.toRawUTF8());
                    ++missing;
                }
        expect (missing == 0, "every parameter has a place on screen, " + juce::String (p.getParameters().size()) + " parameters");

        // Automation moves the window.
        auto* lengthParam = p.apvts.getParameter ("length");
        lengthParam->setValueNotifyingHost (lengthParam->convertTo0to1 (1.3f));
        auto* slider = dynamic_cast<juce::Slider*> (panel.findChildWithID ("length"));
        expect (slider != nullptr && std::abs (slider->getValue() - 1.3) < 0.002, "automation moves the Length control");

        // A drag on the tract moves real parameters, inside begin and end gestures. It goes through the real mouse handlers.
        auto& view = window->getView();
        view.setBounds (view.getBounds());
        window->tick (1.0 / 30.0);
        const auto event = [&view] (juce::Point<float> at)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view, now, at, now, 1, false);
        };
        const auto widthBefore = p.apvts.getRawParameterValue ("width4")->load(), placeBefore = p.apvts.getRawParameterValue ("place4")->load();
        counter.begins = counter.ends = 0;
        const auto from = view.handlePosition (3);
        const auto to = from + juce::Point<float> (40.0f, -18.0f);
        view.mouseDown (event (from));
        view.mouseDrag (event (to));
        window->tick (1.0 / 30.0);
        const auto handleAfter = view.handlePosition (3);
        view.mouseUp (event (to));
        const auto widthAfter = p.apvts.getRawParameterValue ("width4")->load(), placeAfter = p.apvts.getRawParameterValue ("place4")->load();
        expect (widthAfter > widthBefore * 1.05f && placeAfter > placeBefore + 2.0f, "dragging the roof point out and along moves Width 4 from " + juce::String (widthBefore, 2) + " to " + juce::String (widthAfter, 2)
                                                                                     + " and Position 4 from " + juce::String (placeBefore, 1) + " to " + juce::String (placeAfter, 1));
        expect (handleAfter.getDistanceFrom (to) < 2.0f, "the handle lands under the mouse, within " + juce::String (handleAfter.getDistanceFrom (to), 2) + " pixels");
        expect (counter.begins == 2 && counter.ends == 2, "the drag has one begin and one end for each of its two parameters");

        // Reset takes the shape home, and nothing else.
        auto* breathParam = p.apvts.getParameter ("breath");
        breathParam->setValueNotifyingHost (0.3f);
        window->resetShape();
        bool home = true;
        for (const char* id : { "length", "width", "width1", "width2", "width3", "width4", "width5", "place2", "place3", "place4", "place5" })
            home = home && p.apvts.getParameter (id)->getValue() == p.apvts.getParameter (id)->getDefaultValue();
        expect (home && std::abs (breathParam->getValue() - 0.3f) < 0.002f, "Reset puts the shape back and leaves Breath alone");

        // Typing goes through the same parameter.
        auto* typed = dynamic_cast<ui::ParamSlider*> (panel.findChildWithID ("width2"));
        typed->applyTyped ("1.5");
        expect (std::abs (p.apvts.getRawParameterValue ("width2")->load() - 1.5f) < 0.01f, "typing 1.5 sets Width 2");
        typed->applyTyped ("nonsense");
        expect (std::abs (p.apvts.getRawParameterValue ("width2")->load() - 1.5f) < 0.01f, "typing nonsense changes nothing");

        // The window's two values survive a session, and stay in range. A session from before the window opens at the default size.
        window->setAdvancedOpen (true, false);
        p.windowWidth.store (1400);
        juce::MemoryBlock state;
        p.getStateInformation (state);
        window.reset();
        p.removeListener (&counter);

        PluginProcessor q;
        q.setStateInformation (state.getData(), (int) state.getSize());
        expect (q.windowWidth.load() == 1400 && q.advancedOpen.load(), "window size and Advanced come back with the session");
        expect (! q.apvts.state.hasProperty ("windowWidth") && ! q.apvts.state.hasProperty ("advancedOpen"), "they stay out of the parameter tree");

        auto xml = q.getXmlFromBinary (state.getData(), (int) state.getSize());
        xml->setAttribute ("windowWidth", 999999);
        juce::MemoryBlock damaged;
        q.copyXmlToBinary (*xml, damaged);
        q.setStateInformation (damaged.getData(), (int) damaged.getSize());
        expect (q.windowWidth.load() == PluginProcessor::maxWindowWidth, "a damaged window size is clamped");

        juce::MemoryBlock old;
        PluginProcessor fromBefore;
        const bool loaded = stateFile().loadFileAsData (old);
        fromBefore.setStateInformation (old.getData(), (int) old.getSize());
        expect (loaded && fromBefore.windowWidth.load() == PluginProcessor::defaultWindowWidth && ! fromBefore.advancedOpen.load()
                    && std::abs (fromBefore.apvts.getRawParameterValue ("length")->load() - 1.17f) < 0.002f,
                "a 0.2.0 session loads its settings and opens the window at the default size");
    }
   #endif

    std::printf (failures == 0 ? "null test passed\n" : "%d null test failures\n", failures);
    return failures == 0 ? 0 : 1;
}
