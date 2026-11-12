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
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
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

// The presets as 0.3.1 held them. Since 0.5.0 the factory presets switch Clean on and four of them moved (Luke left that call to the lead
// on 2026-11-11), so each preset case is now the 0.3.1 preset's values with Clean off: what a session saved with that preset in 0.3.1
// opens with (a session with no Clean in it opens with Clean off; tests/PluginTest.cpp, testCleanDefault, holds that to the sample).
// They are put in the way 0.3.1's preset put them, so they must still match the 0.2.1 render to the last bit. tests/au_session_null.sh
// makes the same proof through the real AU with states saved by the old builds themselves.
struct OldPreset { int number; std::vector<std::pair<const char*, float>> values; };
static const std::vector<OldPreset>& oldPresets()
{
    static const std::vector<OldPreset> table = {
        { 0, { { "voices", 2 }, { "rate", 0.8f }, { "depth", 1.5f }, { "delay", 15 }, { "wow", 15 }, { "flutter", 10 }, { "saturation", 20 }, { "tone", -2 }, { "spread", 70 }, { "mix", 35 } } },
        { 3, { { "voices", 2 }, { "rate", 0.35f }, { "depth", 1.0f }, { "delay", 12 }, { "wow", 60 }, { "flutter", 45 }, { "saturation", 55 }, { "tone", -7 }, { "spread", 50 }, { "mix", 50 } } },
        { 7, { { "voices", 8 }, { "rate", 0.3f }, { "depth", 1.2f }, { "delay", 18 }, { "wow", 12 }, { "flutter", 5 }, { "saturation", 15 }, { "tone", -2 }, { "spread", 100 }, { "mix", 35 },
               { "lowCut", 180 }, { "random", 15 }, { "randomRate", 0.5f }, { "unsync", 1 } } },
        { 9, { { "voices", 6 }, { "rate", 0.25f }, { "depth", 1.5f }, { "delay", 20 }, { "wow", 30 }, { "flutter", 20 }, { "saturation", 40 }, { "tone", -3 }, { "spread", 100 }, { "mix", 45 },
               { "feedback", 10 }, { "lowCut", 150 }, { "highCut", 7000 }, { "wowRate", 0.4f }, { "flutterRate", 6.0f }, { "random", 20 }, { "randomRate", 0.7f }, { "unsync", 1 },
               { "degrade", 20 }, { "degradeRate", 1.5f }, { "noise", 15 } } },
        { 12, { { "voices", 3 }, { "rate", 0.35f }, { "depth", 2.0f }, { "delay", 9 }, { "wow", 15 }, { "flutter", 8 }, { "saturation", 20 }, { "tone", -2 }, { "spread", 100 }, { "mix", 40 },
                { "feedback", 55 }, { "lowCut", 220 }, { "highCut", 9000 }, { "random", 10 }, { "randomRate", 0.5f }, { "unsync", 1 } } } };
    return table;
}

static void configure (PluginProcessor& p, const Case& c)
{
    if (c.preset >= 0)
    {
        for (const auto& o : oldPresets())
            if (o.number == c.preset)
                for (const auto& [id, v] : o.values)
                {
                    auto* prm = p.apvts.getParameter (id);
                    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
                }
        p.apvts.getParameter ("clean")->setValueNotifyingHost (0.0f);
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

#if HAS_WINDOW
// The window's own promises. Every control is a real parameter that moves both ways, with gestures, and the window saves only its two values.
static void runWindowPromises (int& failures)
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
    std::unique_ptr<TapeChorusEditor> window (dynamic_cast<TapeChorusEditor*> (p.createEditor()));
    window->setVisible (true);
    for (int i = 0; i < 20; ++i) window->tick (1.0 / 30.0);
    expect (counter.changes == 0 && counter.begins == 0, "opening and running it writes no parameter");

    auto& panel = window->getPanel();
    int missing = 0;
    for (auto* prm : p.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            if (panel.findChildWithID (withId->paramID) == nullptr)
            {
                std::printf ("      no control for %s\n", withId->paramID.toRawUTF8());
                ++missing;
            }
    expect (missing == 0, "every parameter has a place on screen, " + juce::String (p.getParameters().size()) + " parameters");

    // Automation moves the window.
    auto* depthParam = p.apvts.getParameter ("depth");
    depthParam->setValueNotifyingHost (depthParam->convertTo0to1 (3.0f));
    auto* depthStrip = dynamic_cast<ui::ParamStrip*> (panel.findChildWithID ("depth"));
    expect (depthStrip != nullptr && std::abs (depthStrip->getValue() - 3.0) < 0.002, "automation moves the Depth control");

    // A real drag on a column moves the real parameter, inside one begin and end gesture.
    {
        const auto event = [depthStrip] (juce::Point<float> at)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, depthStrip, depthStrip, now, at, now, 1, false);
        };
        counter.begins = counter.ends = 0;
        const auto before = p.apvts.getRawParameterValue ("depth")->load();
        depthStrip->mouseDown (event ({ 40.0f, 40.0f }));
        depthStrip->mouseDrag (event ({ 80.0f, 40.0f }));
        depthStrip->mouseUp (event ({ 80.0f, 40.0f }));
        const auto after = p.apvts.getRawParameterValue ("depth")->load();
        expect (after > before + 0.5f && after < before + 2.0f, "dragging Depth moves it from " + juce::String (before, 2) + " to " + juce::String (after, 2) + " ms");
        expect (counter.begins == 1 && counter.ends == 1, "the drag has one begin and one end");
    }

    // Drags on the picture itself: the tape for Rate and Depth, a reel for its gain. One gesture for each parameter touched.
    {
        auto& view = window->getView();
        const auto event = [&view] (juce::Point<float> at, juce::Point<float> downAt, int clicks = 1)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view, now, downAt, now, clicks, at != downAt);
        };

        const juce::Point<float> mid ((float) view.getWidth() * 0.5f, (float) view.getHeight() * 0.5f);
        const auto rateBefore = p.apvts.getRawParameterValue ("rate")->load(), depthBefore = p.apvts.getRawParameterValue ("depth")->load();
        counter.begins = counter.ends = 0;
        view.mouseDown (event (mid, mid));
        view.mouseDrag (event (mid + juce::Point<float> (60.0f, -30.0f), mid));
        view.mouseUp (event (mid + juce::Point<float> (60.0f, -30.0f), mid));
        expect (p.apvts.getRawParameterValue ("rate")->load() > rateBefore && p.apvts.getRawParameterValue ("depth")->load() > depthBefore,
                "dragging the tape up and right raises Rate and Depth");
        expect (counter.begins == 2 && counter.ends == 2, "that drag has one begin and one end for each");

        const auto reel = view.leftReelCentre();
        counter.begins = counter.ends = 0;
        view.mouseDown (event (reel, reel));
        view.mouseDrag (event (reel + juce::Point<float> (0.0f, -30.0f), reel));
        view.mouseUp (event (reel + juce::Point<float> (0.0f, -30.0f), reel));
        expect (p.apvts.getRawParameterValue ("inputGain")->load() > 3.0f && counter.begins == 1 && counter.ends == 1,
                "dragging the left reel up raises Input, in one gesture");

        view.mouseDoubleClick (event (reel, reel, 2));
        expect (std::abs (p.apvts.getRawParameterValue ("inputGain")->load()) < 0.01f, "double-clicking the reel resets Input");
        view.mouseDoubleClick (event (mid, mid, 2));
        expect (std::abs (p.apvts.getRawParameterValue ("rate")->load() - 0.8f) < 0.01f && std::abs (p.apvts.getRawParameterValue ("depth")->load() - 1.5f) < 0.01f,
                "double-clicking the tape resets Rate and Depth");
        depthParam->setValueNotifyingHost (depthParam->convertTo0to1 (3.0f));
    }

    // Double-click resets, through the slider's own return value.
    expect (std::abs (depthStrip->getDoubleClickReturnValue() - 1.5) < 0.001, "double-click goes back to the default, 1.50 ms");

    // Typing goes through the same parameter.
    {
        depthStrip->applyTyped ("2.5");
        expect (std::abs (p.apvts.getRawParameterValue ("depth")->load() - 2.5f) < 0.01f, "typing 2.5 sets Depth");
        depthStrip->applyTyped ("nonsense");
        expect (std::abs (p.apvts.getRawParameterValue ("depth")->load() - 2.5f) < 0.01f, "typing nonsense changes nothing");

        auto* lowCutStrip = dynamic_cast<ui::ParamStrip*> (panel.findChildWithID ("lowCut"));
        auto* highCutStrip = dynamic_cast<ui::ParamStrip*> (panel.findChildWithID ("highCut"));
        highCutStrip->applyTyped ("7");
        expect (std::abs (p.apvts.getRawParameterValue ("highCut")->load() - 7000.0f) < 1.0f, "typing 7 into High Cut, which reads in kHz, sets 7 kHz");
        lowCutStrip->applyTyped ("150");
        lowCutStrip->applyTyped ("off");
        expect (std::abs (p.apvts.getRawParameterValue ("lowCut")->load() - TapeChorusEngine::lowCutOffHz) < 0.01f, "typing off turns Low Cut off");
        expect (lowCutStrip->numberText() == "Off", "Low Cut then reads Off");
    }

    // A switch moves its parameter, and the other way round.
    {
        auto* unsync = dynamic_cast<juce::Button*> (panel.findChildWithID ("unsync"));
        counter.begins = counter.ends = 0;
        unsync->setToggleState (true, juce::sendNotificationSync);
        expect (p.apvts.getRawParameterValue ("unsync")->load() > 0.5f && counter.begins == 1, "clicking Unsync sets the parameter inside a gesture");
    }

    // The hero reads what the engine is really doing.
    {
        juce::AudioBuffer<float> b (2, kBlock);
        juce::MidiBuffer midi;
        b.clear();
        const auto before = p.getEngine().getTraceCount();
        for (int i = 0; i < 40; ++i) p.processBlock (b, midi);
        const auto count = p.getEngine().getTraceCount();
        const float rows = (float) (count - before), want = 40.0f * (float) kBlock * p.getEngine().getTraceHz() / (float) kRate;
        expect (std::abs (rows - want) <= 2.0f, "the tape picture gets a new row " + juce::String (p.getEngine().getTraceHz(), 0) + " times a second, " + juce::String (rows, 0) + " rows in 40 blocks");
        expect (std::abs (p.getEngine().getTraceRow (count - 1).delayMs[0][0].load() - p.getVoiceDelayMs (0, 0)) < 0.5f, "its newest row agrees with the read head reading");
        expect (p.getEngine().getTraceRow (count - 1).delayMs[0][7].load() == 0.0f, "a silent voice has no line");
    }

    // The tape stops when there is nothing to draw, so the window costs almost nothing.
    {
        int moved = 0;
        for (int i = 0; i < 60; ++i) moved += window->getView().tick (1.0 / 30.0) ? 1 : 0;
        for (int i = 0; i < 30; ++i) moved = window->getView().tick (1.0 / 30.0) ? 1 : 0;
        expect (moved == 0, "with no audio running and nothing changing, no frames are drawn");
        depthParam->setValueNotifyingHost (depthParam->convertTo0to1 (1.0f));
        expect (window->getView().tick (1.0 / 30.0), "a changed setting wakes it");
    }

    // The window's two values survive a session, and stay in range. A session from before the window opens at the default size.
    {
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
                    && std::abs (fromBefore.apvts.getRawParameterValue ("depth")->load() - 2.5f) < 0.002f,
                "a 0.2.1 session loads its settings and opens the window at the default size");
    }
}
#endif

// What the audio thread costs, in instructions, which do not depend on what else the Mac is doing. Build this on two versions to compare them.
static void measureAudioCost (const std::vector<float>& left, const std::vector<float>& right)
{
    const auto instructions = []
    {
        rusage_info_v6 info {};
        proc_pid_rusage (getpid(), RUSAGE_INFO_V6, (rusage_info_t*) &info);
        return (double) info.ri_instructions;
    };

    for (const int preset : { 0, 3, 7, 9 })
    {
        PluginProcessor p;
        p.setCurrentProgram (preset);
        p.setPlayConfigDetails (2, 2, kRate, 128);
        p.prepareToPlay (kRate, 128);
        juce::AudioBuffer<float> b (2, 128);
        juce::MidiBuffer midi;
        constexpr int blocks = (int) (kRate * 20.0) / 128;
        const auto before = instructions();
        for (int n = 0; n < blocks; ++n)
        {
            const int at = (n * 128) % (kSamples - 128);
            std::copy (left.begin() + at, left.begin() + at + 128, b.getWritePointer (0));
            std::copy (right.begin() + at, right.begin() + at + 128, b.getWritePointer (1));
            p.processBlock (b, midi);
        }
        std::printf ("%-22s %8.2f million instructions per second of audio\n", p.getProgramName (preset).toRawUTF8(), (instructions() - before) / 20.0 * 1.0e-6);
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const bool saving = argc > 1 && juce::String (argv[1]) == "--save";
    const auto left = readWav ("samantha.wav", 20000), right = readWav ("daniel.wav", 30000);
    int failures = 0;

    if (argc > 1 && juce::String (argv[1]) == "--cost")
    {
        measureAudioCost (left, right);
        return 0;
    }

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
