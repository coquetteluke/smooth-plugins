// Renders the real window to PNG with real speech running through the real processor, with no host. Also measures its CPU.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
//                         build/RenderWindow_artefacts/Release/RenderWindow --live [runs] [seconds] [silent]
// Sung material is macOS `say` Good News voice, spoken material is `say` Samantha. Neither is a singer.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "editor/DuoEditor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
static constexpr int kSamplesPerFrame = 1600;   // one display frame at 30 a second

static std::vector<float> loadCorpus (const char* name)
{
    juce::AudioFormatManager m;
    m.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (m.createReaderFor (juce::File (juce::String (DUO_CORPUS) + "/" + name + ".wav")));
    std::vector<float> out;

    if (reader == nullptr)
        return out;

    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, b.getNumSamples(), 0, true, false);
    out.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());

    double peak = 0.0;
    for (auto v : out) peak = std::max (peak, (double) std::abs (v));
    if (peak > 0.0)
        for (auto& v : out) v *= (float) (0.5 / peak);

    return out;
}

static void setParam (PluginProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<DuoEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, bool advanced = false)
    {
        if (width > 0)
            processor.windowWidth.store (width);

        processor.advancedOpen.store (advanced);
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<DuoEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    ~Session()
    {
        editor.reset();
    }

    /** Runs audio up to the given frame of the material, ticking the display as the screen would. */
    void play (const std::vector<float>& voice, int startFrame, int endFrame)
    {
        for (int frame = startFrame; frame < endFrame; ++frame)
        {
            const size_t begin = (size_t) frame * kSamplesPerFrame;

            for (int done = 0; done < kSamplesPerFrame; done += kBlock)
            {
                const int n = std::min (kBlock, kSamplesPerFrame - done);
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

// ---- CPU ------------------------------------------------------------------------------------------------------------

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

/** The real thing: a real window with all but one column of pixels off the edge of the screen, painted by the system at the screen's real pixel density,
    with sung material running in real time on another thread. The window's share is everything this process used except the audio thread, timed by
    that thread itself in the same run, so there is no separate baseline to subtract. Loose Gang is the most there is to draw: three doubles, all moving. */
static int runLive (int repeats, int seconds, bool withAudio)
{
    const auto voice = loadCorpus ("sung_goodnews");
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   main%%  audio%%  Minstr/s  Minstr/frame  load  builds\n");

    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        Session s;
        s.processor.setCurrentProgram (4);
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
        s.processor.setCurrentProgram (4);

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
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlock; ++i)
                        buffer.setSample (ch, i, withAudio ? voice[(at + (size_t) i) % voice.size()] : 0.0f);

                at += kBlock;
                s.processor.processBlock (buffer, midi);
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

        auto& stage = s.editor->getStage();
        const int paintsBefore = stage.getPaintCount();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int rawPaints = stage.getPaintCount() - paintsBefore;
        const int paints = juce::jmax (1, rawPaints);
        const double window = (after.total - before.total - audioUsed) / wall;

        double load[1] { 0.0 };
        getloadavg (load, 1);

        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %5.1f  %5.1f  %5.1f  %8.0f  %11.1f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     rawPaints / wall, mainUsed / wall * 100.0, audioUsed / wall * 100.0,
                     instructionRate * 1.0e-6, instructionRate / (paints / wall) * 1.0e-6, load[0], builds);
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

// ---- pictures -------------------------------------------------------------------------------------------------------

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    // What the window's one addition to the audio path costs: reading the input peak, in instructions, against what the audio uses anyway.
    if (argc > 1 && juce::String (argv[1]) == "--peakcost")
    {
        const auto voice = loadCorpus ("sung_goodnews");
        juce::AudioBuffer<float> b (2, kBlock);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kBlock; ++i)
                b.setSample (ch, i, voice[(size_t) (20000 + i)]);
        constexpr int blocks = 200000;
        const auto before = CpuReading::now();
        float peak = 0.0f;
        for (int n = 0; n < blocks; ++n)
            for (int ch = 0; ch < 2; ++ch)
                peak = juce::jmax (peak * 0.5f, b.getMagnitude (ch, 0, kBlock));
        const double perBlock = (double) (CpuReading::now().instructions - before.instructions) / blocks;
        const double blocksPerSecond = kRate / kBlock;
        std::printf ("peak read: %.0f instructions a block, %.3f million a second, %.3f%% of one fast core (peak %.3f)\n", perBlock, perBlock * blocksPerSecond * 1.0e-6, perBlock * blocksPerSecond / 8.0e9 * 100.0, peak);
        return 0;
    }

    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? atoi (argv[2]) : 5, argc > 3 ? atoi (argv[3]) : 6, ! (argc > 4 && juce::String (argv[4]) == "silent"));

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    const auto sung = loadCorpus ("sung_goodnews");
    const auto speech = loadCorpus ("speech_samantha");

    if (sung.empty() || speech.empty())
    {
        std::printf ("FAILED could not read the recordings in tests/corpus\n");
        return 1;
    }

    const auto lastLoudFrame = [] (const std::vector<float>& v, int upTo)
    {
        // Stop mid word, so the picture shows the light under the singers.
        int best = upTo;
        for (int f = upTo; f > upTo - 40 && f > 0; --f)
        {
            float peak = 0.0f;
            for (int i = 0; i < kSamplesPerFrame * 2 && (size_t) (f * kSamplesPerFrame + i) < v.size(); ++i)
                peak = std::max (peak, std::abs (v[(size_t) (f * kSamplesPerFrame + i)]));
            if (peak > 0.35f)
            {
                best = f;
                break;
            }
        }
        return best;
    };

    const int sungEnd = lastLoudFrame (sung, std::min (205, (int) (sung.size() / kSamplesPerFrame) - 2));
    const int speechEnd = lastLoudFrame (speech, std::min (150, (int) (speech.size() / kSamplesPerFrame) - 2));

    // Idle: nothing has played. Everyone stands on their mark.
    {
        Session s;
        for (int i = 0; i < 40; ++i)
            s.editor->tick (1.0 / 30.0);
        s.saveBoth (folder, "idle");
    }

    // Active: the default, on sung material.
    {
        Session s;
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "active");
    }

    // Wide Stack: three doubles, the third with no partner, spread to both sides.
    {
        Session s;
        s.processor.setCurrentProgram (3);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "wide-stack");
    }

    // Loose Gang: long zones, far from their marks.
    {
        Session s;
        s.processor.setCurrentProgram (4);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "loose-gang");
    }

    // Tight Double, on speech.
    {
        Session s;
        s.processor.setCurrentProgram (1);
        s.play (speech, 0, speechEnd);
        s.saveBoth (folder, "tight-double");
    }

    // Width 0: everyone in a line behind the lead, tight.
    {
        Session s;
        s.processor.setCurrentProgram (0);
        setParam (s.processor, "width", 0.0f);
        setParam (s.processor, "tightness", 0.0f);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "narrow");
    }

    // Two Singers: lead on one side, one double on the other, different throat.
    {
        Session s;
        s.processor.setCurrentProgram (6);
        s.play (speech, 0, speechEnd);
        s.saveBoth (folder, "two-singers");
    }

    // Slapback: far back on the stage.
    {
        Session s;
        s.processor.setCurrentProgram (7);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "slapback");
    }

    // Advanced open, active, with a control hovered.
    {
        Session s (0, true);
        s.processor.setCurrentProgram (3);
        s.play (sung, 0, sungEnd);
        s.editor->previewHover ("timbre");
        s.saveBoth (folder, "advanced-open");
    }

    // The mouse over a double on the stage.
    {
        Session s;
        s.editor->previewHover ("singer1");
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "hover-double");
    }

    // Bypassed.
    {
        Session s;
        setParam (s.processor, "bypass", 1.0f);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "bypassed");
    }

    // The lead off, and one double off.
    {
        Session s;
        s.processor.setCurrentProgram (3);
        setParam (s.processor, "lead", -60.0f);
        setParam (s.processor, "double2", -60.0f);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "some-off");
    }

    // A mono track.
    {
        PluginProcessor p;
        p.setPlayConfigDetails (1, 1, kRate, kBlock);
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (juce::AudioChannelSet::mono());
        layout.outputBuses.add (juce::AudioChannelSet::mono());
        p.setBusesLayout (layout);
        p.prepareToPlay (kRate, kBlock);
        std::unique_ptr<DuoEditor> e (dynamic_cast<DuoEditor*> (p.createEditor()));
        e->setVisible (true);
        for (int i = 0; i < 40; ++i)
            e->tick (1.0 / 30.0);
        auto image = e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f);
        auto file = folder.getChildFile ("mono-track@1x.png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        std::printf ("%s  mono-track@1x.png\n", out.openedOk() && png.writeImageToStream (image, out) ? "saved" : "FAILED");
    }

    // The smallest size the window can be.
    {
        Session s (PluginProcessor::minWindowWidth);
        s.processor.setCurrentProgram (3);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "smallest");
    }

    return 0;
}
