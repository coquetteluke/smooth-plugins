// Renders the real window to PNG with real speech running through the real processor, with no screen and no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// Measures it too: RenderWindow --live [runs] [seconds] [silent or background]. See ~/smooth-resonance/docs/window-cpu.md.

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "editor/TapeChorusEditor.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifndef SPEECH_DIR
 #define SPEECH_DIR "build/speech"
#endif

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;
static constexpr int kSamplesPerFrame = 1600;   // one display frame at 30 a second

static const char* const speechText =
    "Every evening I sing the same slow song, and the sound rises and falls across the room. "
    "Oh, how we all love the warm low hum of a voice held long and open. "
    "Please leave the door open, because the bright voices echo through the hall.";

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
    std::unique_ptr<TapeChorusEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0)
    {
        if (width > 0)
            processor.windowWidth.store (width);

        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<TapeChorusEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    ~Session()
    {
        editor.reset();
    }

    /** Runs audio up to the given frame of the speech, ticking the display as the screen would. */
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

/** One reading of everything in this process, from the kernel. Times are CPU seconds. */
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

/** The real thing: a real window with all but one column of pixels off the edge of the screen, painted by the system at the
    screen's real pixel density, with speech running in real time on another thread.
    The window's share is everything this process used except the audio thread, timed by that thread itself in the same run. */
static int runLive (int repeats, int seconds, bool withAudio, bool background, int preset)
{
    auto voice = loadVoice();
    const auto setUp = [preset] (PluginProcessor& p) { p.setCurrentProgram (preset); };

    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (background ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("main thread is %s\n", background ? "background, so it stays on the slow cores: the worst case" : "user interactive, like a host's");
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   main%%  audio%%  Minstr/s  Minstr/frame  load  builds\n");

    std::vector<double> results, instructionRates;
    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        PluginProcessor p;
        p.setPlayConfigDetails (2, 2, kRate, kBlock);
        p.prepareToPlay (kRate, kBlock);
        setUp (p);
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
            p.processBlock (buffer, midi);
            next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
            std::this_thread::sleep_until (next);
        }

        audioInstructionRate = (double) (CpuReading::now().instructions - before.instructions) / 3.0;
        std::printf ("audio alone: %.0f million instructions a second\n", audioInstructionRate * 1.0e-6);
    }

    for (int run = 0; run < repeats; ++run)
    {
        Session s;
        setUp (s.processor);

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

        auto& view = s.editor->getView();
        const int paintsBefore = view.getPaintCount();
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

    // --live [runs] [seconds] [normal, silent or background] [preset]
    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? atoi (argv[2]) : 5, argc > 3 ? atoi (argv[3]) : 6, ! (argc > 4 && juce::String (argv[4]) == "silent"),
                        argc > 4 && juce::String (argv[4]) == "background", argc > 5 ? atoi (argv[5]) : 0);

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    auto voice = loadVoice();

    if (voice.empty())
    {
        std::printf ("no speech from say\n");
        return 1;
    }

    const int totalFrames = (int) (voice.size() / kSamplesPerFrame);
    const int moment = juce::jmin (totalFrames - 1, 195);   // well into a phrase, so the tape is full

    {
        // Nothing has run: it draws what the settings will do.
        Session s;
        s.saveBoth (folder, "idle");
    }

    {
        Session s;
        s.play (voice, 0, moment + 1);
        s.saveBoth (folder, "active");
    }

    {
        Session s;
        s.processor.setCurrentProgram (7);   // Eight Voice Stack
        s.play (voice, 0, moment + 1);
        s.saveBoth (folder, "eight-voices");
    }

    {
        Session s;
        s.processor.setCurrentProgram (3);   // Worn Cassette
        setParam (s.processor, "degrade", 80.0f);
        s.play (voice, 0, moment + 1);
        s.saveBoth (folder, "worn-tape");
    }

    {
        Session s;
        s.processor.setCurrentProgram (12);   // Resonant Tape Swirl
        s.editor->setAdvancedOpen (true, false);
        s.play (voice, 0, moment + 1);
        s.saveBoth (folder, "advanced-open");
    }

    {
        Session s;
        s.processor.setCurrentProgram (5);   // Fast Shimmer
        s.play (voice, 0, moment + 1);
        s.saveBoth (folder, "fast-rate");
    }

    {
        Session s;
        s.processor.setCurrentProgram (4);   // Slow Drift
        setParam (s.processor, "bypass", 1.0f);
        s.play (voice, 0, moment + 1);
        s.saveBoth (folder, "bypassed");
    }

    {
        Session s;
        setParam (s.processor, "inputGain", 16.0f);
        s.play (voice, 0, moment + 1);
        s.editor->previewHover ("depth");

        if (auto* field = dynamic_cast<ui::ParamStrip*> (s.editor->getPanel().findChildWithID ("delay")))
            field->startTyping();

        s.saveBoth (folder, "hovered-typed-over");
    }

    // Motion check: six moments of the same phrase, about 130 ms apart, stacked into one picture.
    {
        Session s;
        s.processor.setCurrentProgram (2);
        const int start = juce::jmax (0, moment - 24);
        s.play (voice, 0, start);
        constexpr int shots = 6;
        const auto bounds = s.editor->getView().getBounds();
        juce::Image sheet (juce::Image::RGB, bounds.getWidth(), bounds.getHeight() * 3 / 2, true);
        juce::Graphics g (sheet);

        for (int i = 0; i < shots; ++i)
        {
            s.play (voice, start + i * 4, start + i * 4 + 4);
            auto frameImage = s.editor->getView().createComponentSnapshot (s.editor->getView().getLocalBounds(), true, 1.0f);
            const int w = sheet.getWidth() / 2, h = sheet.getHeight() / 3;
            g.drawImage (frameImage, juce::Rectangle<float> ((float) (i % 2) * (float) w, (float) (i / 2) * (float) h, (float) w, (float) h));
        }

        auto file = folder.getChildFile ("motion-six-frames.png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (sheet, out);
    }

    // The smallest size the window can be.
    {
        Session s (PluginProcessor::minWindowWidth);
        s.processor.setCurrentProgram (9);
        s.play (voice, 0, moment + 1);
        s.saveBoth (folder, "smallest");
    }

    return 0;
}
