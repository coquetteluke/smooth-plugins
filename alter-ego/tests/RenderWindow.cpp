// Renders the real window to PNG with real speech from macOS say going through the real processor, with no screen and no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// --live [runs] [seconds] [silent] opens the real window with one column of pixels on screen and measures its CPU at the screen's real pixel
// density. The method is the one in ~/smooth-resonance/docs/window-cpu.md: everything in the process minus the audio thread, timed by itself
// in the same run, with the main thread at user interactive QoS as a host's is. Instructions are compared to an audio-only count.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "MorphEditor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;
static constexpr int kFrameSamples = 1600;   // one window frame at 30 a second

static std::vector<float> loadVoice (const char* name)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (SMOOTH_MORPH_CORPUS).getChildFile (name)));
    std::vector<float> x;
    if (reader == nullptr)
        return x;
    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, b.getNumSamples(), 0, true, false);
    x.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    double peak = 0.0;
    for (auto v : x) peak = std::max (peak, (double) std::abs (v));
    if (peak > 0.0)
        for (auto& v : x) v *= (float) (0.5 / peak);
    return x;
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<MorphEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, int height = 0)
    {
        if (width > 0)  processor.windowWidth.store (width);
        if (height > 0) processor.windowHeight.store (height);
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<MorphEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    void set (const juce::String& id, float value)
    {
        auto* p = processor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
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
            editor->tick();
        }
    }

    void save (const juce::File& folder, const juce::String& name, float scale)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);   // lets attachments catch up with what was set
        editor->tick();
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
        auto file = folder.getChildFile (name + (scale > 1.5f ? "@2x.png" : "@1x.png"));
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        const bool ok = out.openedOk() && png.writeImageToStream (image, out);
        std::printf ("%s  %s  %d x %d  %s\n", ok ? "saved" : "FAILED", file.getFileName().toRawUTF8(), image.getWidth(), image.getHeight(), editor->readingText().toRawUTF8());
    }

    void saveBoth (const juce::File& folder, const juce::String& name)
    {
        save (folder, name, 1.0f);
        save (folder, name, 2.0f);
    }
};

// The loudest stretch of voice with a steady pitch, so the picture shows a vowel and not a gap between words.
static int bestFrameOf (const std::vector<float>& voice, int from)
{
    Session s;
    int best = from, run = 0;
    float bestScore = -1.0f;
    const int total = (int) (voice.size() / kFrameSamples) - 1;
    for (int frame = 0; frame < total; ++frame)
    {
        s.play (voice, frame, frame + 1);
        run = s.editor->getMirror().isVoiced() ? run + 1 : 0;
        if (frame >= from && run >= 8)
        {
            const float score = s.editor->getMirror().getOutputLevel() + 0.01f * (float) juce::jmin (run, 20);
            if (score > bestScore) { bestScore = score; best = frame; }
        }
    }
    std::printf ("chosen moment: frame %d of %d\n", best, total);
    return best;
}

// ---- the live measure ---------------------------------------------------------------------------------------------

struct CpuReading
{
    double total = 0.0, pCores = 0.0;
    unsigned long long instructions = 0;

    static CpuReading now()
    {
        rusage_info_v6 info {};
        CpuReading r;
        if (proc_pid_rusage (getpid(), RUSAGE_INFO_V6, (rusage_info_t*) &info) == 0)
        {
            mach_timebase_info_data_t base {};
            mach_timebase_info (&base);
            const double toSeconds = 1.0e-9 * (double) base.numer / (double) base.denom;
            r.total = (double) (info.ri_user_time + info.ri_system_time) * toSeconds;
            r.pCores = (double) (info.ri_user_ptime + info.ri_system_ptime) * toSeconds;
            r.instructions = info.ri_instructions;
        }
        return r;
    }
};

static double threadCpuSeconds()
{
    return (double) clock_gettime_nsec_np (CLOCK_THREAD_CPUTIME_ID) * 1.0e-9;
}

static int runLive (const std::vector<float>& voice, int repeats, int seconds, bool withAudio)
{
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%   mirror fps  pad fps   ms/paint  main%%  audio%%  Minstr/s  load  builds\n");

    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double audioInstructionRate = 0.0;
    std::vector<double> results, works;

    const auto setUp = [] (Session& s)
    {
        s.set ("character", 2.0f);
        s.set ("vibratoDepth", 40.0f);
    };

    if (withAudio)
    {
        Session s;
        setUp (s);
        s.editor.reset();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        const auto before = CpuReading::now();
        size_t at = 0;
        const int blocks = (int) (kRate * 3.0) / kBlock;
        auto next = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < kBlock; ++i)
                    buffer.setSample (ch, i, voice[(at + (size_t) i) % voice.size()]);
            at += kBlock;
            s.processor.processBlock (buffer, midi);
            next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
            std::this_thread::sleep_until (next);
        }
        audioInstructionRate = (double) (CpuReading::now().instructions - before.instructions) / 3.0;
        std::printf ("audio alone: %.0f million instructions a second\n", audioInstructionRate * 1.0e-6);
    }

    for (int run = 0; run < repeats; ++run)
    {
        Session s;
        setUp (s);

        std::atomic<bool> running { true };
        std::atomic<double> audioCpu { 0.0 };

        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, kBlock);
            juce::MidiBuffer midi;
            size_t at = 0;
            auto next = std::chrono::steady_clock::now();
            while (running.load())
            {
                if (withAudio)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < kBlock; ++i)
                            buffer.setSample (ch, i, voice[(at + (size_t) i) % voice.size()]);
                    at += kBlock;
                    s.processor.processBlock (buffer, midi);
                }
                audioCpu.store (threadCpuSeconds());
                next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
                std::this_thread::sleep_until (next);
            }
        });

        s.editor->setVisible (false);
        s.editor->setTopLeftPosition (1 - s.editor->getWidth(), 300);   // one column of pixels on screen, or macOS skips painting it
        s.editor->addToDesktop (juce::ComponentPeer::windowIgnoresKeyPresses | juce::ComponentPeer::windowIgnoresMouseClicks);
        s.editor->setVisible (true);

        juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);

        auto& mirror = s.editor->getMirror();
        auto& pad = s.editor->getPad();
        const int mirrorBefore = mirror.getPaintCount(), padBefore = pad.getPaintCount();
        const double paintBefore = mirror.getPaintSeconds() + pad.getPaintSeconds();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int mirrorPaints = mirror.getPaintCount() - mirrorBefore, padPaints = pad.getPaintCount() - padBefore;
        const double window = (after.total - before.total - audioUsed) / wall;
        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);

        double load[1] { 0.0 };
        getloadavg (load, 1);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %9.1f  %8.1f  %7.2f  %5.1f  %5.1f  %8.0f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     mirrorPaints / wall, padPaints / wall,
                     (mirror.getPaintSeconds() + pad.getPaintSeconds() - paintBefore) * 1000.0 / juce::jmax (1, mirrorPaints + padPaints),
                     mainUsed / wall * 100.0, audioUsed / wall * 100.0, instructionRate * 1.0e-6, load[0], builds);
        std::fflush (stdout);
        results.push_back (window * 100.0);
        works.push_back (instructionRate / instructionsPerFastCoreSecond * 100.0);

        s.editor->removeFromDesktop();
        running = false;
        audio.join();
    }

    std::sort (results.begin(), results.end());
    std::sort (works.begin(), works.end());
    std::printf ("cpu time: lowest %.1f%%, median %.1f%%, highest %.1f%% of one core\n", results.front(), results[results.size() / 2], results.back());
    std::printf ("work, in fast core terms: lowest %.1f%%, median %.1f%%, highest %.1f%%\n", works.front(), works[works.size() / 2], works.back());
    return 0;
}

// ---- the pictures -------------------------------------------------------------------------------------------------

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const auto woman = loadVoice ("speech_samantha.wav");
    const auto man = loadVoice ("speech_daniel.wav");
    if (woman.empty() || man.empty())
    {
        std::printf ("FAILED could not read tests/corpus\n");
        return 1;
    }

    if (argc > 1 && juce::String (argv[1]) == "--live")
    {
        const int runs = argc > 2 ? juce::String (argv[2]).getIntValue() : 5;
        const int seconds = argc > 3 ? juce::String (argv[3]).getIntValue() : 8;
        const bool silent = argc > 4 && juce::String (argv[4]) == "silent";
        return runLive (woman, runs, seconds, ! silent);
    }

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    const int womanFrame = bestFrameOf (woman, 30);
    const int manFrame = bestFrameOf (man, 30);

    { Session s; s.saveBoth (folder, "idle"); }

    {
        // The same voice on both sides: a perfect ink blot.
        Session s;
        s.play (woman, 0, womanFrame + 1);
        s.saveBoth (folder, "natural");
    }

    {
        Session s;
        s.set ("character", 2.0f);   // Child
        s.play (woman, 0, womanFrame + 1);
        s.saveBoth (folder, "active");
    }

    {
        Session s;
        s.set ("pitch", -7.0f);
        s.set ("formant", 5.0f);
        s.play (man, 0, manFrame + 1);
        s.saveBoth (folder, "by-hand");
    }

    {
        Session s;
        s.set ("character", 1.0f);   // Deep
        s.editor->getPad().showCharacter (4);
        s.play (man, 0, manFrame + 1);
        s.saveBoth (folder, "deep-hover-monster");
    }

    {
        Session s;
        s.set ("character", 4.0f);   // Monster
        s.set ("whisper", 15.0f);
        s.play (man, 0, manFrame + 1);
        s.saveBoth (folder, "monster");
    }

    {
        Session s;
        s.set ("character", 3.0f);   // Robot
        s.set ("robotNote", 48.0f);
        s.play (man, 0, manFrame + 1);
        s.saveBoth (folder, "robot");
    }

    {
        Session s;
        s.set ("vibratoDepth", 90.0f);
        s.set ("whisper", 80.0f);
        s.set ("formant", 3.0f);
        s.play (woman, 0, womanFrame + 1);
        s.editor->previewHint ("whisper");
        s.saveBoth (folder, "whisper-vibrato");
    }

    {
        Session s;
        s.set ("character", 2.0f);
        s.set ("bypass", 1.0f);
        s.play (woman, 0, womanFrame + 1);
        s.saveBoth (folder, "bypassed");
    }

    {
        Session s;
        s.editor->setAdvancedOpen (true);
        s.set ("character", 1.0f);
        s.set ("rangeMatch", 1.0f);
        s.set ("rangeCentre", 52.0f);
        s.set ("mix", 80.0f);
        s.set ("output", -3.0f);
        s.play (woman, 0, womanFrame + 1);
        s.saveBoth (folder, "advanced-open");
    }

    {
        Session s;
        s.set ("character", 2.0f);
        s.set ("mix", 60.0f);
        s.set ("output", -2.0f);
        s.play (woman, 0, womanFrame + 1);
        if (auto* field = dynamic_cast<ui::ValueField*> (s.editor->findChildWithID ("pitchValue")))
            field->startTyping();
        s.saveBoth (folder, "advanced-closed-typing");
    }

    {
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        s.set ("character", 4.0f);
        s.play (man, 0, manFrame + 1);
        s.saveBoth (folder, "smallest");
    }

    {
        // 1.2.6: the drawer at the smallest window, at the widest values (they no longer print over their names).
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        s.editor->setAdvancedOpen (true);
        s.set ("vibratoDepth", 150.0f);
        s.set ("rangeMatch", 1.0f);
        s.set ("rangeCentre", 58.0f);
        s.set ("output", -24.0f);
        s.play (woman, 0, womanFrame + 1);
        s.saveBoth (folder, "smallest-advanced-open");
    }

    {
        // 1.2.6: Advanced closed with four settings moved: what fits, and how many more.
        Session s;
        s.set ("rangeMatch", 1.0f);
        s.set ("peakGuard", 1.0f);
        s.set ("mix", 50.0f);
        s.set ("output", 2.5f);
        s.play (woman, 0, womanFrame + 1);
        s.saveBoth (folder, "advanced-closed-four-moved");
    }

    {
        // 1.2.6: the largest window, where the pad keeps a semitone the same distance either way.
        Session s (PluginProcessor::maxWindowWidth, PluginProcessor::maxWindowHeight);
        s.set ("character", 1.0f);
        s.play (man, 0, manFrame + 1);
        s.save (folder, "largest", 1.0f);
    }

    {
        Session s (1500, 1000);
        s.set ("character", 2.0f);
        s.play (woman, 0, womanFrame + 1);
        s.save (folder, "large", 1.0f);
    }

    return 0;
}
