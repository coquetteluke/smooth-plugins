#include "PluginProcessor.h"
#include "KeyEditor.h"
#include "StateGuard.h"
#include <chrono>

namespace
{
    struct Preset { const char* name; int response; int source; int mode; };
    // response: 0 Fast, 1 Stable. source: 0 Full Mix, 1 Single Voice. mode: 0 Listen, 1 Manual.
    constexpr Preset presets[] = {
        { "Whole Song", 1, 0, 0 },
        { "Quick Check", 0, 0, 0 },
        { "Vocal Track", 1, 1, 0 },
        { "Vocal Quick Check", 0, 1, 0 },
        { "Set Key By Hand", 1, 0, 1 },
    };

    double unixNow() noexcept
    {
        using namespace std::chrono;
        return duration<double> (system_clock::now().time_since_epoch()).count();
    }
    double steadyNow() noexcept
    {
        using namespace std::chrono;
        return duration<double> (steady_clock::now().time_since_epoch()).count();
    }

    juce::String keyText (float v)
    {
        const int i = juce::roundToInt (v);
        return i >= 0 && i < 12 ? juce::String (smoothkey::keyNames[i]) : juce::String ("None");
    }
    juce::String scaleText (float v) { return v >= 0.5f ? "Minor" : "Major"; }
    juce::String percentText (float v) { return juce::String (v, 0); }
    juce::String hertzText (float v) { return juce::String (v, 1); }
    juce::String bpmText (float v) { return v < 1.0f ? juce::String ("None") : juce::String (v, 1); }

    const juce::Identifier windowWidthAttribute { "windowWidth" }, advancedOpenAttribute { "advancedOpen" };

    int relativeKey (int key, bool minor) noexcept { return minor ? (key + 3) % 12 : (key + 9) % 12; }
}

// The one background thread: drains the queue, runs the analyser, publishes readings and writes the shared file.
class PluginProcessor::Worker final : public juce::Thread
{
public:
    Worker (PluginProcessor& p, const juce::String& jsonPath, double minWriteSeconds) : juce::Thread ("Smooth Key analysis"), owner (p)
    {
        char path[PATH_MAX] = {};
        if (jsonPath.isNotEmpty())
            std::snprintf (path, sizeof path, "%s", jsonPath.toRawUTF8());
        else if (! smoothkey::KeyFile::defaultPath (path, sizeof path))
            path[0] = 0;
        file.setPath (path, minWriteSeconds);
    }

    void run() override
    {
        double lastAudio = steadyNow(), lastTick = lastAudio;
        bool resetWasOn = false;
        while (! threadShouldExit())
        {
            const double rate = owner.pendingRate.exchange (0.0, std::memory_order_relaxed);
            if (rate > 0.0)
            {
                if (rate != currentRate) { analyser.configure (rate); currentRate = rate; }
                owner.queue.discard();
            }

            const bool resetOn = owner.resetParam->load (std::memory_order_relaxed) >= 0.5f;
            const bool asked = owner.resetRequest.exchange (false, std::memory_order_relaxed);
            if (asked || (resetOn && ! resetWasOn))
                analyser.reset();
            resetWasOn = resetOn;

            analyser.setResponse (owner.response->load (std::memory_order_relaxed) >= 0.5f ? smoothkey::Response::stable : smoothkey::Response::fast);
            analyser.setSource (owner.source->load (std::memory_order_relaxed) >= 0.5f ? smoothkey::Source::voice : smoothkey::Source::mix);

            bool gotAudio = false;
            for (int i = 0; i < 64; ++i)
            {
                const int n = owner.queue.pop (buffer, (int) std::size (buffer));
                if (n == 0) break;
                analyser.push (buffer, n);
                gotAudio = true;
                if (threadShouldExit()) break;
            }

            const double now = steadyNow();
            if (gotAudio) lastAudio = now;
            // A host may stop calling the plug-in when playback stops. Treat that as silence so the confidence falls.
            else if (owner.isListeningNow() && now - lastAudio > 0.5)
                analyser.idle (now - lastTick);
            lastTick = now;

            owner.workerPublish (analyser.reading());
            writeFile (now);
            wait (25);
        }

        // The last word as the plug-in closes: nothing is listening here any more. Only if it has ever had something to say.
        if (file.everWritten())
        {
            auto last = file.lastWritten();
            last.listening = false;
            file.flush (last, unixNow());
        }
    }

private:
    // False when there is no key to report yet.
    bool currentState (smoothkey::KeyState& s) const noexcept
    {
        const auto d = owner.getDisplay();
        if (d.key < 0) return false;
        s.key = d.key;
        s.minor = d.minor;
        s.confidence = d.confidence;
        s.listening = d.active;
        return true;
    }

    void writeFile (double now) noexcept
    {
        smoothkey::KeyState s;
        if (! currentState (s))
        {
            // After Reset the file format has no way to say no key, so the last key stays with confidence 0.
            if (! file.everWritten()) return;
            s = file.lastWritten();
            s.confidence = 0.0;
            s.listening = owner.isListeningNow();
        }
        file.update (s, now, unixNow());
        owner.fileFailed.store (file.hasFailed(), std::memory_order_relaxed);
        owner.fileWrites.store (file.writeCount(), std::memory_order_relaxed);
    }

    PluginProcessor& owner;
    smoothkey::KeyAnalyser analyser;
    smoothkey::KeyFile file;
    double currentRate = 0.0;
    float buffer[4096] {};
};

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    StringArray keys;
    for (auto* k : smoothkey::keyNames) keys.add (k);
    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "listening", 1 }, "Listening", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "reset", 1 }, "Reset", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "response", 1 }, "Response", StringArray { "Fast", "Stable" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Source", StringArray { "Full Mix", "Single Voice" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", StringArray { "Listen", "Manual" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "manualKey", 1 }, "Manual Key", keys, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "manualScale", 1 }, "Manual Scale", StringArray { "Major", "Minor" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "swapRelative", 1 }, "Relative Swap", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "tempoScale", 1 }, "Tempo Scale", StringArray { "As Detected", "Half", "Double" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor (const juce::String& jsonPath, double minWriteSeconds)
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    listening = apvts.getRawParameterValue ("listening");
    resetParam = apvts.getRawParameterValue ("reset");
    response = apvts.getRawParameterValue ("response");
    source = apvts.getRawParameterValue ("source");
    mode = apvts.getRawParameterValue ("mode");
    manualKey = apvts.getRawParameterValue ("manualKey");
    manualScale = apvts.getRawParameterValue ("manualScale");
    swapRelative = apvts.getRawParameterValue ("swapRelative");
    tempoScale = apvts.getRawParameterValue ("tempoScale");
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    addParameter (keyReading = new ReadingParameter ("detectedKey", "Detected Key", "", -1.0f, 11.0f, -1.0f, keyText));
    addParameter (scaleReading = new ReadingParameter ("detectedScale", "Detected Scale", "", 0.0f, 1.0f, 0.0f, scaleText));
    addParameter (confidenceReading = new ReadingParameter ("confidence", "Confidence", "%", 0.0f, 100.0f, 0.0f, percentText));
    addParameter (referenceReading = new ReadingParameter ("referencePitch", "Reference Pitch", "Hz", 415.0f, 466.0f, 440.0f, hertzText));
    addParameter (tempoReading = new ReadingParameter ("detectedTempo", "Detected Tempo", "BPM", 0.0f, 480.0f, 0.0f, bpmText));

    for (auto& c : chroma) c.store (0.0f, std::memory_order_relaxed);
    for (auto& c : candidates) c.store (0.0f, std::memory_order_relaxed);
    for (auto& n : liveNotes) n.store (0.0f, std::memory_order_relaxed);

    worker = std::make_unique<Worker> (*this, jsonPath, minWriteSeconds);
    worker->startThread (juce::Thread::Priority::low);
}

PluginProcessor::~PluginProcessor()
{
    worker->stopThread (4000);
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

bool PluginProcessor::isManual() const noexcept { return mode->load (std::memory_order_relaxed) >= 0.5f; }

bool PluginProcessor::isListeningNow() const noexcept
{
    return listening->load (std::memory_order_relaxed) >= 0.5f && ! isManual() && ! bypass->get();
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    pendingRate.store (sampleRate, std::memory_order_relaxed);
    setLatencySamples (0); // nothing is delayed at any rate, bypassed or not
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // The audio is never written to, so it leaves exactly as it came in. Only the private copy for analysis is cleaned.
    if (! isListeningNow())
        return;

    const int numCh = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    if (numCh <= 0)
        return;
    const float gain = 1.0f / (float) numCh;
    float mono[256];
    for (int start = 0; start < buffer.getNumSamples();)
    {
        const int n = juce::jmin ((int) std::size (mono), buffer.getNumSamples() - start);
        for (int i = 0; i < n; ++i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float x = buffer.getReadPointer (ch)[start + i];
                sum += std::isfinite (x) ? juce::jlimit (-16.0f, 16.0f, x) : 0.0f;
            }
            mono[i] = sum * gain;
        }
        queue.push (mono, n);
        start += n;
    }
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    // Audio passes as it is, and a bypassed plug-in does not listen.
}

void PluginProcessor::workerPublish (const smoothkey::Reading& r) noexcept
{
    int key = r.key;
    bool minor = r.minor;
    float confidence = r.confidence;

    if (isManual())
    {
        key = juce::jlimit (0, 11, juce::roundToInt (manualKey->load (std::memory_order_relaxed)));
        minor = manualScale->load (std::memory_order_relaxed) >= 0.5f;
        confidence = 1.0f;
    }
    else if (key >= 0 && swapRelative->load (std::memory_order_relaxed) >= 0.5f)
    {
        key = relativeKey (key, minor);
        minor = ! minor;
    }

    packedReading.store ((key + 1) | (minor ? 1 << 8 : 0) | (r.signal ? 1 << 9 : 0), std::memory_order_relaxed);
    shownConfidence.store (confidence, std::memory_order_relaxed);
    const float hz = 440.0f * std::pow (2.0f, r.tuningCents / 1200.0f);
    referenceHz.store (hz, std::memory_order_relaxed);
    detectedBpm.store (r.tempoBpm, std::memory_order_relaxed);
    for (size_t i = 0; i < 12; ++i)
    {
        chroma[i].store (r.chroma[i], std::memory_order_relaxed);
        liveNotes[i].store (r.liveNotes[i], std::memory_order_relaxed);
    }
    for (size_t k = 0; k < 24; ++k)
        candidates[k].store (r.candidates[k], std::memory_order_relaxed);

    keyReading->set ((float) key);
    scaleReading->set (minor ? 1.0f : 0.0f);
    confidenceReading->set (confidence * 100.0f);
    referenceReading->set (hz);
    tempoReading->set (getDisplay().shownBpm);
}

PluginProcessor::Display PluginProcessor::getDisplay() const noexcept
{
    Display d;
    const int packed = packedReading.load (std::memory_order_relaxed);
    d.key = (packed & 0xff) - 1;
    d.minor = (packed & (1 << 8)) != 0;
    d.signal = (packed & (1 << 9)) != 0;
    d.confidence = shownConfidence.load (std::memory_order_relaxed);
    d.manual = isManual();
    d.active = isListeningNow();
    d.referenceHz = referenceHz.load (std::memory_order_relaxed);
    d.detectedBpm = detectedBpm.load (std::memory_order_relaxed);
    const int scale = juce::roundToInt (tempoScale->load (std::memory_order_relaxed));
    d.shownBpm = d.detectedBpm * (scale == 1 ? 0.5f : (scale == 2 ? 2.0f : 1.0f));
    for (size_t i = 0; i < 12; ++i)
    {
        d.chroma[i] = chroma[i].load (std::memory_order_relaxed);
        d.liveNotes[i] = liveNotes[i].load (std::memory_order_relaxed);
    }
    for (size_t k = 0; k < 24; ++k)
        d.candidates[k] = candidates[k].load (std::memory_order_relaxed);
    return d;
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new KeyEditor (*this); }

int PluginProcessor::getNumPrograms() { return (int) std::size (presets); }

const juce::String PluginProcessor::getProgramName (int i)
{
    return juce::isPositiveAndBelow (i, getNumPrograms()) ? presets[i].name : "";
}

void PluginProcessor::setCurrentProgram (int i)
{
    if (! juce::isPositiveAndBelow (i, getNumPrograms()))
        return;
    currentProgram = i;
    const auto set = [this] (const char* id, int v)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 ((float) v));
    };
    set ("response", presets[i].response);
    set ("source", presets[i].source);
    set ("mode", presets[i].mode);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (advancedOpenAttribute, advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            if (xml->hasAttribute (windowWidthAttribute))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute (windowWidthAttribute, defaultWindowWidth)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute (advancedOpenAttribute, false), std::memory_order_relaxed);
            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (advancedOpenAttribute);
            applySafeState (apvts, *xml);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
