// Renders the real window to PNG with real speech running through the real processor, with no screen and no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// Speech comes from macOS `say`. Also measures the window on a real screen: RenderWindow --live [runs] [seconds] [silent or background]
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "window/FlangerEditor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifndef SPEECH_DIR
 #define SPEECH_DIR "build/speech"
#endif

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;
static constexpr int kSamplesPerFrame = 1600;   // one display frame at 30 a second

static const char* const speechText =
    "She sells seashells by the seashore, and the shells she sells are surely seashells. "
    "Every evening I sing the same slow song, and the sound rises and falls across the room. "
    "Please leave the door open, because the sharp bright voices echo through the hall.";

static std::vector<float> readFloatWav (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    std::vector<char> bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    std::vector<float> out;
    if (bytes.size() < 44 || std::memcmp (bytes.data(), "RIFF", 4) != 0)
        return out;

    size_t pos = 12;
    int channels = 1;

    while (pos + 8 <= bytes.size())
    {
        uint32_t size = 0;
        std::memcpy (&size, bytes.data() + pos + 4, 4);

        if (std::memcmp (bytes.data() + pos, "fmt ", 4) == 0)
        {
            uint16_t c = 1;
            std::memcpy (&c, bytes.data() + pos + 10, 2);
            channels = c;
        }
        else if (std::memcmp (bytes.data() + pos, "data", 4) == 0)
        {
            const size_t frames = std::min ((size_t) size, bytes.size() - pos - 8) / (4 * (size_t) channels);
            out.resize (frames);
            for (size_t i = 0; i < frames; ++i)
                std::memcpy (&out[i], bytes.data() + pos + 8 + i * 4 * (size_t) channels, 4);
            break;
        }

        pos += 8 + size + (size & 1);
    }

    return out;
}

static std::vector<float> loadVoice()
{
    mkdir (SPEECH_DIR, 0755);
    const std::string path = std::string (SPEECH_DIR) + "/WindowVoice.wav";
    auto x = readFloatWav (path);

    if (x.empty())
    {
        const std::string cmd = "say -v Samantha -o '" + path + "' --file-format=WAVE --data-format=LEF32@48000 \"" + speechText + "\"";
        if (std::system (cmd.c_str()) == 0)
            x = readFloatWav (path);
    }

    double peak = 0.0;
    for (auto v : x) peak = std::max (peak, (double) std::abs (v));
    if (peak > 0.0)
        for (auto& v : x) v *= (float) (0.5 / peak);

    return x;
}

static void setParam (PluginProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<FlangerEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;
    size_t at = 0;

    explicit Session (int width = 0)
    {
        if (width > 0)
            processor.windowWidth.store (width);

        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<FlangerEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    ~Session() { editor.reset(); }

    /** One display frame of speech, looping. Then one tick, as the screen would give it. */
    void frame (const std::vector<float>& voice)
    {
        for (int done = 0; done < kSamplesPerFrame; done += kBlock)
        {
            const int n = std::min (kBlock, kSamplesPerFrame - done);
            buffer.setSize (2, n, false, false, true);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    buffer.setSample (ch, i, voice[(at + (size_t) i) % voice.size()]);
            at += (size_t) n;
            processor.processBlock (buffer, midi);
        }

        editor->tick (1.0 / 30.0);
    }

    void frames (const std::vector<float>& voice, int count)
    {
        for (int i = 0; i < count; ++i)
            frame (voice);
    }

    /** Runs on until a deck has just gone through zero, and the voice is making sound. */
    void untilCrossing (const std::vector<float>& voice, int limit = 900)
    {
        for (int i = 0; i < limit; ++i)
        {
            frame (voice);
            if (editor->getField().getCrossingFlash() > 0.99f && processor.getReadings().outputPeak[0] > 0.02f)
                return;
        }
    }

    void save (const juce::File& folder, const juce::String& name, float scale)
    {
        editor->getField().finishPicture();
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

/** One reading of everything in this process, from the kernel. */
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

/** A real window with all but one column of pixels off the edge of the screen, painted by the system at the screen's real pixel
    density, with speech running in real time on another thread. The window's share is everything this process used except the
    audio thread, timed by that thread itself in the same run. Method from ~/smooth-resonance/docs/window-cpu.md. */
static int runLive (int repeats, int seconds, bool withAudio, bool background, const juce::String& scene)
{
    auto voice = loadVoice();

    const auto setScene = [&] (PluginProcessor& p)
    {
        if (scene == "manual")
            p.setCurrentProgram (7);   // Manual Ride. The hand moves it below.
        if (scene == "manualrest")
        {
            p.setCurrentProgram (7);
            setParam (p, "flange", 2.0f);
        }
        if (scene == "advanced")
            p.advancedOpen.store (true);
    };

    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s, scene %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off", scene.toRawUTF8());
    pthread_set_qos_class_self_np (background ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   main%%  audio%%  Minstr/s  Minstr/frame  load  builds\n");

    std::vector<double> results, instructionRates;
    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        PluginProcessor processor;
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        setScene (processor);
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
            processor.processBlock (buffer, midi);
            next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
            std::this_thread::sleep_until (next);
        }

        audioInstructionRate = (double) (CpuReading::now().instructions - before.instructions) / 3.0;
        std::printf ("audio alone: %.0f million instructions a second\n", audioInstructionRate * 1.0e-6);
    }

    for (int run = 0; run < repeats; ++run)
    {
        Session s;
        setScene (s.processor);

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

        auto& field = s.editor->getField();
        const int paintsBefore = field.getPaintCount();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        if (scene == "manual")
        {
            // A hand riding Flange slowly back and forth through zero.
            for (int i = 0; i < seconds * 30; ++i)
            {
                setParam (s.processor, "flange", 3.0f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 0.3 * i / 30.0));
                juce::MessageManager::getInstance()->runDispatchLoopUntil (33);
            }
        }
        else if (scene == "harness")
        {
            // The same loop as the two scenes either side, moving nothing. Shows what the loop itself costs.
            for (int i = 0; i < seconds * 30; ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (33);
        }
        else if (scene == "feedback")
        {
            // Automation sweeping Feedback the whole time, which is the most the cached picture ever has to work.
            for (int i = 0; i < seconds * 30; ++i)
            {
                setParam (s.processor, "feedback", 90.0f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 0.2 * i / 30.0));
                juce::MessageManager::getInstance()->runDispatchLoopUntil (33);
            }
            std::printf ("cached picture took %.1f ms to work out, last time\n", field.getLastRebuildMs());
        }
        else
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);
        }

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int paints = juce::jmax (1, field.getPaintCount() - paintsBefore);
        const double window = (after.total - before.total - audioUsed) / wall;

        double load[1] { 0.0 };
        getloadavg (load, 1);

        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %5.1f  %5.1f  %5.1f  %8.0f  %11.1f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     paints / wall, mainUsed / wall * 100.0, audioUsed / wall * 100.0,
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

    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? atoi (argv[2]) : 5, argc > 3 ? atoi (argv[3]) : 6,
                        ! (argc > 4 && juce::String (argv[4]) == "silent"), argc > 4 && juce::String (argv[4]) == "background",
                        argc > 4 ? juce::String (argv[4]) : juce::String ("lfo"));

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();
    const juce::String only = argc > 2 ? argv[2] : "";
    const auto want = [&] (const char* name) { return only.isEmpty() || only == name; };

    auto voice = loadVoice();
    if (voice.empty())
    {
        std::printf ("FAILED could not render speech with say\n");
        return 1;
    }

    if (want ("idle"))
    {
        Session s;
        s.editor->tick (1.0 / 30.0);
        s.saveBoth (folder, "idle");
    }

    if (want ("active"))
    {
        // Default settings, on speech, a few sweeps in.
        Session s;
        s.frames (voice, 268);
        s.saveBoth (folder, "active");
    }

    if (want ("crossing"))
    {
        Session s;
        s.frames (voice, 150);
        s.untilCrossing (voice);
        s.frames (voice, 3);   // the lines running out from zero
        s.saveBoth (folder, "crossing");
    }

    if (want ("manual"))
    {
        // Manual Ride, with a hand moving Flange slowly from early to late and part of the way back.
        Session s;
        s.processor.setCurrentProgram (7);
        for (int i = 0; i < 150; ++i)
        {
            setParam (s.processor, "flange", 2.6f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * i / 120.0));
            s.frame (voice);
        }
        s.saveBoth (folder, "manual-riding");
    }

    if (want ("roar"))
    {
        Session s;
        s.processor.setCurrentProgram (2);   // Takeoff Roar
        s.frames (voice, 300);
        s.saveBoth (folder, "takeoff-roar");
    }

    if (want ("hollow"))
    {
        Session s;
        s.processor.setCurrentProgram (3);   // Hollow Inverted
        s.frames (voice, 150);
        s.untilCrossing (voice);
        s.saveBoth (folder, "hollow-inverted");
    }

    if (want ("offzero"))
    {
        Session s;
        s.processor.setCurrentProgram (9);   // Off Zero Double
        s.frames (voice, 200);
        s.saveBoth (folder, "off-zero");
    }

    if (want ("advanced"))
    {
        Session s;
        s.editor->setAdvancedOpen (true, false);
        s.processor.setCurrentProgram (1);   // Wide Vocal Sweep
        s.frames (voice, 200);
        s.editor->previewHover ("lfoshape");
        s.saveBoth (folder, "advanced-open");
    }

    if (want ("synced"))
    {
        Session s;
        s.processor.setCurrentProgram (4);   // One Bar Swirl
        s.frames (voice, 180);
        s.editor->previewHover ("division");
        s.saveBoth (folder, "synced-hovered");
    }

    if (want ("dry"))
    {
        // Manual Ride, sitting on 0: dry only.
        Session s;
        s.processor.setCurrentProgram (7);
        s.frames (voice, 120);
        s.saveBoth (folder, "manual-dry");
    }

    if (want ("motion"))
    {
        // Six frames around a crossing, a frame apart, to check the motion reads.
        Session s;
        setParam (s.processor, "spread", 0.0f);
        setParam (s.processor, "rate", 0.6f);
        s.frames (voice, 60);
        s.untilCrossing (voice);
        juce::Image sheet (juce::Image::RGB, 3 * 700 + 40, 2 * 460 + 20, true);
        juce::Graphics g (sheet);
        for (int i = 0; i < 6; ++i)
        {
            auto bounds = s.editor->getLocalBounds().withTrimmedTop (76).withTrimmedBottom (174).withTrimmedLeft (84).withTrimmedRight (84);
            s.editor->getField().finishPicture();
            auto shot = s.editor->createComponentSnapshot (bounds, true, 1.0f);
            g.drawImageAt (shot.rescaled (700, 460), (i % 3) * 720, (i / 3) * 480);
            s.frames (voice, 2);
        }
        auto file = folder.getChildFile ("motion-six-frames.png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        png.writeImageToStream (sheet, out);
        std::printf ("saved motion-six-frames.png\n");
    }

    if (want ("smallest"))
    {
        Session s (PluginProcessor::minWindowWidth);
        s.frames (voice, 150);
        s.saveBoth (folder, "smallest");
    }

    return 0;
}
