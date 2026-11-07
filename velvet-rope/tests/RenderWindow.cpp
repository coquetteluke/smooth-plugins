// Renders the real window to PNG with real speech running through the real processor, with no screen and no host.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
// Speech comes from macOS `say`, with resonant boosts added at known frequencies, the same way as SpeechTest.
// Also times the window's work per frame, so its CPU use is a number and not a guess.

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "editor/ResonanceEditor.h"

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

using ui::ParamSlider;

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;
static constexpr int kSamplesPerFrame = 800;   // one display frame at 60 a second

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

/** The same resonant boost a harsh room or mic gives: a narrow peaking filter. */
static void addResonance (std::vector<float>& x, double hz, double q, double gainDb)
{
    const double a = std::pow (10.0, gainDb / 40.0), w = 2.0 * juce::MathConstants<double>::pi * hz / kRate;
    const double alpha = std::sin (w) / (2.0 * q), cosw = std::cos (w), a0 = 1.0 + alpha / a;
    const double b0 = (1.0 + alpha * a) / a0, b1 = -2.0 * cosw / a0, b2 = (1.0 - alpha * a) / a0, a1 = b1, a2 = (1.0 - alpha / a) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    for (auto& v : x)
    {
        const double in = v, out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in; y2 = y1; y1 = out;
        v = (float) out;
    }
}

static void setParam (SmoothResonanceProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.parameters.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

struct Session
{
    SmoothResonanceProcessor processor;
    std::unique_ptr<ResonanceEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0)
    {
        if (width > 0)
            processor.windowWidth.store (width);

        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<ResonanceEditor*> (processor.createEditor()));
        editor->setVisible (true);
    }

    ~Session()
    {
        editor.reset();
    }

    /** Runs audio up to the given frame of the speech, ticking the display as the screen would. */
    void play (const std::vector<float>& voice, int startFrame, int endFrame, const std::vector<float>* rightVoice = nullptr)
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
                        const auto& source = (ch == 1 && rightVoice != nullptr) ? *rightVoice : voice;
                        buffer.setSample (ch, i, at < source.size() ? source[at] : 0.0f);
                    }

                processor.processBlock (buffer, midi);
            }

            editor->tick (1.0 / 60.0);
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

/** The real thing: a real window with all but one column of pixels off the edge of the screen, painted by the system at the
    screen's real pixel density, with speech running in real time on another thread.
    The window's share is everything this process used except the audio thread, timed by that thread itself in the same run.
    There is no second "audio only" run to subtract, because that baseline moves by several percent between runs. */
static int runLive (int repeats, int seconds, bool withAudio, bool background)
{
    auto voice = loadVoice();
    addResonance (voice, 2600.0, 7.0, 11.0);
    addResonance (voice, 4300.0, 8.0, 10.0);
    addResonance (voice, 7400.0, 6.0, 9.0);

    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    std::printf ("screen %.0fx, %d seconds a run, audio %s\n", display != nullptr ? display->scale : 0.0, seconds, withAudio ? "on" : "off");
    pthread_set_qos_class_self_np (background ? QOS_CLASS_BACKGROUND : QOS_CLASS_USER_INTERACTIVE, 0);   // a host's main thread is user interactive
    std::printf ("main thread is %s\n", background ? "background, so it stays on the slow cores: the worst case" : "user interactive, like a host's");
    std::printf ("run  cpu%%   fastcore-equiv%%  Pcore%%  fps   ms/paint  area   main%%  audio%%  Minstr/s  Minstr/frame  load  builds\n");

    std::vector<double> results, instructionRates;

    // Instructions are the one number that does not depend on which core ran them, so the audio's share can fairly be
    // measured apart and taken off. A fast core gets through about 8 billion a second, which turns the rate into a percentage.
    constexpr double instructionsPerFastCoreSecond = 8.0e9;
    double audioInstructionRate = 0.0;

    if (withAudio)
    {
        Session s;
        s.processor.setCurrentProgram (1);
        setParam (s.processor, "depth", 14.0f);
        if (std::getenv ("RENDER_WINDOW_LISTEN") != nullptr)
            setParam (s.processor, "bandListen", 2.0f);

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
        s.processor.setCurrentProgram (1);
        setParam (s.processor, "depth", 14.0f);
        if (std::getenv ("RENDER_WINDOW_LISTEN") != nullptr)
            setParam (s.processor, "bandListen", 2.0f);


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

        auto& graph = s.editor->getGraph();
        const int paintsBefore = graph.getPaintCount();
        const double paintBefore = graph.getPaintSeconds(), areaBefore = graph.getPaintedArea();
        const auto before = CpuReading::now();
        const double audioBefore = audioCpu.load(), mainBefore = threadCpuSeconds();
        const double wallBefore = juce::Time::getMillisecondCounterHiRes();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);

        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallBefore) * 0.001;
        const auto after = CpuReading::now();
        const double audioUsed = audioCpu.load() - audioBefore, mainUsed = threadCpuSeconds() - mainBefore;
        const int paints = juce::jmax (1, graph.getPaintCount() - paintsBefore);
        const double window = (after.total - before.total - audioUsed) / wall;

        double load[1] { 0.0 };
        getloadavg (load, 1);

        const double instructionRate = juce::jmax (0.0, (double) (after.instructions - before.instructions) / wall - audioInstructionRate);
        const auto builds = std::system ("exit $(pgrep -f 'clang|cmake --build' | wc -l)") >> 8;

        std::printf ("%3d  %5.1f  %12.1f  %8.0f  %5.1f  %7.2f  %4.0f%%  %5.1f  %5.1f  %8.0f  %11.1f  %4.1f  %5d\n", run + 1, window * 100.0,
                     instructionRate / instructionsPerFastCoreSecond * 100.0,
                     (after.pCores - before.pCores) / juce::jmax (1.0e-6, after.total - before.total) * 100.0,
                     paints / wall,
                     (graph.getPaintSeconds() - paintBefore) * 1000.0 / paints,
                     (graph.getPaintedArea() - areaBefore) / paints / juce::jmax (1.0, (double) graph.getWidth() * graph.getHeight()) * 100.0,
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

    // --live [runs] [seconds] [silent or background]
    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? atoi (argv[2]) : 5, argc > 3 ? atoi (argv[3]) : 6, ! (argc > 4 && juce::String (argv[4]) == "silent"), argc > 4 && juce::String (argv[4]) == "background");

    const bool cpuOnly = argc > 1 && juce::String (argv[1]) == "--cpu";
    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 && ! cpuOnly ? argv[1] : "docs/window-v3");
    folder.createDirectory();

    auto voice = loadVoice();

    if (voice.empty())
    {
        std::printf ("FAILED could not render speech with say\n");
        return 1;
    }

    addResonance (voice, 2600.0, 7.0, 11.0);
    addResonance (voice, 4300.0, 8.0, 10.0);
    addResonance (voice, 7400.0, 6.0, 9.0);

    const int totalFrames = (int) (voice.size() / kSamplesPerFrame);

    const auto setUpActive = [] (SmoothResonanceProcessor& p)
    {
        p.setCurrentProgram (1);   // Harsh Lead Vocal
        setParam (p, "depth", 14.0f);
    };

    // State 1: idle, nothing has played.
    if (! cpuOnly)
    {
        Session s;
        s.saveBoth (folder, "idle");
    }

    // Find the frame with the most going on, then render there.
    int bestFrame = totalFrames / 2;
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        double bestScore = -1.0;

        for (int frame = 0; frame < totalFrames - 1; ++frame)
        {
            s.play (voice, frame, frame + 1);
            const double score = s.processor.maxReduction->getReading();

            if (frame > 30 && score > bestScore)
            {
                bestScore = score;
                bestFrame = frame;
            }
        }

        std::printf ("loudest moment: frame %d of %d, %.1f dB taken out at %.0f Hz\n", bestFrame, totalFrames, bestScore, (double) s.processor.maxReductionFreq->getReading());
    }

    // State 2: active, with reduction, on real speech.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        s.play (voice, 0, bestFrame + 1);
        s.saveBoth (folder, "active");
    }

    // State 3: Delta on, with Depth hovered, which draws the limit on the graph.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        setParam (s.processor, "delta", 1.0f);
        s.play (voice, 0, bestFrame + 1);
        s.editor->previewHover ("depth");
        s.saveBoth (folder, "delta-depth-hovered");
    }

    // State 4: side chain row hovered, bypass on, band 3 selected and pulled up, stereo apart.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        setParam (s.processor, "bypass", 1.0f);
        setParam (s.processor, "band3Sens", 6.0f);
        setParam (s.processor, "band1Sens", -4.0f);
        s.editor->getGraph().setSelectedBand (2);
        s.play (voice, 0, bestFrame + 1);
        s.editor->previewHover ("link");
        s.saveBoth (folder, "bypass-band3");
    }

    // State 5: a dot being dragged with the real mouse handlers, and a value being typed.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        s.play (voice, 0, bestFrame + 1);

        auto& graph = s.editor->getGraph();
        const auto xFor = [&graph] (float hz) { return (float) graph.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f); };
        const auto yFor = [&graph] (float sens) { return (float) graph.getHeight() * (0.5f - sens / 12.0f * 0.30f); };
        const auto event = [&graph] (juce::Point<float> at)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &graph, &graph, now, at, now, 1, false);
        };

        const juce::Point<float> from (xFor (3200.0f), yFor (2.0f)), to (xFor (5200.0f), yFor (7.5f));
        graph.mouseDown (event (from));
        graph.mouseDrag (event (to));

        if (auto* field = dynamic_cast<ParamSlider*> (s.editor->getPanel().findChildWithID ("attack")))
            field->startTyping();

        s.saveBoth (folder, "dot-dragged-value-typed");
        graph.mouseUp (event (to));
    }

    // State 6: Advanced open, in Hard mode with a preset that changes a few things.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        setParam (s.processor, "mode", 1.0f);
        setParam (s.processor, "stereoMode", 1.0f);
        setParam (s.processor, "stereoFocus", 30.0f);
        s.editor->setAdvancedOpen (true, false);
        s.play (voice, 0, bestFrame + 1);
        s.saveBoth (folder, "advanced-open");
    }

    // State 7: Advanced closed with changes inside it, which the closed header reports.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        setParam (s.processor, "stereoFocus", 30.0f);
        setParam (s.processor, "sharpnessTilt", -20.0f);
        s.play (voice, 0, 30);
        s.saveBoth (folder, "advanced-closed-with-changes");
    }

    // State 8: left and right treated apart. The right channel has an extra resonance the left does not, and Link is off.
    if (! cpuOnly)
    {
        auto rightVoice = voice;
        addResonance (rightVoice, 5600.0, 7.0, 12.0);
        Session s;
        setUpActive (s.processor);
        setParam (s.processor, "link", 0.0f);
        s.editor->setAdvancedOpen (true, false);
        s.play (voice, 0, bestFrame + 1, &rightVoice);
        s.saveBoth (folder, "channels-apart");
    }

    // State 9: all 8 dots in use, the preset that shows them.
    if (! cpuOnly)
    {
        Session s;
        s.processor.setCurrentProgram (9);
        s.editor->getGraph().setSelectedBand (5);
        s.play (voice, 0, bestFrame + 1);
        s.saveBoth (folder, "eight-dots");
    }

    // State 10: Band Listen on, which has to be impossible to miss.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        s.editor->getGraph().setSelectedBand (1);
        setParam (s.processor, "bandListen", 2.0f);
        s.play (voice, 0, bestFrame + 1);
        s.saveBoth (folder, "band-listen");
    }

    // State 11: Band Listen with Delta.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        s.editor->getGraph().setSelectedBand (1);
        setParam (s.processor, "bandListen", 2.0f);
        setParam (s.processor, "delta", 1.0f);
        s.play (voice, 0, bestFrame + 1);
        s.saveBoth (folder, "band-listen-delta");
    }

    // State 12: Depth at the top held by Max Cut, in High, with Max Cut hovered, which draws its line on the graph.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        setParam (s.processor, "depth", 24.0f);
        setParam (s.processor, "maxCut", 8.0f);
        setParam (s.processor, "quality", 2.0f);
        s.processor.prepareToPlay (kRate, kBlock);
        s.play (voice, 0, bestFrame + 1);
        s.editor->previewHover ("maxCut");
        s.saveBoth (folder, "max-cut-hovered");
    }

    // State 13: a free dot, and a dot hovered with the real mouse handlers, which shows how to take it away.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        s.play (voice, 0, bestFrame + 1);
        auto& graph = s.editor->getGraph();
        const float x = (float) graph.getWidth() * std::log (3200.0f / 20.0f) / std::log (1000.0f);
        const float y = (float) graph.getHeight() * (0.5f - 2.0f / 12.0f * 0.30f);
        const juce::Point<float> at (x, y);
        const auto now = juce::Time::getCurrentTime();
        graph.mouseMove (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &graph, &graph, now, at, now, 0, false));
        s.saveBoth (folder, "dot-hovered");
    }

    // State 14 and 15: surround layouts, closed and with Advanced open, on real speech with a different resonance in each pair.
    for (const auto& surround : { std::make_pair (juce::String ("surround-5-1"), juce::AudioChannelSet::create5point1()),
                                  std::make_pair (juce::String ("surround-7-1-4-advanced"), juce::AudioChannelSet::create7point1point4()) })
    {
        if (cpuOnly)
            break;

        SmoothResonanceProcessor processor;
        auto layout = processor.getBusesLayout();
        layout.inputBuses.getReference (0) = surround.second;
        layout.outputBuses.getReference (0) = surround.second;
        processor.setBusesLayout (layout);
        processor.setRateAndBufferSizeDetails (kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        setUpActive (processor);
        setParam (processor, "link", 40.0f);

        std::unique_ptr<ResonanceEditor> editor (dynamic_cast<ResonanceEditor*> (processor.createEditor()));
        editor->setVisible (true);

        if (surround.second.size() > 8)
            editor->setAdvancedOpen (true, false);

        const int channels = surround.second.size();
        std::vector<std::vector<float>> feeds;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto feed = voice;
            feed.resize (juce::jmin (feed.size(), (size_t) (bestFrame + 2) * kSamplesPerFrame));
            addResonance (feed, 1400.0 + 700.0 * ch, 7.0, 4.0 + 1.5 * ch);
            feeds.push_back (std::move (feed));
        }

        juce::AudioBuffer<float> buffer (channels, kBlock);
        juce::MidiBuffer midi;

        for (int frame = 0; frame <= bestFrame; ++frame)
        {
            for (int done = 0; done < kSamplesPerFrame; done += kBlock)
            {
                const int n = std::min (kBlock, kSamplesPerFrame - done);
                buffer.setSize (channels, n, false, false, true);

                for (int ch = 0; ch < channels; ++ch)
                    std::copy_n (feeds[(size_t) ch].data() + (size_t) frame * kSamplesPerFrame + (size_t) done, n, buffer.getWritePointer (ch));

                processor.processBlock (buffer, midi);
            }

            editor->tick (1.0 / 60.0);
        }

        for (float scale : { 1.0f, 2.0f })
        {
            auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
            auto file = folder.getChildFile (surround.first + (scale > 1.5f ? "@2x.png" : "@1x.png"));
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (image, out);
        }
    }

    // The smallest size the window can be.
    if (! cpuOnly)
    {
        Session s (SmoothResonanceProcessor::minWindowWidth);
        setUpActive (s.processor);
        s.play (voice, 0, bestFrame + 1);
        s.save (folder, "smallest", 1.0f);
        s.save (folder, "smallest", 2.0f);
    }

    // Motion check: six frames in a row, about 50 ms apart, stacked into one picture.
    if (! cpuOnly)
    {
        Session s;
        setUpActive (s.processor);
        const int start = juce::jmax (0, bestFrame - 9);
        s.play (voice, 0, start);

        constexpr int shots = 6;
        const auto graphBounds = s.editor->getGraph().getBounds();
        juce::Image sheet (juce::Image::RGB, graphBounds.getWidth(), graphBounds.getHeight() / 2 * shots / 2, true);
        juce::Graphics g (sheet);

        for (int i = 0; i < shots; ++i)
        {
            s.play (voice, start + i * 3, start + i * 3 + 3);
            auto frameImage = s.editor->getGraph().createComponentSnapshot (s.editor->getGraph().getLocalBounds(), true, 1.0f);
            const int w = sheet.getWidth() / 2, h = sheet.getHeight() / (shots / 2);
            g.drawImage (frameImage, juce::Rectangle<float> ((float) (i % 2) * (float) w, (float) (i / 2) * (float) h, (float) w, (float) h));
        }

        auto file = folder.getChildFile ("motion-six-frames.png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (sheet, out);
    }

    // CPU: how long one frame's work takes, at normal size and on a Retina-sized canvas, redrawing the whole graph.
    {
        const auto measure = [&] (float scale)
        {
            Session s;
            setUpActive (s.processor);
            s.play (voice, 0, 200);

            juce::Image canvas (juce::Image::RGB, juce::roundToInt ((float) s.editor->getGraph().getWidth() * scale),
                                juce::roundToInt ((float) s.editor->getGraph().getHeight() * scale), true);
            double spent = 0.0;
            constexpr int frames = 300;

            for (int frame = 200; frame < 200 + frames; ++frame)
            {
                s.play (voice, frame, frame + 1);    // audio is not timed, only the window's share below
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                s.editor->tick (1.0 / 60.0);
                juce::Graphics g (canvas);
                g.addTransform (juce::AffineTransform::scale (scale));
                s.editor->getGraph().paintEntireComponent (g, true);
                spent += juce::Time::getMillisecondCounterHiRes() - t0;
            }

            const double perFrame = spent / frames;
            std::printf ("window work per frame at %.0fx: %.2f ms, %.1f%% of one core at 60 frames a second\n", (double) scale, perFrame, perFrame * 60.0 / 10.0);
        };

        measure (1.0f);
        measure (2.0f);
    }

    return 0;
}
