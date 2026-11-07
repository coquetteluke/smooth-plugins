// Renders the real window to PNG, with real speech from macOS `say` and test music through the real processor and its real
// background thread, with no host. Also measures the real window's CPU.
// From the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
//                     build/RenderWindow_artefacts/Release/RenderWindow --live [runs] [seconds] [speech, music or silent]

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "KeyEditor.h"
#include "editor/KeyViews.h"
#include "editor/Controls.h"
#include "KeyCorpus.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <vector>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef SPEECH_DIR
 #define SPEECH_DIR "build/speech"
#endif

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 512;

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

static std::vector<float> music (int tonic, bool minor, double seconds, double cents = 0.0)
{
    using namespace corpus;
    const auto ps = progressions();
    const Progression* chosen = &ps[0];
    for (auto& pr : ps)
        if (pr.minor == minor) { chosen = &pr; break; }

    Timbre t = timbres()[0];
    std::vector<float> out = chordClip (tonic, *chosen, t, 11u, kRate, seconds);

    if (cents != 0.0)
    {
        // Played back a little fast or slow, which is what makes a recording sit away from A 440.
        const double step = std::pow (2.0, cents / 1200.0);
        std::vector<float> moved;
        moved.reserve (out.size());
        for (double at = 0.0; at + 1.0 < (double) out.size(); at += step)
        {
            const auto i = (size_t) at;
            const float f = (float) (at - (double) i);
            moved.push_back (out[i] + (out[i + 1] - out[i]) * f);
        }
        out.swap (moved);
    }

    return out;
}

static void setParam (PluginProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static juce::String tempFile (const char* name)
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KeyRenderWindow").getChildFile (name).getFullPathName();
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<KeyEditor> editor;

    explicit Session (const juce::String& json = tempFile ("key.json"))
        : processor (json, 0.05)
    {
        juce::File (json).getParentDirectory().createDirectory();
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        juce::Thread::sleep (60);
        editor.reset (dynamic_cast<KeyEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    ~Session()
    {
        editor.reset();
    }

    // Faster than real time, but never faster than the background thread can take it.
    void play (const std::vector<float>& audio)
    {
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        int sincePause = 0;

        for (size_t pos = 0; pos + (size_t) kBlock <= audio.size(); pos += (size_t) kBlock)
        {
            for (int ch = 0; ch < 2; ++ch)
                std::copy_n (audio.data() + pos, kBlock, buffer.getWritePointer (ch));

            processor.processBlock (buffer, midi);

            if (++sincePause >= 24)
            {
                sincePause = 0;
                juce::Thread::sleep (32);
            }
        }

        juce::Thread::sleep (200);
        if (processor.getDroppedSamples() > 0)
            std::printf ("note: %u samples dropped\n", processor.getDroppedSamples());
        settle();
    }

    // The circle eases towards each reading, so give it the ticks it would have had by now.
    void settle()
    {
        for (int i = 0; i < 30; ++i)
            editor->refresh();
    }

    void save (const juce::File& folder, const juce::String& name, float scale)
    {
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
        auto file = folder.getChildFile (name + (scale > 1.5f ? "@2x.png" : "@1x.png"));
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        const bool ok = out.openedOk() && png.writeImageToStream (image, out);
        std::printf ("%s  %s  %d x %d  key %s  status \"%s\"\n", ok ? "saved" : "FAILED", file.getFileName().toRawUTF8(), image.getWidth(), image.getHeight(),
                     editor->getKeyText().toRawUTF8(), editor->getStatusText().toRawUTF8());
    }

    void saveBoth (const juce::File& folder, const juce::String& name)
    {
        save (folder, name, 1.0f);
        save (folder, name, 2.0f);
    }
};

// ---- the real window, on a real screen, in real time ----------------------------------------------------------------

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

// The CPU time used so far by the plug-in's own background analysis thread, found by its name. It runs with or without the window.
static double analysisCpuSeconds()
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    double total = 0.0;

    if (task_threads (mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return 0.0;

    for (mach_msg_type_number_t i = 0; i < count; ++i)
    {
        char name[64] = {};

        if (auto handle = pthread_from_mach_thread_np (threads[i]))
            pthread_getname_np (handle, name, sizeof name);

        if (std::strcmp (name, "Smooth Key analysis") == 0)
        {
            thread_basic_info_data_t info {};
            mach_msg_type_number_t infoCount = THREAD_BASIC_INFO_COUNT;

            if (thread_info (threads[i], THREAD_BASIC_INFO, (thread_info_t) &info, &infoCount) == KERN_SUCCESS)
                total += (double) info.user_time.seconds + (double) info.user_time.microseconds * 1.0e-6
                       + (double) info.system_time.seconds + (double) info.system_time.microseconds * 1.0e-6;
        }

        mach_port_deallocate (mach_task_self(), threads[i]);
    }

    vm_deallocate (mach_task_self(), (vm_address_t) threads, sizeof (thread_t) * count);
    return total;
}

static int runLive (int repeats, int seconds, const juce::String& what)
{
    const bool silent = what == "silent";
    std::vector<float> audio;

    if (what == "music")
        audio = music (7, true, 40.0);
    else if (! silent)
        audio = loadVoice();

    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio: %s\n", display != nullptr ? display->scale : 0.0, seconds, what.toRawUTF8());
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("run  window%%  fastcore-equiv%%  Pcore%%  paints/s  main%%  audio%%  analysis%%  Minstr/s  load  builds\n");

    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double aloneRate = 0.0;

    {
        // The instructions of audio and analysis alone, with no window, to take off in the fast core figure.
        PluginProcessor p (tempFile ("key.json"), 0.5);
        p.setPlayConfigDetails (2, 2, kRate, kBlock);
        p.prepareToPlay (kRate, kBlock);
        if (what == "speech") setParam (p, "source", 1.0f);
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
                    buffer.setSample (ch, i, silent ? 0.0f : audio[(at + (size_t) i) % audio.size()]);

            at += (size_t) kBlock;
            p.processBlock (buffer, midi);
            next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
            std::this_thread::sleep_until (next);
        }

        aloneRate = (double) (CpuReading::now().instructions - before.instructions) / 3.0;
        std::printf ("audio and analysis alone: %.0f million instructions a second\n", aloneRate * 1.0e-6);
    }

    std::vector<double> results, work;

    for (int run = 0; run < repeats; ++run)
    {
        Session s (tempFile ("key.json"));
        if (what == "speech") setParam (s.processor, "source", 1.0f);

        std::atomic<bool> running { true };
        std::atomic<double> audioCpu { 0.0 };

        std::thread audioThread ([&]
        {
            juce::AudioBuffer<float> buffer (2, kBlock);
            juce::MidiBuffer midi;
            size_t at = 0;
            auto next = std::chrono::steady_clock::now();

            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlock; ++i)
                        buffer.setSample (ch, i, silent ? 0.0f : audio[(at + (size_t) i) % audio.size()]);

                at += (size_t) kBlock;
                s.processor.processBlock (buffer, midi);
                audioCpu.store (threadCpuSeconds());
                next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
                std::this_thread::sleep_until (next);
            }
        });

        auto& editor = *s.editor;
        editor.setVisible (false);
        editor.setTopLeftPosition (1 - editor.getWidth(), 300);   // one column of pixels on screen, or macOS skips painting it
        editor.addToDesktop (juce::ComponentPeer::windowIgnoresKeyPresses | juce::ComponentPeer::windowIgnoresMouseClicks);
        editor.setVisible (true);

        // Long enough that a key is showing and the window is updating as it does in a song.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (9000);

        const int paintsBefore = keyui::paintCount;
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds(), analysisBefore = analysisCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore, analysisUsed = analysisCpuSeconds() - analysisBefore;
        const double window = (after.total - before.total - audioUsed - analysisUsed) / wall;
        const double rate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - aloneRate);

        double load[1] { 0.0 };
        getloadavg (load, 1);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %6.2f  %14.2f  %6.0f  %8.1f  %5.2f  %5.2f  %8.2f  %8.0f  %4.1f  %5d   key %s\n", run + 1, window * 100.0, rate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     (double) (keyui::paintCount - paintsBefore) / wall, mainUsed / wall * 100.0, audioUsed / wall * 100.0, analysisUsed / wall * 100.0,
                     rate * 1.0e-6, load[0], builds, editor.getKeyText().toRawUTF8());
        std::fflush (stdout);
        results.push_back (window * 100.0);
        work.push_back (rate / instructionsPerFastCoreSecond * 100.0);

        editor.removeFromDesktop();
        running = false;
        audioThread.join();
    }

    std::sort (results.begin(), results.end());
    std::sort (work.begin(), work.end());
    std::printf ("window cpu: lowest %.2f%%, median %.2f%%, highest %.2f%% of one core\n", results.front(), results[results.size() / 2], results.back());
    std::printf ("work, in fast core terms: lowest %.2f%%, median %.2f%%, highest %.2f%%\n", work.front(), work[work.size() / 2], work.back());
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? atoi (argv[2]) : 5, argc > 3 ? atoi (argv[3]) : 6, argc > 4 ? argv[4] : "speech");

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();
    const juce::String only = argc > 2 ? argv[2] : "";
    const auto wants = [&only] (const char* name) { return only.isEmpty() || only.contains (name); };

    const auto voice = loadVoice();

    if (voice.empty())
    {
        std::printf ("FAILED could not render speech with say\n");
        return 1;
    }

    std::printf ("speech: %.1f s from say\n", (double) voice.size() / kRate);

    std::vector<float> voiceTwice = voice;
    voiceTwice.insert (voiceTwice.end(), voice.begin(), voice.end());

    if (wants ("idle"))
    {
        Session s;
        juce::Thread::sleep (100);
        s.editor->refresh();
        s.saveBoth (folder, "idle");
    }

    if (wants ("speech"))
    {
        Session s;
        setParam (s.processor, "source", 1.0f);
        s.play (voiceTwice);
        s.saveBoth (folder, "active-speech");
    }

    const auto song = music (7, true, 26.0);

    if (wants ("music"))
    {
        Session s;
        s.play (song);
        s.saveBoth (folder, "active-music");
    }

    if (wants ("early"))
    {
        Session s;
        setParam (s.processor, "response", 1.0f);
        s.play (music (2, false, 5.0));
        s.saveBoth (folder, "listening-no-key-yet");
    }

    if (wants ("sharp"))
    {
        Session s;
        setParam (s.processor, "tempoScale", 2.0f);
        s.play (music (10, false, 26.0, 28.0));
        s.saveBoth (folder, "off-440-with-sharp");
    }

    if (wants ("swap"))
    {
        Session s;
        setParam (s.processor, "swapRelative", 1.0f);
        s.play (song);
        s.saveBoth (folder, "relative-swap");
    }

    if (wants ("noaudio"))
    {
        Session s;
        s.play (song);
        std::vector<float> nothing ((size_t) (kRate * 4.0), 0.0f);
        s.play (nothing);
        s.saveBoth (folder, "no-audio-last-key");
    }

    if (wants ("manual"))
    {
        Session s;
        setParam (s.processor, "mode", 1.0f);
        setParam (s.processor, "manualKey", 6.0f);
        setParam (s.processor, "manualScale", 0.0f);
        juce::Thread::sleep (120);
        s.settle();
        s.editor->previewHover ("circle");
        s.saveBoth (folder, "manual");
    }

    if (wants ("typing"))
    {
        Session s;
        setParam (s.processor, "mode", 1.0f);
        setParam (s.processor, "manualKey", 9.0f);
        setParam (s.processor, "manualScale", 1.0f);
        juce::Thread::sleep (120);
        s.settle();
        if (auto* hero = dynamic_cast<keyui::HeroView*> (s.editor->getPanel().findChildWithID ("hero")))
            hero->startTyping();
        s.saveBoth (folder, "manual-typing-a-key");
    }

    if (wants ("advanced"))
    {
        Session s;
        setParam (s.processor, "manualKey", 3.0f);
        s.editor->setAdvancedOpen (true, false);
        s.play (song);
        s.editor->previewHover ("response");
        s.saveBoth (folder, "advanced-open");
    }

    if (wants ("failed"))
    {
        // A file where the folder should be, so key.json really cannot be written.
        const auto blocker = juce::File (tempFile ("blocker"));
        blocker.getParentDirectory().createDirectory();
        blocker.replaceWithText ("x");
        Session s (blocker.getChildFile ("key.json").getFullPathName());
        s.play (song);
        s.saveBoth (folder, "file-failed");
        blocker.deleteFile();
    }

    if (wants ("smallest"))
    {
        Session s;
        s.editor->setSize (PluginProcessor::minWindowWidth, juce::roundToInt ((double) PluginProcessor::minWindowWidth * KeyEditor::designHeight / KeyEditor::designWidth));
        s.play (song);
        s.saveBoth (folder, "smallest");
    }

    return 0;
}
