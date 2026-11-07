// Renders the real window to PNG with real speech from macOS say, loaded through the same path as the Load button, going through
// the real processor. No screen and no host. Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// --live [runs] [seconds] [silent or dense] opens the real window with one column of pixels on screen and measures its CPU at the screen's real pixel density.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "SliceEditor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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

static juce::File speechFile()
{
    return juce::File (SMOOTH_SLICE_CORPUS).getChildFile ("say-phrase.wav");
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<SliceEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (bool loadSpeech = true, int width = 0, int height = 0)
    {
        if (width > 0)  processor.windowWidth.store (width);
        if (height > 0) processor.windowHeight.store (height);
        processor.setPlayConfigDetails (0, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        if (loadSpeech)
        {
            processor.loadFile (speechFile());
            processor.waitForLoad (30000);
        }
        editor.reset (dynamic_cast<SliceEditor*> (processor.createEditor()));
        editor->setVisible (true);
        render (2);
    }

    void set (const juce::String& id, float value)
    {
        auto* p = processor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    void render (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            buffer.clear();
            processor.processBlock (buffer, midi);
            midi.clear();
        }
    }

    void notes (std::initializer_list<int> list, int velocity = 110)
    {
        for (auto n : list)
            midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) velocity), 0);
    }

    void save (const juce::File& folder, const juce::String& name, float scale)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);   // lets attachments catch up with what was set
        editor->refresh();
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

// A performance that keeps slices moving, with the audio thread running in real time. Up to three notes overlap.
static bool denseMode = false;   // the worst case: all 16 voices looping at once, each with a moving playhead

static void performBlock (Session& s, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, long long& block)
{
    midi.clear();
    constexpr int blocksPerStep = 45;   // about four notes a second
    if (denseMode)
    {
        if (block == 0)
        {
            s.set ("play_mode", (float) PluginProcessor::playLoop);
            s.set ("polyphony", 16.0f);
            for (int n = 0; n < 16; ++n)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60 + n, (juce::uint8) 110), 0);
        }
    }
    else if (block % blocksPerStep == 0)
    {
        const int step = (int) (block / blocksPerStep);
        const int count = s.processor.getShownTable().count;
        const int note = 60 + (step * 5) % juce::jmax (1, count);
        const int previous = 60 + ((step + 13) * 5) % juce::jmax (1, count);
        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 110), 0);
        midi.addEvent (juce::MidiMessage::noteOff (1, previous), 0);
    }
    ++block;
    buffer.clear();
    s.processor.processBlock (buffer, midi);
}

static int runLive (int repeats, int seconds, bool withAudio)
{
    if (denseMode)
        std::printf ("16 voices looping at once\n");
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "playing slices" : "off");
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   ms/paint  area   main%%  audio%%  Minstr/s  Minstr/frame  rebuilds  voices  load  builds\n");

    std::vector<double> results, instructionRates;
    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        Session s;
        s.editor.reset();
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        const auto before = CpuReading::now();
        long long block = 0;
        constexpr int blocks = (int) (kRate * 3.0) / kBlock;
        auto next = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            performBlock (s, buffer, midi, block);
            next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
            std::this_thread::sleep_until (next);
        }
        audioInstructionRate = (double) (CpuReading::now().instructions - before.instructions) / 3.0;
        std::printf ("audio alone: %.0f million instructions a second\n", audioInstructionRate * 1.0e-6);
    }

    for (int run = 0; run < repeats; ++run)
    {
        Session s;
        std::atomic<bool> running { true };
        std::atomic<double> audioCpu { 0.0 };

        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, kBlock);
            juce::MidiBuffer midi;
            long long block = 0;
            auto next = std::chrono::steady_clock::now();
            while (running.load())
            {
                if (withAudio)
                    performBlock (s, buffer, midi, block);
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

        auto& hero = s.editor->getWaveform();
        const int paintsBefore = hero.getPaintCount();
        const int rebuildsBefore = hero.getRebuildCount();
        const double paintBefore = hero.getPaintSeconds(), areaBefore = hero.getPaintedArea();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int paints = juce::jmax (1, hero.getPaintCount() - paintsBefore);
        const double window = (after.total - before.total - audioUsed) / wall;
        const int voices = hero.getVoiceCount();

        double load[1] { 0.0 };
        getloadavg (load, 1);
        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %5.1f  %7.2f  %4.0f%%  %5.1f  %5.1f  %8.0f  %11.1f  %8d  %6d  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     paints / wall, (hero.getPaintSeconds() - paintBefore) * 1000.0 / paints,
                     (hero.getPaintedArea() - areaBefore) / paints / juce::jmax (1.0, (double) hero.getWidth() * hero.getHeight()) * 100.0,
                     mainUsed / wall * 100.0, audioUsed / wall * 100.0,
                     instructionRate * 1.0e-6, instructionRate / (paints / wall) * 1.0e-6,
                     hero.getRebuildCount() - rebuildsBefore, voices, load[0], builds);
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

    if (! speechFile().existsAsFile())
    {
        std::printf ("FAILED could not read tests/corpus. Run tests/make_corpus.sh\n");
        return 1;
    }

    if (argc > 1 && juce::String (argv[1]) == "--live")
    {
        denseMode = argc > 4 && juce::String (argv[4]) == "dense";
        return runLive (argc > 2 ? juce::String (argv[2]).getIntValue() : 3,
                        argc > 3 ? juce::String (argv[3]).getIntValue() : 8,
                        ! (argc > 4 && juce::String (argv[4]) == "silent"));
    }

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    constexpr int settle = 24;   // about 130 ms in: voices are well under way

    { Session s (false); s.saveBoth (folder, "empty"); }
    { Session s; s.saveBoth (folder, "idle"); }

    {
        Session s;
        s.processor.selectSlice (4);
        s.notes ({ 60, 64 });
        s.render (settle);
        s.notes ({ 67 });
        s.render (settle / 2);
        s.saveBoth (folder, "active");
    }

    {
        Session s;
        s.processor.selectSlice (2);
        s.set ("s03_pitch", -5.0f);
        s.set ("s03_level", -9.0f);
        s.set ("s03_pan", -35.0f);
        s.set ("s03_reverse", 1.0f);
        s.set ("s03_attack", 120.0f);
        s.set ("s03_release", 600.0f);
        s.set ("decay", 400.0f);
        s.set ("sustain", 55.0f);
        s.set ("penv_amount", 7.0f);
        s.set ("penv_time", 240.0f);
        s.set ("s06_reverse", 1.0f);
        s.set ("s07_level", -14.0f);
        s.editor->previewHint ("sustain");
        s.saveBoth (folder, "edited-slice");
    }

    {
        Session s;
        s.processor.selectSlice (3);
        s.editor->setZoomed (true);
        s.notes ({ 63 });
        s.render (settle);
        s.saveBoth (folder, "zoom-slice");
    }

    {
        Session s;
        s.set ("slice_mode", (float) PluginProcessor::modeGrid);
        s.set ("grid", 6.0f);       // 16 parts
        s.set ("slice_start", 500.0f);
        s.editor->refresh();
        s.saveBoth (folder, "grid-equal");
    }

    {
        Session s;
        s.set ("slice_mode", (float) PluginProcessor::modeGrid);
        s.set ("grid_type", (float) PluginProcessor::gridNote);
        s.set ("grid_note", 3.0f);  // 1/16
        s.set ("grid_tempo", 96.0f);
        s.editor->refresh();
        s.editor->refresh();
        s.saveBoth (folder, "grid-note");
    }

    {
        Session s;
        s.set ("slice_mode", (float) PluginProcessor::modeManual);
        s.editor->refresh();
        s.processor.selectSlice (1);
        s.editor->refresh();
        s.saveBoth (folder, "manual");
    }

    {
        Session s;
        s.set ("key_mode", (float) PluginProcessor::keyChromatic);
        s.processor.selectSlice (5);
        s.notes ({ 60, 67 });
        s.render (settle);
        s.saveBoth (folder, "chromatic");
    }

    {
        Session s;
        s.editor->setAdvancedOpen (true);
        s.set ("transpose", -7.0f);
        s.set ("min_slice", 90.0f);
        s.notes ({ 61 });
        s.render (settle);
        s.saveBoth (folder, "advanced-open");
    }

    {
        Session s;
        s.set ("transpose", -7.0f);
        s.set ("bend_range", 12.0f);
        s.saveBoth (folder, "advanced-closed-with-changes");
    }

    {
        Session s;
        s.processor.selectSlice (27);
        s.saveBoth (folder, "unused-slot");
    }

    { Session s; s.set ("bypass", 1.0f); s.saveBoth (folder, "bypassed"); }

    {
        Session s (true, PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        s.processor.selectSlice (4);
        s.notes ({ 60, 64 });
        s.render (settle);
        s.saveBoth (folder, "smallest");
    }

    {
        Session s (true, 1600, 1000);
        s.notes ({ 62 });
        s.render (settle);
        s.save (folder, "large", 1.0f);
    }

    return 0;
}
