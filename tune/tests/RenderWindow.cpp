// Renders the real window to PNG with real speech going through the real processor, with no screen and no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// --live opens the real window with only one column of pixels on screen and measures its CPU at the screen's real pixel density.
// Speech comes from tests/corpus, which tests/make_corpus.sh makes with macOS say.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "TuneEditor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <sys/resource.h>
#include <thread>
#include <vector>

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;
static constexpr int kFrameSamples = 1600;   // one window frame at 30 a second

static std::vector<float> loadVoice (const char* name)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (SMOOTH_TUNE_CORPUS).getChildFile (name)));
    std::vector<float> x;
    if (reader == nullptr)
        return x;
    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, b.getNumSamples(), 0, true, false);
    x.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    double peak = 0.0;
    for (auto v : x) peak = std::max (peak, (double) std::abs (v));
    if (peak > 0.0)
        for (auto& v : x) v *= (float) (0.6 / peak);
    return x;
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<TuneEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, int height = 0)
    {
        if (width > 0)  processor.windowWidth.store (width);
        if (height > 0) processor.windowHeight.store (height);
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<TuneEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    void set (const juce::String& id, float value)
    {
        auto* p = processor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    void demoTuning()
    {
        set ("key", 11.0f);                               // B
        set ("scale", (float) smoothtune::scaleMajor);
        set ("retune", 45.0f);
        set ("humanize", 20.0f);
        set ("inputType", 3.0f);                          // Wide Range
    }

    void play (const std::vector<float>& voice, int fromFrame, int toFrame)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
        for (int frame = fromFrame; frame < toFrame; ++frame)
        {
            const size_t begin = (size_t) frame * kFrameSamples;
            for (int done = 0; done < kFrameSamples; done += kBlock)
            {
                const int n = std::min (kBlock, kFrameSamples - done);
                buffer.setSize (2, n, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                    {
                        const size_t at = begin + (size_t) done + (size_t) i;
                        buffer.setSample (ch, i, at < voice.size() ? voice[at] : 0.0f);
                    }
                processor.processBlock (buffer, midi);
            }
            editor->tick (1.0 / 30.0);
        }
    }

    void save (const juce::File& folder, const juce::String& name, float scale)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);   // lets attachments catch up with what was set
        editor->tick (1.0 / 30.0);
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
        auto file = folder.getChildFile (name + (scale > 1.5f ? "@2x.png" : "@1x.png"));
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        const bool ok = out.openedOk() && png.writeImageToStream (image, out);
        std::printf ("%s  %s  %d x %d\n", ok ? "saved" : "FAILED", file.getFileName().toRawUTF8(), image.getWidth(), image.getHeight());
    }

    void saveBoth (const juce::File& folder, const juce::String& name)
    {
        save (folder, name, 1.0f);
        save (folder, name, 2.0f);
    }
};

static double processCpuSeconds()
{
    rusage usage {};
    getrusage (RUSAGE_SELF, &usage);
    return (double) usage.ru_utime.tv_sec + (double) usage.ru_utime.tv_usec * 1.0e-6
         + (double) usage.ru_stime.tv_sec + (double) usage.ru_stime.tv_usec * 1.0e-6;
}

static int runLive (const std::vector<float>& voice)
{
    const auto run = [&] (bool withWindow)
    {
        Session s;
        s.demoTuning();

        std::atomic<bool> running { true };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, kBlock);
            juce::MidiBuffer midi;
            size_t at = 0;
            auto next = std::chrono::steady_clock::now();

            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlock; ++i)
                        buffer.setSample (ch, i, voice[(at + (size_t) i) % voice.size()]);
                at += kBlock;
                s.processor.processBlock (buffer, midi);
                next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
                std::this_thread::sleep_until (next);
            }
        });

        if (withWindow)
        {
            s.editor->setVisible (false);
            s.editor->setTopLeftPosition (1 - s.editor->getWidth(), 300);   // one column of pixels on screen, or macOS skips painting it
            s.editor->addToDesktop (juce::ComponentPeer::windowIgnoresKeyPresses | juce::ComponentPeer::windowIgnoresMouseClicks);
            s.editor->setVisible (true);
        }

        constexpr int seconds = 8;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1000);
        const int paintsBefore = s.editor->getDisplay().getPaintCount();
        const double cpuBefore = processCpuSeconds();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);
        const double cpu = (processCpuSeconds() - cpuBefore) / seconds;
        const int paints = s.editor->getDisplay().getPaintCount() - paintsBefore;

        if (withWindow)
        {
            const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
            std::printf ("real window: %.1f frames a second, screen %.0fx, peer %s\n", (double) paints / seconds,
                         display != nullptr ? display->scale : 0.0, s.editor->getPeer() != nullptr ? "open" : "none");
            s.editor->removeFromDesktop();
        }

        running = false;
        audio.join();
        return cpu;
    };

    const double without = run (false);
    const double with = run (true);
    std::printf ("audio only: %.2f%%  with window: %.2f%%  window's share: %.2f%% of one core\n", without * 100.0, with * 100.0, (with - without) * 100.0);
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const auto sung = loadVoice ("sung_goodnews.wav");
    const auto speech = loadVoice ("speech_samantha.wav");
    if (sung.empty() || speech.empty())
    {
        std::printf ("FAILED could not read tests/corpus\n");
        return 1;
    }

    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (sung);

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    const int totalFrames = (int) (sung.size() / kFrameSamples);

    if (argc > 1 && juce::String (argv[1]) == "--keys")
    {
        // Which key suits the voice best, so the pictures show ordinary tuning and not a voice far from every note.
        for (int scale : { (int) smoothtune::scaleMajor, (int) smoothtune::scaleMinor })
            for (int key = 0; key < 12; ++key)
            {
                Session s;
                s.demoTuning();
                s.set ("key", (float) key);
                s.set ("scale", (float) scale);
                double sum = 0.0;
                int n = 0;
                for (int frame = 0; frame < totalFrames; ++frame)
                {
                    s.play (sung, frame, frame + 1);
                    const auto r = s.processor.getReadings();
                    if (r.voiced) { sum += std::abs (r.centsOffTarget); ++n; }
                }
                std::printf ("%s %s %.1f\n", smoothtune::noteNames[key], smoothtune::scaleNames[scale], sum / juce::jmax (1, n));
            }
        return 0;
    }

    // Find the moment with the most correction on a settled voice.
    int bestFrame = totalFrames / 2;
    {
        Session s;
        s.demoTuning();
        float best = -1.0f;
        for (int frame = 0; frame < totalFrames - 1; ++frame)
        {
            s.play (sung, frame, frame + 1);
            const auto r = s.processor.getReadings();
            // Settled notes in view, and a moderate amount of correction right now: the most telling picture.
            int settled = 0;
            const auto count = s.processor.getPitchHistoryCount();
            for (auto i = count - 800; i < count; ++i)
            {
                const auto& pt = s.processor.getPitchPoint (i);
                if (pt.inputNote.load() > 0.0f && std::abs (pt.inputNote.load() - pt.targetNote.load()) < 0.5f)
                    ++settled;
            }
            const float amount = r.voiced ? std::abs (r.correctionCents) : 0.0f;
            const float score = amount >= 18.0f && amount <= 45.0f ? (float) settled : 0.0f;
            if (frame > 130 && score > best)
            {
                best = score;
                bestFrame = frame;
            }
        }
        std::printf ("chosen moment: frame %d of %d, %.0f settled points in view\n", bestFrame, totalFrames, (double) best);
    }

    { Session s; s.saveBoth (folder, "idle"); }

    {
        Session s;
        s.demoTuning();
        s.play (sung, 0, bestFrame + 1);
        s.saveBoth (folder, "active");
    }

    {
        Session s;
        s.set ("retune", 0.0f);
        s.set ("humanize", 0.0f);
        s.set ("scale", (float) smoothtune::scaleChromatic);
        s.set ("inputType", 3.0f);
        s.play (speech, 0, (int) (speech.size() / kFrameSamples) * 2 / 3);
        s.saveBoth (folder, "hard-tune-speech");
    }

    {
        Session s;
        s.demoTuning();
        s.set ("flex", 30.0f);
        s.set ("scale", (float) smoothtune::scaleCustom);
        const int removed[] = { 0, 2, 5, 7, 9 };
        for (auto n : removed) s.set ("note" + juce::String (n), 1.0f);
        s.set ("note4", 2.0f);
        s.set ("note10", 2.0f);
        s.play (sung, 0, bestFrame + 1);
        s.editor->previewHint ("flex");
        s.saveBoth (folder, "custom-notes-flex");
    }

    {
        Session s;
        s.demoTuning();
        s.editor->setAdvancedOpen (true);
        s.set ("transpose", 7.0f);
        s.set ("throat", 112.0f);
        s.play (sung, 0, bestFrame + 1);
        if (auto* value = dynamic_cast<ui::BarSlider*> (s.editor->findChildWithID ("humanize")))
            value->startTyping();
        s.saveBoth (folder, "advanced-open");
    }

    {
        Session s;
        s.demoTuning();
        s.set ("transpose", -12.0f);
        s.set ("formantPreserve", 0.0f);
        s.play (sung, 0, bestFrame + 1);
        s.saveBoth (folder, "advanced-closed-with-changes");
    }

    {
        Session s;
        s.demoTuning();
        s.set ("bypass", 1.0f);
        s.play (sung, 0, bestFrame + 1);
        s.saveBoth (folder, "bypassed");
    }

    {
        Session s;
        s.demoTuning();
        s.set ("followKey", 1.0f);
        s.play (speech, 0, (int) (speech.size() / kFrameSamples) / 2);
        s.saveBoth (folder, "follow-key");
    }

    {
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        s.demoTuning();
        s.play (sung, 0, bestFrame + 1);
        s.saveBoth (folder, "smallest");
    }

    // Since 1.2.2: all five Advanced settings moved at the smallest size (the summary names what fits and counts the rest), and a tall,
    // narrow window with only C to tune to, so the voice is far from its note (the note still turns lime, the readouts fit beside it).
    {
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        s.demoTuning();
        s.set ("transpose", 3.0f);
        s.set ("reference", 432.0f);
        s.set ("tracking", 75.0f);
        s.set ("throat", 110.0f);
        s.set ("formantPreserve", 0.0f);
        s.play (sung, 0, bestFrame + 1);
        s.saveBoth (folder, "smallest-advanced-closed-with-changes");
    }

    {
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::maxWindowHeight);
        s.demoTuning();
        s.set ("scale", (float) smoothtune::scaleCustom);
        for (int n = 1; n < 12; ++n) s.set ("note" + juce::String (n), 1.0f);
        s.play (sung, 0, bestFrame + 1);
        s.saveBoth (folder, "tall-narrow-one-note");
    }

    {
        Session s (1400, 1000);
        s.demoTuning();
        s.play (sung, 0, bestFrame + 1);
        s.save (folder, "large", 1.0f);
    }

    return 0;
}
