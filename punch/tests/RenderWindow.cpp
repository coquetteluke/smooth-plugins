// Renders the real window to PNG with real speech going through the real processor, with no screen and no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// --live [runs] [seconds] [silent] opens the real window with one column of pixels on screen, painted by the system at the screen's real
// pixel density, with speech running in real time on another thread, and measures its CPU. Method from ~/smooth-resonance/docs/window-cpu.md.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "editor/PunchEditor.h"

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
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (SMOOTH_PUNCH_CORPUS).getChildFile (name)));
    std::vector<float> x;
    if (reader == nullptr)
        return x;
    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, b.getNumSamples(), 0, true, false);
    x.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    return x;
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<PunchEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, int height = 0)
    {
        if (width > 0)  processor.windowWidth.store (width);
        if (height > 0) processor.windowHeight.store (height);
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<PunchEditor*> (processor.createEditor()));
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

/** One reading of everything in this process, from the kernel. Times are CPU seconds. P-core time shows how much of it ran on the fast cores. */
struct CpuReading
{
    double total = 0.0, pCores = 0.0;
    unsigned long long instructions = 0, cycles = 0;

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
            r.cycles = info.ri_cycles;
        }

        return r;
    }
};

static double threadCpuSeconds()
{
    return (double) clock_gettime_nsec_np (CLOCK_THREAD_CPUTIME_ID) * 1.0e-9;
}

static void setBusy (Session& s)
{
    s.set ("attack", 6.0f);
    s.set ("sustain", -4.0f);
    s.set ("punch", 25.0f);
    s.set ("impact", 45.0f);
    s.set ("ceilingOn", 1.0f);
}

static int runLive (const std::vector<float>& voice, int repeats, int seconds, bool withAudio)
{
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   ms/paint  main%%  audio%%  Minstr/s  Minstr/frame  load  builds\n");

    std::vector<double> results, instructionRates;
    constexpr double instructionsPerFastCoreSecond = 8.0e9;   // about how many a fast core gets through in a second
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        Session s;
        setBusy (s);
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

    for (int run = 0; run < repeats; ++run)
    {
        Session s;
        setBusy (s);

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

        auto& view = s.editor->getHitView();
        const int paintsBefore = view.getPaintCount();
        const double paintBefore = view.getPaintSeconds();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int paints = juce::jmax (1, view.getPaintCount() - paintsBefore);
        const double window = (after.total - before.total - audioUsed) / wall;

        double load[1] { 0.0 };
        getloadavg (load, 1);

        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %5.1f  %7.2f  %5.1f  %5.1f  %8.0f  %11.1f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     paints / wall,
                     (view.getPaintSeconds() - paintBefore) * 1000.0 / paints,
                     mainUsed / wall * 100.0, audioUsed / wall * 100.0,
                     instructionRate * 1.0e-6, instructionRate / (paints / wall) * 1.0e-6, load[0], builds);
        instructionRates.push_back (instructionRate / instructionsPerFastCoreSecond * 100.0);
        std::fflush (stdout);
        results.push_back (window * 100.0);

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

    const auto samantha = loadVoice ("speech_samantha.wav");
    const auto daniel = loadVoice ("speech_daniel.wav");

    if (samantha.empty() || daniel.empty())
    {
        std::printf ("no speech: run tests/make_corpus.sh\n");
        return 1;
    }

    if (argc > 1 && juce::String (argv[1]) == "--live")
    {
        const int runs = argc > 2 ? juce::String (argv[2]).getIntValue() : 3;
        const int seconds = argc > 3 ? juce::String (argv[3]).getIntValue() : 10;
        const bool silent = argc > 4 && juce::String (argv[4]) == "silent";
        return runLive (samantha, runs, seconds, ! silent);
    }

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    const int total = (int) (samantha.size() / kFrameSamples);

    // Plays until the hit in view has been fully drawn out, at about the same place in the speech each time.
    const auto playToHit = [&] (Session& s, int fromFrame)
    {
        s.play (samantha, 0, fromFrame);
        const auto& tracker = s.editor->getHitView().getTracker();
        for (int frame = fromFrame; frame < total; ++frame)
        {
            s.play (samantha, frame, frame + 1);
            const auto& hit = tracker.latest();
            if (hit.length >= punchview::hitBins - 40 && hit.smoothed > 0)
            {
                float peak = 0.0f;
                for (int i = 0; i < hit.length; ++i)
                    peak = juce::jmax (peak, hit.rawBefore[(size_t) i]);
                if (punchview::toDb (peak) > -14.0f)
                    return;
            }
        }
    };

    constexpr int moment = 60;

    { Session s; s.saveBoth (folder, "idle"); }

    {
        Session s;
        playToHit (s, moment);
        s.saveBoth (folder, "active");
    }

    {
        Session s;
        s.set ("attack", 9.0f);
        s.set ("sustain", -7.0f);
        playToHit (s, moment);
        s.editor->previewHint ("attack");
        s.saveBoth (folder, "shaped");
    }

    {
        Session s;
        s.set ("attack", -8.0f);
        s.set ("sustain", 8.0f);
        playToHit (s, moment);
        s.saveBoth (folder, "softened");
    }

    {
        Session s;

        s.set ("attack", 4.0f);
        s.set ("punch", 30.0f);
        s.set ("impact", 70.0f);
        s.set ("ceiling", -6.0f);
        s.set ("ceilingOn", 1.0f);
        playToHit (s, moment);
        s.saveBoth (folder, "reductions");
    }

    {
        Session s;
        s.set ("attack", 6.0f);
        s.set ("sustain", -4.0f);
        playToHit (s, moment);
        // Stopped, then the controls moved: the hit in view is redrawn with the new settings.
        juce::AudioBuffer<float> none;
        s.set ("attack", -6.0f);
        s.set ("sustain", 6.0f);
        for (int i = 0; i < 16; ++i)
            s.editor->tick (1.0 / 30.0);
        s.saveBoth (folder, "preview-stopped");
    }

    {
        Session s;
        s.editor->setAdvancedOpen (true);
        s.set ("gain", 6.0f);
        s.set ("mix", 80.0f);
        playToHit (s, moment);
        if (auto* value = dynamic_cast<ui::BarSlider*> (s.editor->findChildWithID ("mix")))
            value->startTyping();
        s.saveBoth (folder, "advanced-open");
    }

    {
        Session s;
        s.set ("gain", 6.0f);
        s.set ("attack", 5.0f);
        s.set ("bypass", 1.0f);
        playToHit (s, moment);
        s.saveBoth (folder, "bypassed");
    }

    {
        Session s (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        s.set ("attack", 6.0f);
        s.set ("sustain", -4.0f);
        playToHit (s, moment);
        s.saveBoth (folder, "smallest");
    }

    {
        Session s (1500, 1100);
        s.set ("attack", 6.0f);
        s.set ("sustain", -4.0f);
        s.play (daniel, 0, 90);
        s.save (folder, "large", 1.0f);
    }

    return 0;
}
