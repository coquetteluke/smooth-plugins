// Renders the real window to PNG with real speech going through the real processor, with no screen and no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// --live [runs] [seconds] [silent or background] opens the real window with one column of pixels on screen and measures its CPU at the
// screen's real pixel density, the way ~/smooth-resonance/docs/window-cpu.md says: the threads timed in one run, the audio thread by itself.
// The speech is macOS say, saved in tests/baseline/voice.f32 so every run gets the same words.

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "TapeColorEditor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;
static constexpr int kFrameSamples = 1600;   // one window frame at 30 a second

static std::vector<float> loadVoice()
{
    std::ifstream f (TAPE_VOICE, std::ios::binary | std::ios::ate);
    std::vector<float> x ((size_t) f.tellg() / sizeof (float));
    f.seekg (0);
    f.read ((char*) x.data(), (std::streamsize) (x.size() * sizeof (float)));
    return x;
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<TapeColorEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, int height = 0, bool advanced = false)
    {
        if (width > 0)  processor.windowWidth.store (width);
        if (height > 0) processor.windowHeight.store (height);
        if (advanced)
        {
            processor.advancedOpen.store (true);
            if (height <= 0)
                processor.windowHeight.store (PluginProcessor::defaultWindowHeight + TapeColorEditor::drawerHeight);
        }
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<TapeColorEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    void set (const juce::String& id, float value)
    {
        auto* p = processor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    void settle()
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);   // lets attachments catch up with what was set
    }

    void play (const std::vector<float>& voice, int fromFrame, int toFrame)
    {
        settle();
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
        settle();
        editor->tick (1.0 / 30.0);
        settle();
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

/** One reading of everything in this process, from the kernel. Times are CPU seconds. P-core time shows how much of it ran on the fast cores. */
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

static void useLiveSettings (Session& s)
{
    s.processor.setCurrentProgram (3);   // Warm Lead: Drive, Bias, a little Age, hiss and grain, so all the live parts move
}

static int runLive (const std::vector<float>& voice, int repeats, int seconds, bool withAudio, bool background)
{
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (background ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("main thread is %s\n", background ? "background, so it stays on the slow cores: the worst case" : "user interactive, like a host's");
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  frames/s  main%%  audio%%  Minstr/s  load  builds\n");

    constexpr double instructionsPerFastCoreSecond = 8.0e9;   // about what a fast core gets through, which turns the rate into a percentage
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        Session s;
        useLiveSettings (s);
        s.editor.reset();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        const auto before = CpuReading::now();
        size_t at = 0;
        constexpr int blocks = (int) (kRate * 3.0) / kBlock;
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

    std::vector<double> results, instructionRates;

    for (int run = 0; run < repeats; ++run)
    {
        Session s;
        useLiveSettings (s);

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

        auto& graph = s.editor->getResponse();
        const int paintsBefore = graph.getPaintCount();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const double window = (after.total - before.total - audioUsed) / wall;
        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);

        double load[1] { 0.0 };
        getloadavg (load, 1);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %8.1f  %5.1f  %5.1f  %8.0f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     (graph.getPaintCount() - paintsBefore) / wall,
                     mainUsed / wall * 100.0, audioUsed / wall * 100.0, instructionRate * 1.0e-6, load[0], builds);
        std::fflush (stdout);
        results.push_back (window * 100.0);
        instructionRates.push_back (instructionRate / instructionsPerFastCoreSecond * 100.0);

        s.editor->removeFromDesktop();
        running = false;
        audio.join();
    }

    std::sort (results.begin(), results.end());
    std::sort (instructionRates.begin(), instructionRates.end());
    std::printf ("cpu time: lowest %.1f%%, median %.1f%%, highest %.1f%% of one core\n", results.front(), results[results.size() / 2], results.back());
    std::printf ("work, in fast core terms: lowest %.1f%%, median %.1f%%, highest %.1f%%\n", instructionRates.front(), instructionRates[instructionRates.size() / 2], instructionRates.back());
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const auto voice = loadVoice();
    if (voice.empty())
    {
        std::printf ("FAILED could not read the speech\n");
        return 1;
    }

    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (voice, argc > 2 ? atoi (argv[2]) : 5, argc > 3 ? atoi (argv[3]) : 6, ! (argc > 4 && juce::String (argv[4]) == "silent"), argc > 4 && juce::String (argv[4]) == "background");

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    const int words = (int) (voice.size() / kFrameSamples) * 2 / 5;   // a moment in the middle of a loud word

    { Session s; s.saveBoth (folder, "idle"); }

    {
        Session s;
        s.processor.setCurrentProgram (3);   // Warm Lead
        s.play (voice, 0, words);
        s.saveBoth (folder, "active");
    }

    {
        Session s;
        s.processor.setCurrentProgram (8);   // Worn Cassette
        s.play (voice, 0, words);
        s.saveBoth (folder, "worn-cassette");
    }

    {
        Session s;
        s.set ("age", 45.0f);
        s.set ("drive", 20.0f);
        s.play (voice, 0, words);
        s.saveBoth (folder, "age-45");
    }

    {
        Session s;
        s.set ("age", 100.0f);
        s.play (voice, 0, 30);
        std::vector<float> silence (kFrameSamples * 40, 0.0f);
        s.play (silence, 0, 40);
        s.saveBoth (folder, "age-100-silence");
    }

    {
        Session s (0, 0, true);
        s.processor.setCurrentProgram (8);
        s.play (voice, 0, words);
        s.saveBoth (folder, "advanced-open");
    }

    {
        Session s;
        s.set ("compander", 2.0f);
        s.set ("spacing", 35.0f);
        s.set ("device", 1.0f);
        s.set ("lowcut", 120.0f);
        s.play (voice, 0, words);
        s.saveBoth (folder, "advanced-closed-with-changes");
    }

    {
        Session s;
        s.processor.setCurrentProgram (3);
        s.set ("bypass", 1.0f);
        s.play (voice, 0, words);
        s.saveBoth (folder, "bypassed");
    }

    {
        Session s;
        s.processor.setCurrentProgram (7);   // Starved Grit: the dead zone shows on the curve
        s.play (voice, 0, words);
        s.editor->hoverTile ("bias", true);
        s.editor->previewHint ("bias");
        s.saveBoth (folder, "starved-bias-hover");
    }

    {
        Session s;
        s.processor.setCurrentProgram (2);   // Vocal Glue
        s.play (voice, 0, words);
        s.editor->startTypingTile ("drive");
        s.saveBoth (folder, "typing-a-value");
    }

    {
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        s.processor.setCurrentProgram (3);
        s.play (voice, 0, words);
        s.saveBoth (folder, "smallest");
    }

    {
        // The smallest window with Advanced open and the longest words in it (0.4.4: these were cut off before).
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight + TapeColorEditor::drawerHeight, true);
        s.processor.setCurrentProgram (8);   // Worn Cassette
        s.set ("device", 4.0f);              // Dictation Machine
        s.set ("output", -12.0f);
        s.play (voice, 0, words);
        s.saveBoth (folder, "smallest-advanced-open");
    }

    {
        Session s (1500, 900);
        s.processor.setCurrentProgram (3);
        s.play (voice, 0, words);
        s.save (folder, "large", 1.0f);
    }

    return 0;
}
