#include "PluginProcessor.h"
#include "StateGuard.h"

namespace
{
    constexpr double kDryDelayMs = 5.5;    // longest sweep plus room for the interpolator
    constexpr double kMaxDepthMs = 5.0;
    constexpr double kLoopMinMs = 0.25;    // shortest feedback loop, reached at the zero crossing
    constexpr double kLoopLowHz = 12000.0; // head loss on each pass round the loop
    constexpr double kLoopHighHz = 20.0;   // stops DC building up in the loop, low enough not to pull the comb peaks off pitch
    constexpr double kToneHz = 4000.0;
    constexpr int kTick = 16;              // oversampled samples between LFO updates
    constexpr float kSatRef = 0.125f;      // -18 dBFS passes at the same level at every Saturation setting
    constexpr double kTwoPi = 6.283185307179586476925286766559;

    // Length of one LFO cycle in quarter notes
    constexpr double kDivisionBeats[] = { 32.0, 16.0, 8.0, 4.0, 3.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.25 };
    const char* const kDivisionNames[] = { "8 bars", "4 bars", "2 bars", "1 bar", "1/2 dotted", "1/2", "1/2 triplet", "1/4 dotted", "1/4", "1/4 triplet", "1/8 dotted", "1/8", "1/8 triplet", "1/16" };

    struct Preset { const char* name; float rate; bool sync; int division; float depth, feedback, saturation, tone, spread, mix; };
    constexpr Preset presets[] = {
        { "Slow Zero Crossing",  0.12f, false, 3, 3.0f,  20.0f, 15.0f, -1.0f,   0.0f, 50.0f },
        { "Wide Vocal Sweep",    0.20f, false, 3, 2.0f,  15.0f, 10.0f,  0.0f,  90.0f, 35.0f },
        { "Takeoff Roar",        0.08f, false, 3, 5.0f,  75.0f, 30.0f,  0.0f,  30.0f, 50.0f },
        { "Hollow Inverted",     0.15f, false, 3, 3.5f, -55.0f, 15.0f, -3.0f,   0.0f, 50.0f },
        { "One Bar Swirl",       0.25f, true,  3, 2.5f,  40.0f, 20.0f,  0.0f,  90.0f, 50.0f },
        { "Dark Tape Haze",      0.30f, false, 3, 1.5f,  35.0f, 60.0f, -8.0f, 120.0f, 40.0f },
        { "Quarter Note Wobble", 1.00f, true,  8, 1.0f, -30.0f, 25.0f,  0.0f, 180.0f, 40.0f },
    };

    double wrapTurns (double x) { return x - std::floor (x); }
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    // IDs are saved in sessions and automation: never rename them once installed.
    NormalisableRange<float> rateRange (0.01f, 10.0f, 0.001f);
    rateRange.setSkewForCentre (0.5f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "rate", 1 }, "Rate", rateRange, 0.25f, AudioParameterFloatAttributes().withLabel ("Hz")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "sync", 1 }, "Tempo Sync", false));
    StringArray names;
    for (auto* n : kDivisionNames)
        names.add (n);
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "division", 1 }, "Sync Division", names, 3));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "depth", 1 }, "Depth", NormalisableRange<float> (0.0f, (float) kMaxDepthMs, 0.01f), 2.5f, AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "feedback", 1 }, "Feedback", NormalisableRange<float> (-95.0f, 95.0f, 0.1f), 30.0f, AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "saturation", 1 }, "Saturation", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 20.0f, AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tone", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "spread", 1 }, "Spread", NormalisableRange<float> (0.0f, 180.0f, 1.0f), 45.0f, AudioParameterFloatAttributes().withLabel ("deg")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    rateHz = apvts.getRawParameterValue ("rate");
    depthMs = apvts.getRawParameterValue ("depth");
    feedbackPct = apvts.getRawParameterValue ("feedback");
    saturationPct = apvts.getRawParameterValue ("saturation");
    toneDb = apvts.getRawParameterValue ("tone");
    spreadDeg = apvts.getRawParameterValue ("spread");
    mixPct = apvts.getRawParameterValue ("mix");
    sync = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("sync"));
    division = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("division"));
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    configure (48000.0, 512); // so a host that asks for the latency before preparing gets a true figure
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PluginProcessor::configure (double sampleRate, int samplesPerBlock)
{
    hostRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock = juce::jmax (samplesPerBlock, 32);

    // The core runs near 192 kHz at every host rate, which keeps saturation products and interpolation loss out of the audio band.
    const int stages = hostRate > 120000.0 ? 0 : (hostRate > 60000.0 ? 1 : 2);
    osFactor = 1 << stages;
    osRate = hostRate * osFactor;
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, (size_t) stages, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
    oversampler->initProcessing ((size_t) maxBlock);
    oversampler->reset();

    // The fixed delay is padded so the total is a whole number of host samples at every rate.
    const double osLatency = (double) oversampler->getLatencyInSamples();
    const int total = (int) std::ceil (osLatency + kDryDelayMs * 0.001 * hostRate);
    dryDelay = (int) std::lround (((double) total - osLatency) * osFactor);
    setLatencySamples (total);

    const int needed = (int) std::ceil ((kDryDelayMs + kMaxDepthMs + 1.0) * 0.001 * osRate) + 16;
    const int size = juce::nextPowerOfTwo (needed);
    mask = size - 1;
    for (auto& c : channels)
    {
        c.line.assign ((size_t) size, 0.0f);
        c.dry.assign ((size_t) size, 0.0f);
    }

    const auto tickCoeff = [this] (double seconds) { return (float) (1.0 - std::exp (-(double) kTick / (seconds * osRate))); };
    const auto sampleCoeff = [this] (double seconds) { return (float) (1.0 - std::exp (-1.0 / (seconds * osRate))); };
    slowTickDepth = tickCoeff (0.06);
    slowTickSpread = tickCoeff (0.08);
    slowTickFast = tickCoeff (0.02);
    sampleCoeffFeedback = sampleCoeff (0.03);
    sampleCoeffFast = sampleCoeff (0.015);
    correctionTick = 1.0 - std::exp (-(double) kTick / (0.05 * osRate));
    loopLowCoeff = (float) (1.0 - std::exp (-kTwoPi * kLoopLowHz / osRate));
    loopHighCoeff = (float) (1.0 - std::exp (-kTwoPi * kLoopHighHz / osRate));
    loopLowDelay = (1.0 - (double) loopLowCoeff) / (double) loopLowCoeff;
    loopMinSamples = kLoopMinMs * 0.001 * osRate;

    snapToParameters();
}

void PluginProcessor::snapToParameters()
{
    readTargets (false);
    depthNow = depthTarget;
    spreadNow = spreadTarget;
    toneNow = toneTarget;
    satNow = satTarget;
    feedbackNow = feedbackTarget;
    polarityNow = polarityTarget;
    mixNow = mixTarget;
    onNow = onTarget;
    toneDone = 1000.0f;
    correction = 0.0;
    phase = 0.0;
    writeIndex = 0;
    tickCountdown = 0;
    firstBlock = true;
    for (size_t ch = 0; ch < channels.size(); ++ch)
    {
        auto& c = channels[ch];
        std::fill (c.line.begin(), c.line.end(), 0.0f);
        std::fill (c.dry.begin(), c.dry.end(), 0.0f);
        c.tone = {};
        c.loopLow = c.loopHigh = 0.0f;
        c.rel = (double) depthNow * 0.001 * osRate * std::sin (kTwoPi * (ch == 0 ? 0.0 : (double) spreadNow));
        c.relStep = 0.0;
        c.loop = std::sqrt (c.rel * c.rel + loopMinSamples * loopMinSamples) - loopLowDelay;
        c.loopStep = 0.0;
    }
}

void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    configure (sampleRate, samplesPerBlock);
}

void PluginProcessor::readTargets (bool forceBypass)
{
    depthTarget = juce::jlimit (0.0f, (float) kMaxDepthMs, depthMs->load());
    spreadTarget = juce::jlimit (0.0f, 180.0f, spreadDeg->load()) / 360.0f; // in turns
    toneTarget = juce::jlimit (-12.0f, 12.0f, toneDb->load());
    satTarget = juce::jlimit (0.0f, 100.0f, saturationPct->load()) * 0.01f;
    const float fb = juce::jlimit (-95.0f, 95.0f, feedbackPct->load()) * 0.01f;
    feedbackTarget = fb;
    polarityTarget = fb < 0.0f ? -1.0f : 1.0f; // negative feedback flips the wet path, which keeps the comb deep on that side too
    mixTarget = juce::jlimit (0.0f, 100.0f, mixPct->load()) * 0.01f;
    onTarget = (forceBypass || bypass->get()) ? 0.0f : 1.0f;
}

void PluginProcessor::followHostTransport()
{
    double bpm = 120.0, ppq = 0.0;
    bool playing = false, havePpq = false;
    if (auto* head = getPlayHead())
        if (auto pos = head->getPosition())
        {
            if (auto b = pos->getBpm())
                if (std::isfinite (*b) && *b > 0.0)
                    bpm = juce::jlimit (20.0, 999.0, *b);
            playing = pos->getIsPlaying();
            if (auto q = pos->getPpqPosition())
                if (std::isfinite (*q))
                {
                    ppq = *q;
                    havePpq = true;
                }
        }

    if (! sync->get())
    {
        increment = (double) rateHz->load() / osRate;
        correction = 0.0;
        firstBlock = false;
        return;
    }

    const double beats = kDivisionBeats[juce::jlimit (0, (int) std::size (kDivisionBeats) - 1, division->getIndex())];
    increment = bpm / (60.0 * beats * osRate);

    if (playing && havePpq)
    {
        // Lock to the bar. A transport jump is slewed so the delay never jumps.
        const double target = wrapTurns (ppq / beats);
        if (firstBlock)
        {
            phase = target;
            tickCountdown = 0;
            for (size_t ch = 0; ch < channels.size(); ++ch)
            {
                auto& c = channels[ch];
                c.rel = (double) depthNow * 0.001 * osRate * std::sin (kTwoPi * (phase + (ch == 0 ? 0.0 : (double) spreadNow)));
                c.loop = std::sqrt (c.rel * c.rel + loopMinSamples * loopMinSamples) - loopLowDelay;
            }
            correction = 0.0;
        }
        else
        {
            const double now = phase - increment * tickCountdown; // phase holds the value at the end of the current tick
            double error = wrapTurns (target - now);
            if (error > 0.5)
                error -= 1.0;
            correction = error;
        }
    }
    else
    {
        correction = 0.0;
    }
    firstBlock = false;
}

void PluginProcessor::controlTick() noexcept
{
    using namespace flanger;
    smoothTo (depthNow, depthTarget, slowTickDepth);
    smoothTo (spreadNow, spreadTarget, slowTickSpread);
    smoothTo (toneNow, toneTarget, slowTickFast);
    smoothTo (satNow, satTarget, slowTickFast);

    if (std::abs (toneNow - toneDone) > 1.0e-4f)
    {
        shelf.set (osRate, kToneHz, (double) toneNow);
        toneDone = toneNow;
    }

    driveNow = 1.0f + 5.0f * satNow;
    makeup = kSatRef / tapeSat (kSatRef, satNow, driveNow); // outside the loop, so it never raises the loop gain
    inputScale = std::sqrt (juce::jmax (0.0f, 1.0f - feedbackNow * feedbackNow)); // holds broadband level as Feedback rises

    double step = increment * kTick;
    if (correction != 0.0)
    {
        const double d = correction * correctionTick;
        correction -= d;
        step += d;
    }
    phase = wrapTurns (phase + step);

    const double depthSamples = (double) depthNow * 0.001 * osRate;
    for (int ch = 0; ch < numActive; ++ch)
    {
        auto& c = channels[(size_t) ch];
        const double rel = depthSamples * std::sin (kTwoPi * (phase + (ch == 0 ? 0.0 : (double) spreadNow)));
        // The loop length follows the distance from the dry path, rounded off near zero so its pitch never kinks.
        const double loop = std::sqrt (rel * rel + loopMinSamples * loopMinSamples) - loopLowDelay;
        c.relStep = (rel - c.rel) / kTick;
        c.loopStep = (loop - c.loop) / kTick;
    }
}

void PluginProcessor::renderOversampled (juce::dsp::AudioBlock<float>& block) noexcept
{
    using namespace flanger;
    const int n = (int) block.getNumSamples();
    const int numCh = juce::jmin ((int) block.getNumChannels(), numActive);
    float* samples[2] = { block.getChannelPointer (0), numCh > 1 ? block.getChannelPointer (1) : nullptr };

    for (int i = 0; i < n; ++i)
    {
        if (tickCountdown <= 0)
        {
            controlTick();
            tickCountdown = kTick;
        }
        --tickCountdown;

        smoothTo (feedbackNow, feedbackTarget, sampleCoeffFeedback);
        smoothTo (polarityNow, polarityTarget, sampleCoeffFast);
        smoothTo (mixNow, mixTarget, sampleCoeffFast);
        smoothTo (onNow, onTarget, sampleCoeffFast);
        const float m = mixNow * onNow;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& c = channels[(size_t) ch];
            const float in = samples[ch][i];

            const float dry = c.dry[(size_t) ((writeIndex - dryDelay) & mask)];
            c.dry[(size_t) writeIndex] = in;

            const float back = readCubic (c.line.data(), mask, writeIndex, c.loop);
            c.loopLow += loopLowCoeff * (back - c.loopLow);
            c.loopHigh += loopHighCoeff * (c.loopLow - c.loopHigh);
            const float toLine = inputScale * in + limitFeedback (feedbackNow * (c.loopLow - c.loopHigh));
            c.line[(size_t) writeIndex] = tapeSat (toLine, satNow, driveNow);

            const float wet = c.tone.process (shelf, makeup * polarityNow * readCubic (c.line.data(), mask, writeIndex, (double) dryDelay + c.rel));
            samples[ch][i] = (1.0f - m) * dry + m * wet;

            c.rel += c.relStep;
            c.loop += c.loopStep;
        }
        writeIndex = (writeIndex + 1) & mask;
    }
}

void PluginProcessor::run (juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    const int numCh = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels(), 2);
    const int total = buffer.getNumSamples();
    if (numCh <= 0 || total <= 0)
        return;
    numActive = numCh;

    readTargets (forceBypass);
    followHostTransport();

    for (int pos = 0; pos < total;)
    {
        const int len = juce::jmin (maxBlock, total - pos);
        juce::dsp::AudioBlock<float> host (buffer.getArrayOfWritePointers(), (size_t) numCh, (size_t) pos, (size_t) len);
        auto up = oversampler->processSamplesUp (host);
        renderOversampled (up);
        oversampler->processSamplesDown (host);
        pos += len;
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    run (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    run (buffer, true); // same path and same delay, with the wet faded out
}

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
    const auto set = [this] (const char* id, float v)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    const auto& p = presets[i];
    set ("rate", p.rate);
    set ("sync", p.sync ? 1.0f : 0.0f);
    set ("division", (float) p.division);
    set ("depth", p.depth);
    set ("feedback", p.feedback);
    set ("saturation", p.saturation);
    set ("tone", p.tone);
    set ("spread", p.spread);
    set ("mix", p.mix);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            applySafeState (apvts, *xml);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
