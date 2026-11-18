// Renders the real window to PNG with real speech running through the real processor, with no host. Also measures its CPU.
// Run from the repo root: build/RenderWindow_artefacts/Release/RenderWindow [output folder]
//                         build/RenderWindow_artefacts/Release/RenderWindow --live [runs]
// Sung material is macOS `say` Good News voice, spoken material is `say` Samantha. Both real recordings of `say`, neither is a singer.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "editor/HarmonyEditor.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <libproc.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/resource.h>
#include <thread>
#include <vector>

static constexpr double kRate = 48000.0;
static constexpr int kBlock = 256;
static constexpr int kSamplesPerFrame = 2000;   // one display frame at 24 a second

static std::vector<float> loadCorpus (const char* name)
{
    juce::AudioFormatManager m;
    m.registerBasicFormats();
    juce::File f (juce::String (SMOOTH_HARMONY_CORPUS) + "/" + name + ".wav");
    std::unique_ptr<juce::AudioFormatReader> reader (m.createReaderFor (f));
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

// The real key file is never read here: a render that follows a key gets its own file (1.6.0).
static PluginProcessor::Options optionsFor (const juce::String& keyFile)
{
    PluginProcessor::Options o;
    o.keyFilePath = keyFile.isNotEmpty() ? keyFile : juce::String ("/nonexistent/choir-loft-render-key.json");
    return o;
}

struct Session
{
    PluginProcessor processor;
    std::unique_ptr<HarmonyEditor> editor;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;

    explicit Session (int width = 0, bool advanced = false, const juce::String& keyFile = {})
        : processor (optionsFor (keyFile))
    {
        if (width > 0)
            processor.windowWidth.store (width);

        processor.advancedOpen.store (advanced);
        processor.setPlayConfigDetails (2, 2, kRate, kBlock);
        processor.prepareToPlay (kRate, kBlock);
        editor.reset (dynamic_cast<HarmonyEditor*> (processor.createEditor()));
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
                midi.clear();
            }

            editor->tick (1.0 / 24.0);
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

static double threadSeconds (thread_t thread)
{
    thread_basic_info_data_t info {};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info (thread, THREAD_BASIC_INFO, (thread_info_t) &info, &count) != KERN_SUCCESS)
        return 0.0;
    return (double) info.user_time.seconds + (double) info.user_time.microseconds * 1.0e-6
         + (double) info.system_time.seconds + (double) info.system_time.microseconds * 1.0e-6;
}

struct ProcessUsage { uint64_t instructions = 0, fast = 0, total = 0; };

static ProcessUsage processUsage()
{
    rusage_info_v6 info {};
    ProcessUsage out;
    if (proc_pid_rusage (getpid(), RUSAGE_INFO_V6, (rusage_info_t*) &info) == 0)
    {
        out.instructions = info.ri_instructions;
        out.fast = info.ri_user_ptime + info.ri_system_ptime;
        out.total = info.ri_user_time + info.ri_system_time;
    }
    return out;
}

/** The real thing: a real window with all but one column of pixels off the edge of the screen, painted by the system at the screen's real pixel density, with sung
    material running in real time on another thread. The message thread is timed alone, in the same run as the audio thread, so nothing is subtracted. */
static int runLive (int runs)
{
    auto voice = loadCorpus ("sung_goodnews");

    for (int run = 0; run < runs; ++run)
    {
        Session s;
        s.processor.setCurrentProgram (12);   // Eight Part Choir: eight voices, the most to draw

        std::atomic<bool> running { true };
        std::atomic<thread_t> audioThread { 0 };
        std::thread audio ([&]
        {
            audioThread = pthread_mach_thread_np (pthread_self());
            juce::AudioBuffer<float> buffer (2, kBlock);
            juce::MidiBuffer midi;
            size_t at = 0;
            auto next = std::chrono::steady_clock::now();

            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < kBlock; ++i)
                        buffer.setSample (ch, i, voice[(at + (size_t) i) % voice.size()]);

                at += kBlock;
                s.processor.processBlock (buffer, midi);
                next += std::chrono::microseconds ((long long) (1.0e6 * kBlock / kRate));
                std::this_thread::sleep_until (next);
            }
        });

        s.editor->setVisible (false);
        s.editor->setTopLeftPosition (1 - s.editor->getWidth(), 300);   // one column of pixels on screen, or macOS skips painting it
        s.editor->addToDesktop (juce::ComponentPeer::windowIgnoresKeyPresses | juce::ComponentPeer::windowIgnoresMouseClicks);
        s.editor->setVisible (true);

        juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);

        constexpr int seconds = 8;
        const auto mainThread = pthread_mach_thread_np (pthread_self());
        const int paintsBefore = s.editor->getStage().getPaintCount();
        const double messageBefore = threadSeconds (mainThread), audioBefore = threadSeconds (audioThread.load());
        const auto usageBefore = processUsage();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);
        const double message = (threadSeconds (mainThread) - messageBefore) / seconds;
        const double audioCpu = (threadSeconds (audioThread.load()) - audioBefore) / seconds;
        const auto usageAfter = processUsage();
        const int paints = s.editor->getStage().getPaintCount() - paintsBefore;

        auto* peer = s.editor->getPeer();
        const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const double total = (double) (usageAfter.total - usageBefore.total);
        const double fastShare = total > 0.0 ? (double) (usageAfter.fast - usageBefore.fast) / total : 0.0;
        const double framesPerSecond = (double) paints / seconds;
        const double instructionsPerFrame = paints > 0 ? (double) (usageAfter.instructions - usageBefore.instructions) / paints : 0.0;

        std::printf ("run %d: window %.1f%% of one core, audio %.1f%%, %.1f frames a second, screen %.0fx, peer %s, fast core share %.0f%%, %.1f million instructions a frame (audio included)\n",
                     run + 1, message * 100.0, audioCpu * 100.0, framesPerSecond, display != nullptr ? display->scale : 0.0,
                     peer != nullptr ? "open" : "none", fastShare * 100.0, instructionsPerFrame * 1.0e-6);
        std::fflush (stdout);

        s.editor->removeFromDesktop();
        running = false;
        audio.join();
    }

    // Open with nothing playing: it should draw nothing at all.
    {
        Session s;
        s.editor->setVisible (false);
        s.editor->setTopLeftPosition (1 - s.editor->getWidth(), 300);
        s.editor->addToDesktop (juce::ComponentPeer::windowIgnoresKeyPresses | juce::ComponentPeer::windowIgnoresMouseClicks);
        s.editor->setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1500);

        constexpr int seconds = 5;
        const auto mainThread = pthread_mach_thread_np (pthread_self());
        const int paintsBefore = s.editor->getStage().getPaintCount();
        const double before = threadSeconds (mainThread);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (seconds * 1000);
        std::printf ("nothing playing: window %.2f%% of one core, %d paints in %d seconds\n", (threadSeconds (mainThread) - before) / seconds * 100.0,
                     s.editor->getStage().getPaintCount() - paintsBefore, seconds);
        s.editor->removeFromDesktop();
    }

    return 0;
}

// ---- pictures -------------------------------------------------------------------------------------------------------

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc > 1 && juce::String (argv[1]) == "--live")
        return runLive (argc > 2 ? juce::String (argv[2]).getIntValue() : 3);

    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/window");
    folder.createDirectory();

    const auto sung = loadCorpus ("sung_goodnews");
    const auto speech = loadCorpus ("speech_samantha");

    if (sung.empty() || speech.empty())
    {
        std::printf ("FAILED could not read the recordings in tests/corpus\n");
        return 1;
    }

    const int sungEnd = juce::jmin (205, (int) (sung.size() / kSamplesPerFrame) - 1);
    const int speechEnd = juce::jmin (150, (int) (speech.size() / kSamplesPerFrame) - 1);

    // Idle: nothing has played. Singers show where they would sit.
    {
        Session s;
        s.editor->tick (1.0 / 30.0);
        s.saveBoth (folder, "idle");
    }

    // Active: two voices on sung material, the default sound.
    {
        Session s;
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "active");
    }

    // Follow Key on (1.6.0): the Mode row says whose key it is and how old.
    {
        const auto keyFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("choir-loft-render-key.json");
        keyFile.replaceWithText ("{\"key\":\"D\",\"scale\":\"minor\",\"confidence\":0.90,\"time\":"
                                 + juce::String ((double) std::time (nullptr) - 130.0, 1) + ",\"listening\":true}");
        Session s (0, false, keyFile.getFullPathName());
        s.processor.apvts.getParameter ("followKey")->setValueNotifyingHost (1.0f);
        s.processor.pollKeyFileNow();
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "follow-key");
        keyFile.deleteFile();
    }

    // Four voices, each somewhere else on the stage.
    {
        Session s;
        s.processor.setCurrentProgram (3);   // Full Stack
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "four-voices");
    }

    // Eight voices, the most there can be.
    {
        Session s;
        s.processor.setCurrentProgram (12);   // Eight Part Choir
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "eight-voices");
    }

    // Choosing who to hear: part way through, three singers and the lead go off. Their lines and singers dim.
    {
        Session s;
        s.processor.setCurrentProgram (12);
        s.play (sung, 0, sungEnd - 60);
        for (const char* id : { "voice2On", "voice3On", "voice5On", "voice7On", "voice8On", "leadOn" })
            setParam (s.processor, id, 0.0f);
        s.play (sung, sungEnd - 60, sungEnd);
        s.saveBoth (folder, "some-off");
    }

    // Four voices with two off, on the wider cards.
    {
        Session s;
        s.processor.setCurrentProgram (3);
        setParam (s.processor, "voice2On", 0.0f);
        setParam (s.processor, "voice4On", 0.0f);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "four-two-off");
    }

    // Six voices: the cards in between.
    {
        Session s;
        s.processor.setCurrentProgram (11);   // Six Part Stack
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "six-voices");
    }

    // Eight voices with Advanced open.
    {
        Session s (0, true);
        s.processor.setCurrentProgram (12);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "eight-advanced");
    }

    // Spoken material, where the lines break up and wander.
    {
        Session s;
        s.processor.setCurrentProgram (3);
        s.play (speech, 0, speechEnd);
        s.saveBoth (folder, "speech");
    }

    // MIDI Chord, with a held chord, and a hovered strip.
    {
        Session s;
        s.processor.setCurrentProgram (7);   // MIDI Choir
        s.midi.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100), 0);
        s.midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        s.midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
        s.midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 0);
        s.play (sung, 0, sungEnd);
        s.editor->previewHover ("voice2Pan");
        s.saveBoth (folder, "midi-chord");
    }

    // Chord Tones, idle, with one voice off and one voice's level off.
    {
        Session s;
        s.processor.setCurrentProgram (9);   // Chord Tones Pad
        setParam (s.processor, "chordRoot", 9.0f);
        setParam (s.processor, "chordType", 1.0f);
        setParam (s.processor, "voice2Level", -60.0f);
        for (int i = 0; i < 60; ++i)
            s.editor->tick (1.0 / 30.0);
        s.saveBoth (folder, "chord-tones");
    }

    // Advanced open, active, with a voice being typed.
    {
        Session s (0, true);
        s.processor.setCurrentProgram (3);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "advanced-open");
    }

    // Human turned up, with the mouse over it so the line under the controls says what it does.
    {
        Session s;
        setParam (s.processor, "human", 65.0f);
        s.play (sung, 0, sungEnd);
        s.editor->previewHover ("human");
        s.saveBoth (folder, "human");
    }

    // 1.5.2: a loud lead on Full Stack with the Ceiling on. The output light reads the ceiling and says it is near full scale.
    {
        Session s;
        s.processor.setCurrentProgram (3);
        setParam (s.processor, "ceiling", 1.0f);
        auto loud = sung;
        float top = 0.0f;
        for (float v : loud) top = std::max (top, std::abs (v));
        for (auto& v : loud) v *= 0.89f / std::max (top, 1.0e-6f);   // peaks at -1 dBFS
        s.play (loud, 0, sungEnd);
        s.editor->previewHover ("ceiling");
        s.saveBoth (folder, "ceiling");
    }

    // Bypassed.
    {
        Session s;
        s.processor.setCurrentProgram (0);
        setParam (s.processor, "bypass", 1.0f);
        s.play (sung, 0, sungEnd);
        s.saveBoth (folder, "bypassed");
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
