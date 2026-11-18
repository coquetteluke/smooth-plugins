// Renders the real window to PNG with real speech running through the real processor, with no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// RenderWindow --live opens the real window with audio on a second thread and measures its CPU, against the same audio with no window.

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "TuneEditor.h"

#include <algorithm>
#include <atomic>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread/qos.h>
#include <time.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <thread>
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
        const std::string cmd = "say -v Samantha -r 150 -o '" + path + "' --file-format=WAVE --data-format=LEF32@48000 \"" + speechText + "\"";
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

// Gentle, audible tuning in A minor, so input and corrected pull apart and the note blocks show.
static void setUpActive (PluginProcessor& p)
{
    setParam (p, "key", 9.0f);
    setParam (p, "scale", (float) smoothtune::scaleMinor);
    setParam (p, "retune", 12.0f);
    setParam (p, "humanize", 0.0f);
    setParam (p, "flex", 0.0f);
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<TuneEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, bool advanced = false)
    {
        if (width > 0)
            processor.windowWidth.store (width);
        processor.advancedOpen.store (advanced);
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<TuneEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    ~Session() { editor.reset(); }

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
        // Let readouts that update a few times a second catch up to the frame that gets saved.
        for (int i = 0; i < 6; ++i)
            editor->tick (1.0 / 30.0);
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

// The method from ~/smooth-resonance/docs/window-cpu.md. A real window with all but one column of pixels off the edge of the screen, painted by the system at
// the screen's real pixel density, with speech running in real time on another thread. The window's share is everything this process used except the audio
// thread, timed by that thread itself in the same run. --live [runs] [seconds] [silent]
static int runLive (int repeats, int seconds, bool withAudio)
{
    const auto voice = loadVoice();
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   main%%  audio%%  Minstr/frame  load  builds\n");

    std::vector<double> results, work;
    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        Session s;
        setUpActive (s.processor);
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
    }

    for (int run = 0; run < repeats; ++run)
    {
        Session s;
        setUpActive (s.processor);

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

        juce::MessageManager::getInstance()->runDispatchLoopUntil (3000);

        const int paintsBefore = s.editor->getPitchView().getPaintCount();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int paints = s.editor->getPitchView().getPaintCount() - paintsBefore;
        const double window = (after.total - before.total - audioUsed) / wall;
        double load[1] { 0.0 };
        getloadavg (load, 1);
        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %5.1f  %5.1f  %5.1f  %11.1f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     paints / wall, mainUsed / wall * 100.0, audioUsed / wall * 100.0,
                     instructionRate / juce::jmax (1.0, paints / wall) * 1.0e-6, load[0], builds);
        std::fflush (stdout);
        results.push_back (window * 100.0);
        work.push_back (instructionRate / instructionsPerFastCoreSecond * 100.0);

        s.editor->removeFromDesktop();
        running = false;
        audio.join();
    }

    std::sort (results.begin(), results.end());
    std::sort (work.begin(), work.end());
    std::printf ("cpu time: lowest %.1f%%, median %.1f%%, highest %.1f%% of one core\n", results.front(), results[results.size() / 2], results.back());
    std::printf ("work, in fast core terms: lowest %.1f%%, median %.1f%%, highest %.1f%%\n", work.front(), work[work.size() / 2], work.back());
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? atoi (argv[2]) : 5, argc > 3 ? atoi (argv[3]) : 6, ! (argc > 4 && juce::String (argv[4]) == "silent"));

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    const auto voice = loadVoice();
    if (voice.empty())
    {
        std::printf ("FAILED could not render speech with say\n");
        return 1;
    }
    std::printf ("speech: %.1f s\n", (double) voice.size() / kRate);
    const int total = (int) (voice.size() / kSamplesPerFrame);

    // Eight seconds in, mid sentence, so the display is full.
    int frame = juce::jmin (total - 1, 10 * 30);

    // Search a little around there for a moment with a voice right at the newest point.
    {
        Session s;
        setUpActive (s.processor);
        int best = frame;
        s.play (voice, 0, frame);
        for (int f = frame; f < juce::jmin (total - 1, frame + 90); ++f)
        {
            s.play (voice, f, f + 1);
            const auto r = s.processor.getReadings();
            if (r.voiced && std::abs (r.correctionCents) > 25.0f)
            {
                best = f;
                break;
            }
        }
        frame = best;
        std::printf ("frame %d of %d\n", frame, total);
    }

    { Session s; s.saveBoth (folder, "idle"); }

    {
        Session s;
        setUpActive (s.processor);
        s.play (voice, 0, frame + 1);
        s.saveBoth (folder, "active");
    }

    // Advanced open, one control typed into and notes in each mode.
    {
        Session s (0, true);
        setUpActive (s.processor);
        setParam (s.processor, "note2", 1.0f);
        setParam (s.processor, "note9", 2.0f);
        setParam (s.processor, "note1", 1.0f);
        setParam (s.processor, "note5", 2.0f);
        setParam (s.processor, "note10", 2.0f);
        setParam (s.processor, "vibratoAmount", 25.0f);
        setParam (s.processor, "transpose", 2.0f);
        setParam (s.processor, "engine", 1.0f);
        s.play (voice, 0, frame + 1);
        s.editor->previewHover ("vibratoAmount");
        s.saveBoth (folder, "advanced-open");
    }

    // Hard tuning on Chromatic, closed, with hidden changes and a hovered lead control and a value being typed.
    {
        Session s;
        s.processor.setCurrentProgram (5);
        setParam (s.processor, "throat", 120.0f);
        setParam (s.processor, "bypass", 0.0f);
        s.play (voice, 0, frame + 1);
        s.editor->previewHover ("retune");
        if (auto* flex = dynamic_cast<tuneui::ParamSlider*> (s.editor->getPanel().findChildWithID ("flex")))
            flex->startTyping();
        s.saveBoth (folder, "hard-tune-typing");
    }

    // Bypassed.
    {
        Session s;
        setUpActive (s.processor);
        setParam (s.processor, "bypass", 1.0f);
        s.play (voice, 0, frame + 1);
        s.saveBoth (folder, "bypassed");
    }

    // Smallest size.
    {
        Session s (PluginProcessor::minWindowWidth);
        setUpActive (s.processor);
        s.play (voice, 0, frame + 1);
        s.saveBoth (folder, "smallest");
    }

    return 0;
}
