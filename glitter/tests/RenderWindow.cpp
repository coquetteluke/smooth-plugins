// Renders the real window to PNG with real speech running through the real processor, with no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// RenderWindow --live [runs] [seconds] [audio, silent or background] [module] opens the real window with audio on a second thread and measures its CPU in the same run.

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "TuneFxEditor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

// The song's key, tuned hard enough to see, with a few modules on so the row and strip have something to say.
static void setUpActive (PluginProcessor& p)
{
    setParam (p, "key", 9.0f);
    setParam (p, "scale", (float) smoothtune::scaleMinor);
    setParam (p, "retune", 12.0f);
    setParam (p, "harmonyOn", 1.0f);
    setParam (p, "harmonyInterval1", 7.0f);
    setParam (p, "harmonyVoices", 0.0f);
    setParam (p, "doublerOn", 1.0f);
    setParam (p, "filterOn", 1.0f);
    setParam (p, "filterType", 2.0f);
    setParam (p, "filterCutoff", 4200.0f);
    setParam (p, "filterResonance", 45.0f);
    setParam (p, "filterSweepDepth", 1.2f);
    setParam (p, "delayOn", 1.0f);
    setParam (p, "delayTime", 6.0f);
    setParam (p, "delayFeedback", 45.0f);
    setParam (p, "reverbOn", 1.0f);
    setParam (p, "reverbMix", 22.0f);
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<TuneFxEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, bool advanced = false, int module = 0)
    {
        if (width > 0)
            processor.windowWidth.store (width);
        processor.advancedOpen.store (advanced);
        processor.selectedModule.store (module);
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<TuneFxEditor*> (processor.createEditor()));
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

static double threadCpuSeconds()
{
    return (double) clock_gettime_nsec_np (CLOCK_THREAD_CPUTIME_ID) * 1.0e-9;
}

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

/** The real thing: a real window with all but one column of pixels off the edge of the screen, painted by the system at the screen's real pixel
    density, with speech running in real time on another thread. The window's share is everything this process used except the audio thread,
    timed by that thread itself in the same run. */
static int runLive (int repeats, int seconds, bool withAudio, bool background, int module)
{
    const auto voice = loadVoice();
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (background ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("main thread is %s\n", background ? "background, so it stays on the slow cores: the worst case" : "user interactive, like a host's");
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   main%%  audio%%  Minstr/s  load  builds\n");

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
        std::printf ("audio alone: %.0f million instructions a second\n", audioInstructionRate * 1.0e-6);
    }

    std::vector<double> results, work;

    for (int run = 0; run < repeats; ++run)
    {
        Session s (0, false, module);
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

        juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);

        const int paintsBefore = s.editor->getStrip().getPaintCount();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int paints = s.editor->getStrip().getPaintCount() - paintsBefore;
        const double window = (after.total - before.total - audioUsed) / wall;

        double load[1] { 0.0 };
        getloadavg (load, 1);

        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %5.1f  %5.1f  %5.1f  %8.0f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     paints / wall, mainUsed / wall * 100.0, audioUsed / wall * 100.0, instructionRate * 1.0e-6, load[0], builds);
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

    // --live [runs] [seconds] [audio, silent or background] [module]
    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? juce::String (argv[2]).getIntValue() : 3,
                        argc > 3 ? juce::String (argv[3]).getIntValue() : 8,
                        ! (argc > 4 && juce::String (argv[4]) == "silent"),
                        argc > 4 && juce::String (argv[4]) == "background",
                        argc > 5 ? juce::String (argv[5]).getIntValue() : 7);    // 7 is Filter, with its sweep moving: the busiest panel

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

    // Ten seconds in, mid sentence, so the display is full.
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
        Session s (0, false, 7);
        setUpActive (s.processor);
        s.play (voice, 0, frame + 1);
        s.saveBoth (folder, "active");
    }

    // Each module's own panel, with its module on, on the same voice.
    static const char* const names[] = { "pitch", "motion", "throat", "harmony", "doubler", "vocoder", "distortion", "filter", "ring", "delay", "reverb" };
    const auto shortPlay = juce::jmin (frame + 1, 6 * 30);
    for (int m = 0; m < fxui::numModules; ++m)
    {
        Session s (0, false, m);
        setUpActive (s.processor);
        setParam (s.processor, fxui::modules()[(size_t) m].onId, 1.0f);
        switch (m)
        {
            case 0: setParam (s.processor, "pitchSemitones", 7.0f); setParam (s.processor, "pitchOctave", -1.0f); break;
            case 1: setParam (s.processor, "motionPattern", 7.0f); setParam (s.processor, "motionRate", 3.0f); break;
            case 2: setParam (s.processor, "throatLength", 124.0f); break;
            case 3: setParam (s.processor, "harmonyVoices", 1.0f); setParam (s.processor, "harmonyInterval1", 4.0f); setParam (s.processor, "harmonyInterval2", -5.0f); setParam (s.processor, "harmonyWidth", 80.0f); break;
            case 5: setParam (s.processor, "vocoderMix", 70.0f); setParam (s.processor, "vocoderWave", 2.0f); break;
            case 6: setParam (s.processor, "distDrive", 55.0f); setParam (s.processor, "distTone", 6000.0f); break;
            case 8: setParam (s.processor, "ringFrequency", 310.0f); setParam (s.processor, "ringMix", 60.0f); break;
            case 9: setParam (s.processor, "delayPingPong", 1.0f); setParam (s.processor, "delayMix", 40.0f); break;
            case 10: setParam (s.processor, "reverbDecay", 3.4f); setParam (s.processor, "reverbPreDelay", 60.0f); setParam (s.processor, "reverbType", 2.0f); break;
            default: break;
        }
        s.play (voice, 0, shortPlay);
        s.save (folder, juce::String ("module-") + names[m], 1.0f);
        s.save (folder, juce::String ("module-") + names[m], 2.0f);
    }

    // A module that is off, selected, with a value being typed and another control under the mouse.
    {
        Session s (0, false, 9);
        s.processor.setCurrentProgram (0);
        s.play (voice, 0, shortPlay);
        s.editor->previewHover ("delayFeedback");
        if (auto* mix = dynamic_cast<fxui::ParamSlider*> (s.editor->findControl ("delayMix")))
            mix->startTyping();
        s.saveBoth (folder, "module-off-typing");
    }

    // Advanced open, with Scale on Custom and notes in each mode.
    {
        Session s (0, true, 3);
        setUpActive (s.processor);
        setParam (s.processor, "scale", (float) smoothtune::scaleCustom);
        setParam (s.processor, "note1", 1.0f);
        setParam (s.processor, "note3", 1.0f);
        setParam (s.processor, "note6", 1.0f);
        setParam (s.processor, "note8", 1.0f);
        setParam (s.processor, "note10", 1.0f);
        setParam (s.processor, "note5", 2.0f);
        setParam (s.processor, "midiTarget", 2.0f);
        setParam (s.processor, "tracking", 65.0f);
        s.play (voice, 0, shortPlay);
        s.saveBoth (folder, "advanced-open");
    }

    // Everything on, in a preset that uses most of the row.
    {
        Session s (0, false, 3);
        for (int m = 0; m < fxui::numModules; ++m)
            setParam (s.processor, fxui::modules()[(size_t) m].onId, 1.0f);
        s.play (voice, 0, shortPlay);
        s.saveBoth (folder, "all-on");
    }

    // Bypassed.
    {
        Session s (0, false, 7);
        setUpActive (s.processor);
        setParam (s.processor, "bypass", 1.0f);
        s.play (voice, 0, shortPlay);
        s.saveBoth (folder, "bypassed");
    }

    // Smallest size.
    {
        Session s (PluginProcessor::minWindowWidth, false, 7);
        setUpActive (s.processor);
        s.play (voice, 0, shortPlay);
        s.saveBoth (folder, "smallest");
    }

    return 0;
}
