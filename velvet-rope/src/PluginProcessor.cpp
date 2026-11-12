#include "PluginProcessor.h"

#include "StateGuard.h"
#include "editor/ResonanceEditor.h"

#include <array>

namespace
{
    const juce::Identifier windowWidthAttribute { "windowWidth" };
    const juce::Identifier advancedOpenAttribute { "advancedOpen" };

    // These IDs are saved in sessions and automation. Never rename them.
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    juce::String oneDecimal (float value, int)  { return juce::String (value, 1); }
    juce::String wholeNumber (float value, int) { return juce::String (juce::roundToInt (value)); }
    juce::String twoDecimals (float value, int) { return juce::String (value, 2); }

    std::unique_ptr<juce::AudioParameterFloat> makeFloat (const juce::String& id, const juce::String& name, juce::NormalisableRange<float> range,
                                                          float defaultValue, const char* unit,
                                                          juce::String (*format) (float, int))
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue,
                                                            juce::AudioParameterFloatAttributes().withLabel (unit)
                                                                                                 .withStringFromValueFunction (format));
    }

    // The small step also makes a value read back exactly as it was set, which AU hosts and auval check.
    juce::NormalisableRange<float> skewed (float low, float high, float centre, float step)
    {
        juce::NormalisableRange<float> range (low, high, step);
        range.setSkewForCentre (centre);
        return range;
    }

    Layout makeLayout()
    {
        const smoothres::Params defaults;
        Layout layout;

        layout.add (makeFloat ("depth", "Depth", { 0.0f, 24.0f }, defaults.depthDb, "dB", oneDecimal));
        layout.add (makeFloat ("sharpness", "Sharpness", { 0.0f, 100.0f }, defaults.sharpness * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("selectivity", "Selectivity", { 0.0f, 100.0f }, defaults.selectivity * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("attack", "Attack", skewed (1.0f, 200.0f, 20.0f, 0.01f), defaults.attackMs, "ms", oneDecimal));
        layout.add (makeFloat ("release", "Release", skewed (10.0f, 1000.0f, 120.0f, 0.1f), defaults.releaseMs, "ms", wholeNumber));
        layout.add (makeFloat ("lowFreq", "Low Frequency", skewed (20.0f, 20000.0f, 1000.0f, 0.1f), defaults.lowHz, "Hz", wholeNumber));
        layout.add (makeFloat ("highFreq", "High Frequency", skewed (20.0f, 20000.0f, 1000.0f, 0.1f), defaults.highHz, "Hz", wholeNumber));
        layout.add (makeFloat ("mix", "Mix", { 0.0f, 100.0f }, defaults.mix * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("outputGain", "Output Gain", { -24.0f, 24.0f }, defaults.outputDb, "dB", oneDecimal));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "delta", 1 }, "Delta", false));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "stereoMode", 1 }, "Stereo Mode",
                                                                  juce::StringArray { "Stereo", "Mid/Side" }, 0));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));

        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mode", 1 }, "Mode", juce::StringArray { "Soft", "Hard" }, 0));
        layout.add (makeFloat ("hardThreshold", "Hard Threshold", { -80.0f, 0.0f }, defaults.hardThresholdDb, "dB", oneDecimal));
        layout.add (makeFloat ("link", "Stereo Link", { 0.0f, 100.0f }, defaults.link * 100.0f, "%", wholeNumber));
        layout.add (makeFloat ("stereoFocus", "Stereo Focus", { -100.0f, 100.0f }, defaults.stereoFocus * 100.0f, "%", wholeNumber));

        for (int b = 0; b < smoothres::Params::numBands; ++b)
        {
            const auto number = juce::String (b + 1);
            layout.add (makeFloat ("band" + number + "Freq", "Band " + number + " Frequency", skewed (20.0f, 20000.0f, 1000.0f, 0.1f), defaults.bands[b].hz, "Hz", wholeNumber));
            layout.add (makeFloat ("band" + number + "Sens", "Band " + number + " Sensitivity", { -12.0f, 12.0f }, defaults.bands[b].sensitivityDb, "dB", oneDecimal));
            layout.add (makeFloat ("band" + number + "Width", "Band " + number + " Width", skewed (0.1f, 4.0f, 1.0f, 0.01f), defaults.bands[b].widthOctaves, "oct", twoDecimals));
        }

        layout.add (makeFloat ("sharpnessTilt", "Sharpness Tilt", { -100.0f, 100.0f }, 0.0f, "%", wholeNumber));
        layout.add (makeFloat ("attackTilt", "Attack Tilt", { -100.0f, 100.0f }, 0.0f, "%", wholeNumber));
        layout.add (makeFloat ("releaseTilt", "Release Tilt", { -100.0f, 100.0f }, 0.0f, "%", wholeNumber));
        layout.add (makeFloat ("wetTrim", "Wet Trim", { -24.0f, 24.0f }, 0.0f, "dB", oneDecimal));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "sideChain", 1 }, "Side Chain", false));
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "sideChainListen", 1 }, "Side Chain Listen", false));

        // Added in 0.5.0. At their defaults these change nothing, so old sessions and presets sound the same.
        layout.add (makeFloat ("maxCut", "Max Cut", { 0.0f, 24.0f, 0.01f }, defaults.maxCutDb, "dB", oneDecimal));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "quality", 1 }, "Quality",
                                                                  juce::StringArray { "Low Latency", "Normal", "High" }, (int) defaults.quality));
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "bandListen", 1 }, "Band Listen",
                                                                  juce::StringArray { "Off", "Dot 1", "Dot 2", "Dot 3", "Dot 4", "Dot 5", "Dot 6", "Dot 7", "Dot 8" }, 0));

        for (int b = 0; b < smoothres::Params::numBands; ++b)
        {
            const auto number = juce::String (b + 1);
            layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "band" + number + "On", 1 }, "Band " + number + " On", defaults.bands[b].on));
        }

        // Added in 0.6.0, last in the list. Off is the sound of 0.5.1. On in a new instance since 0.8.0: Luke said yes to this on
        // 2026-11-11. The engine's own default stays off (smoothres::Params), and a state saved before 0.6.0, which has no Held
        // Notes value, loads with it off (setStateInformation), so a session saved before it existed plays as it did.
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "heldNotes", 1 }, "Held Notes", true));

        return layout;
    }

    struct Band { float hz, sens, width; };

    struct Preset
    {
        const char* name;
        float depth, sharpness, selectivity, attack, release, low, high, mix;
        int mode;
        float hardThreshold;
        int midSide, sideChain;
        Band bands[3];

        // Added in 0.5.0. Presets that do not mention these keep today's sound.
        float maxCut = 24.0f;
        int quality = 1;
        unsigned dotsOn = 0b00000111;
        Band moreBands[5] = { { 150.0f, 0.0f, 1.0f }, { 600.0f, 0.0f, 1.0f }, { 1500.0f, 0.0f, 1.0f }, { 5000.0f, 0.0f, 1.0f }, { 11000.0f, 0.0f, 1.0f } };

        // Added in 0.6.0. Every preset that does not mention it turns Held Notes off.
        int heldNotes = 0;
    };

    constexpr Band flat1 { 300.0f, 0.0f, 1.0f }, flat2 { 3000.0f, 0.0f, 1.0f }, flat3 { 7000.0f, 0.0f, 1.0f };

    constexpr Preset presets[] = {
        //                         Depth Sharp  Sel  Atk  Rel    Low    High   Mix  Mode  Hard  M/S  SC   bands
        { "Gentle Vocal Polish",    6.0f, 50.0f, 50.0f, 15.0f, 120.0f, 1000.0f, 12000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, flat3 } },
        { "Harsh Lead Vocal",      12.0f, 50.0f, 50.0f, 10.0f, 100.0f, 1200.0f, 10000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, { 3200.0f, 2.0f, 1.5f }, flat3 } },
        { "Vocal Stack Smoother",   9.0f, 35.0f, 40.0f, 25.0f, 200.0f,  800.0f, 14000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, flat3 } },
        { "Wide Stack Mid Side",    8.0f, 40.0f, 45.0f, 20.0f, 180.0f,  800.0f, 14000.0f, 100.0f, 0, -36.0f, 1, 0, { flat1, flat2, flat3 } },
        { "Sibilance Soother",     10.0f, 30.0f, 35.0f,  5.0f,  70.0f, 4500.0f, 14000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, { 7000.0f, 3.0f, 1.0f } } },
        // Gentle on purpose (values of 0.7.1): from 300 Hz to 1.1 kHz the first harmonics of a voice are its body, and each of
        // them reads as a peak. Up to 0.7.0 this was Depth 8, Selectivity 45, Low 180 Hz and dot 1 at +3 dB, which thinned the voice.
        { "Boxy Low Mids",          3.0f, 40.0f, 80.0f, 30.0f, 220.0f,  300.0f,  1100.0f, 100.0f, 0, -36.0f, 0, 0, { { 400.0f, 0.0f, 1.0f }, flat2, flat3 } },
        { "Hard Whistle Catcher",  14.0f, 80.0f, 70.0f,  5.0f,  80.0f, 1500.0f, 12000.0f, 100.0f, 1, -42.0f, 0, 0, { flat1, flat2, flat3 } },
        { "Side Chain Carve",       6.0f, 30.0f,  0.0f, 10.0f, 120.0f,  300.0f,  8000.0f, 100.0f, 1, -40.0f, 0, 1, { flat1, flat2, flat3 } },

        // These show off what 0.5.0 added. New presets go after the old ones, so the old numbers stay the same.
        // Cuts quickly with a quarter of the delay. For singing through it while recording.
        { "Tracking Low Latency",   8.0f, 40.0f, 45.0f,  5.0f,  80.0f, 1000.0f, 12000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, flat3 }, 24.0f, 0 },
        // Every dot in use: protects the body of the voice and the air, and looks harder where a vocal gets harsh.
        { "Eight Dot Vocal Sculpt", 12.0f, 50.0f, 45.0f, 10.0f, 120.0f,  250.0f, 14000.0f, 100.0f, 0, -36.0f, 0, 0,
          // Dots 1, 4 and 5 hold the body harder since 0.7.1 (they were -8 dB, -5 dB, and +1 dB over 0.7 octaves).
          { { 300.0f, -12.0f, 1.0f }, { 3000.0f, 4.0f, 0.8f }, { 7000.0f, 3.0f, 0.7f } }, 24.0f, 1, 0b11111111,
          { { 500.0f, -8.0f, 0.8f }, { 900.0f, -6.0f, 1.0f }, { 1800.0f, 2.0f, 0.7f }, { 4500.0f, 3.0f, 0.7f }, { 11000.0f, -6.0f, 1.2f } } },
        // Depth at its strongest with a ceiling: finest detail, wider areas held down, and never more than 6 dB anywhere.
        { "High Detail With Ceiling", 24.0f, 70.0f, 50.0f, 15.0f, 150.0f, 800.0f, 14000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, flat3 }, 6.0f, 2 },

        // Added in 0.6.0, after every older preset so that their numbers stay the same. The default settings with Held Notes on.
        { "Gentle Polish With Held Notes", 6.0f, 50.0f, 50.0f, 15.0f, 120.0f, 1000.0f, 12000.0f, 100.0f, 0, -36.0f, 0, 0, { flat1, flat2, flat3 }, 24.0f, 1, 0b00000111,
          { { 150.0f, 0.0f, 1.0f }, { 600.0f, 0.0f, 1.0f }, { 1500.0f, 0.0f, 1.0f }, { 5000.0f, 0.0f, 1.0f }, { 11000.0f, 0.0f, 1.0f } }, 1 },
    };
}

SmoothResonanceProcessor::SmoothResonanceProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withInput ("Side Chain", juce::AudioChannelSet::stereo(), false)),
      parameters (*this, nullptr, "SmoothResonance", makeLayout()),
      depth (parameters.getRawParameterValue ("depth")),
      sharpness (parameters.getRawParameterValue ("sharpness")),
      selectivity (parameters.getRawParameterValue ("selectivity")),
      attack (parameters.getRawParameterValue ("attack")),
      release (parameters.getRawParameterValue ("release")),
      lowFreq (parameters.getRawParameterValue ("lowFreq")),
      highFreq (parameters.getRawParameterValue ("highFreq")),
      mix (parameters.getRawParameterValue ("mix")),
      outputGain (parameters.getRawParameterValue ("outputGain")),
      delta (parameters.getRawParameterValue ("delta")),
      stereoMode (parameters.getRawParameterValue ("stereoMode")),
      bypass (parameters.getRawParameterValue ("bypass")),
      mode (parameters.getRawParameterValue ("mode")),
      hardThreshold (parameters.getRawParameterValue ("hardThreshold")),
      link (parameters.getRawParameterValue ("link")),
      stereoFocus (parameters.getRawParameterValue ("stereoFocus")),
      sharpnessTilt (parameters.getRawParameterValue ("sharpnessTilt")),
      attackTilt (parameters.getRawParameterValue ("attackTilt")),
      releaseTilt (parameters.getRawParameterValue ("releaseTilt")),
      wetTrim (parameters.getRawParameterValue ("wetTrim")),
      sideChain (parameters.getRawParameterValue ("sideChain")),
      sideChainListen (parameters.getRawParameterValue ("sideChainListen")),
      maxCut (parameters.getRawParameterValue ("maxCut")),
      quality (parameters.getRawParameterValue ("quality")),
      bandListen (parameters.getRawParameterValue ("bandListen")),
      heldNotes (parameters.getRawParameterValue ("heldNotes"))
{
    for (int b = 0; b < smoothres::Params::numBands; ++b)
    {
        const auto number = juce::String (b + 1);
        bandFreq[b]  = parameters.getRawParameterValue ("band" + number + "Freq");
        bandSens[b]  = parameters.getRawParameterValue ("band" + number + "Sens");
        bandWidth[b] = parameters.getRawParameterValue ("band" + number + "Width");
        bandOn[b]    = parameters.getRawParameterValue ("band" + number + "On");
    }

    using Cat = juce::AudioProcessorParameter::Category;
    maxReduction = new ReadingParameter ("maxReduction", "Max Reduction", "dB", ReadingParameter::Style::reduction, 24.0f, Cat::compressorLimiterGainReductionMeter);
    maxReductionFreq = new ReadingParameter ("maxReductionFreq", "Max Reduction Frequency", "Hz", ReadingParameter::Style::hertz, 24000.0f, Cat::otherMeter);
    addParameter (maxReduction);
    addParameter (maxReductionFreq);

    // A host can ask for the latency before it prepares us, so have the right answer for the usual rates.
    setLatencySamples (smoothres::ResonanceEngine::fftSizeForRate (48000.0));

    parameters.addParameterListener ("quality", this);
    startTimerHz (5);
}

SmoothResonanceProcessor::~SmoothResonanceProcessor()
{
    stopTimer();
    parameters.removeParameterListener ("quality", this);
}

int SmoothResonanceProcessor::latencyForQuality (smoothres::Quality q) const noexcept
{
    const auto rate = getSampleRate();
    return smoothres::ResonanceEngine::fftSizeForRate (rate > 0.0 ? rate : 48000.0, q);
}

void SmoothResonanceProcessor::syncLatencyToQuality()
{
    const auto want = latencyForQuality ((smoothres::Quality) juce::jlimit (0, smoothres::numQualities - 1, juce::roundToInt (quality->load())));

    if (getLatencySamples() != want)
        setLatencySamples (want);
}

void SmoothResonanceProcessor::parameterChanged (const juce::String&, float)
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        syncLatencyToQuality();
}

void SmoothResonanceProcessor::timerCallback()
{
    syncLatencyToQuality();
}

juce::String SmoothResonanceProcessor::getLayoutName() const
{
    return getChannelLayoutOfBus (false, 0).getDescription();
}

juce::StringArray SmoothResonanceProcessor::getChannelNames() const
{
    juce::StringArray names;
    const auto set = getChannelLayoutOfBus (false, 0);

    for (int ch = 0; ch < set.size(); ++ch)
        names.add (juce::AudioChannelSet::getAbbreviatedChannelTypeName (set.getTypeOfChannel (ch)));

    return names;
}

int SmoothResonanceProcessor::getLfeChannel() const
{
    return getChannelLayoutOfBus (false, 0).getChannelIndexForType (juce::AudioChannelSet::LFE);
}

bool SmoothResonanceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    using Set = juce::AudioChannelSet;
    const auto out = layouts.getMainOutputChannelSet();

    // In and out must match. Discrete channels are left out on purpose: with no names there is no telling which one is the LFE.
    static const std::array<Set, 16> allowed { Set::mono(), Set::stereo(), Set::quadraphonic(), Set::createLCRS(), Set::create5point0(), Set::create5point1(),
                                               Set::create6point0(), Set::create6point1(), Set::create7point0(), Set::create7point1(),
                                               Set::create5point1point2(), Set::create7point1point2(), Set::create5point1point4(),
                                               Set::create7point0point4(), Set::create7point1point4(), Set::create9point1point6() };

    if (std::find (allowed.begin(), allowed.end(), out) == allowed.end() || layouts.getMainInputChannelSet() != out)
        return false;

    if (layouts.inputBuses.size() < 2)
        return true;

    const auto side = layouts.inputBuses.getReference (1);
    return side.isDisabled() || side == juce::AudioChannelSet::mono() || side == juce::AudioChannelSet::stereo();
}

void SmoothResonanceProcessor::prepareToPlay (double sampleRate, int)
{
    // Which channel is which, from the host's layout. Names decide: the LFE only gets the delay, and left and right side
    // channels follow Stereo Focus and pick a side chain channel. Any other position counts as centre.
    using Set = juce::AudioChannelSet;
    const auto set = getChannelLayoutOfBus (false, 0);
    std::array<smoothres::ChannelRole, smoothres::maxEngineChannels> roles {};

    for (int ch = 0; ch < juce::jmin (set.size(), smoothres::maxEngineChannels); ++ch)
    {
        switch (set.getTypeOfChannel (ch))
        {
            case Set::LFE: case Set::LFE2:
                roles[(size_t) ch] = smoothres::ChannelRole::lfe;
                break;

            case Set::left: case Set::leftSurround: case Set::leftCentre: case Set::leftSurroundSide: case Set::leftSurroundRear:
            case Set::wideLeft: case Set::topFrontLeft: case Set::topSideLeft: case Set::topRearLeft:
                roles[(size_t) ch] = smoothres::ChannelRole::left;
                break;

            case Set::right: case Set::rightSurround: case Set::rightCentre: case Set::rightSurroundSide: case Set::rightSurroundRear:
            case Set::wideRight: case Set::topFrontRight: case Set::topSideRight: case Set::topRearRight:
                roles[(size_t) ch] = smoothres::ChannelRole::right;
                break;

            default:
                roles[(size_t) ch] = smoothres::ChannelRole::centre;
                break;
        }
    }

    engine.prepare (sampleRate, getMainBusNumOutputChannels(), roles.data());
    setLatencySamples (latencyForQuality ((smoothres::Quality) juce::jlimit (0, smoothres::numQualities - 1, juce::roundToInt (quality->load()))));
    maxReduction->setReading (0.0f);
    maxReductionFreq->setReading (0.0f);
}

double SmoothResonanceProcessor::getTailLengthSeconds() const
{
    const auto rate = getSampleRate();
    return rate > 0.0 ? getLatencySamples() / rate : 0.0;
}

void SmoothResonanceProcessor::render (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);

    smoothres::Params p;
    p.depthDb     = depth->load();
    p.sharpness   = sharpness->load() * 0.01f;
    p.selectivity = selectivity->load() * 0.01f;
    p.attackMs    = attack->load();
    p.releaseMs   = release->load();
    p.lowHz       = lowFreq->load();
    p.highHz      = highFreq->load();
    p.mix         = mix->load() * 0.01f;
    p.outputDb    = outputGain->load();
    p.delta       = delta->load() > 0.5f;
    p.midSide     = stereoMode->load() > 0.5f;
    p.bypass      = forceBypass || bypass->load() > 0.5f;

    p.hard            = mode->load() > 0.5f;
    p.hardThresholdDb = hardThreshold->load();
    p.link            = link->load() * 0.01f;
    p.stereoFocus     = stereoFocus->load() * 0.01f;
    p.sharpnessTilt   = sharpnessTilt->load() * 0.01f;
    p.attackTilt      = attackTilt->load() * 0.01f;
    p.releaseTilt     = releaseTilt->load() * 0.01f;
    p.wetTrimDb       = wetTrim->load();
    p.sideChain       = sideChain->load() > 0.5f;
    p.sideChainListen = sideChainListen->load() > 0.5f;

    for (int b = 0; b < smoothres::Params::numBands; ++b)
        p.bands[b] = { bandFreq[b]->load(), bandSens[b]->load(), bandWidth[b]->load(), bandOn[b]->load() > 0.5f };

    p.maxCutDb   = maxCut->load();
    p.heldNotes  = heldNotes->load() > 0.5f;
    p.quality    = (smoothres::Quality) juce::jlimit (0, smoothres::numQualities - 1, juce::roundToInt (quality->load()));
    p.bandListen = juce::jlimit (0, smoothres::Params::numBands, juce::roundToInt (bandListen->load()));

    engine.setParams (p);

    // The main bus comes first in the buffer, then the side chain if the host connected one.
    const int mainCh = getMainBusNumOutputChannels();
    const auto* sideBus = getBusCount (true) > 1 ? getBus (true, 1) : nullptr;
    const int sideCh = sideBus != nullptr && sideBus->isEnabled() ? sideBus->getNumberOfChannels() : 0;
    const int usableKey = buffer.getNumChannels() >= mainCh + sideCh ? juce::jmin (sideCh, 2) : 0;
    const float* key[2] = { nullptr, nullptr };

    for (int ch = 0; ch < usableKey; ++ch)
        key[ch] = buffer.getReadPointer (mainCh + ch);

    engine.process (buffer.getArrayOfWritePointers(), juce::jmin (mainCh, buffer.getNumChannels()), buffer.getNumSamples(), key, usableKey);

    maxReduction->setReading (engine.getMaxReductionDb());
    maxReductionFreq->setReading (engine.getMaxReductionHz());
}

void SmoothResonanceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, false);
}

void SmoothResonanceProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, true);
}

juce::AudioProcessorParameter* SmoothResonanceProcessor::getBypassParameter() const
{
    return parameters.getParameter ("bypass");
}

juce::AudioProcessorEditor* SmoothResonanceProcessor::createEditor()
{
    return new ResonanceEditor (*this);
}

int SmoothResonanceProcessor::getNumPrograms()
{
    return (int) std::size (presets);
}

const juce::String SmoothResonanceProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? presets[index].name : "";
}

void SmoothResonanceProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms()))
        return;

    currentProgram = index;
    const auto& x = presets[index];

    const auto set = [this] (const juce::String& id, float value)
    {
        auto* p = parameters.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    };

    set ("depth", x.depth);
    set ("sharpness", x.sharpness);
    set ("selectivity", x.selectivity);
    set ("attack", x.attack);
    set ("release", x.release);
    set ("lowFreq", x.low);
    set ("highFreq", x.high);
    set ("mix", x.mix);
    set ("outputGain", 0.0f);
    set ("delta", 0.0f);
    set ("stereoMode", (float) x.midSide);
    set ("mode", (float) x.mode);
    set ("hardThreshold", x.hardThreshold);
    set ("link", 100.0f);
    set ("stereoFocus", 0.0f);
    set ("sharpnessTilt", 0.0f);
    set ("attackTilt", 0.0f);
    set ("releaseTilt", 0.0f);
    set ("wetTrim", 0.0f);
    set ("sideChain", (float) x.sideChain);
    set ("sideChainListen", 0.0f);

    set ("maxCut", x.maxCut);
    set ("heldNotes", (float) x.heldNotes);
    set ("quality", (float) x.quality);
    set ("bandListen", 0.0f);

    for (int b = 0; b < smoothres::Params::numBands; ++b)
    {
        const auto number = juce::String (b + 1);
        const auto& band = b < 3 ? x.bands[b] : x.moreBands[b - 3];
        set ("band" + number + "Freq", band.hz);
        set ("band" + number + "Sens", band.sens);
        set ("band" + number + "Width", band.width);
        set ("band" + number + "On", ((x.dotsOn >> b) & 1u) != 0 ? 1.0f : 0.0f);
    }

    syncLatencyToQuality();
}

void SmoothResonanceProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    if (const auto xml = parameters.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (advancedOpenAttribute, advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, destination);
    }
}

void SmoothResonanceProcessor::setStateInformation (const void* data, int size)
{
    if (const auto xml = getXmlFromBinary (data, size))
    {
        if (xml->hasTagName (parameters.state.getType()))
        {
            // Older sessions have no window state. A damaged size goes back in range. Neither is a parameter, so they stay
            // out of the value tree.
            if (xml->hasAttribute (windowWidthAttribute))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute (windowWidthAttribute, defaultWindowWidth)), std::memory_order_relaxed);

            advancedOpen.store (xml->getBoolAttribute (advancedOpenAttribute, false), std::memory_order_relaxed);

            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (advancedOpenAttribute);

            // Held Notes is on in a new instance since 0.8.0. A state with no Held Notes value was saved before 0.6.0 and played
            // without it, so it loads with it off and sounds as it was saved. A damaged value counts as none, since otherwise
            // applySafeState would drop it and leave the new default.
            if (! hasUsableValue (*xml, "heldNotes"))
            {
                for (auto* child = xml->getFirstChildElement(); child != nullptr;)
                {
                    auto* next = child->getNextElement();
                    if (child->getStringAttribute ("id") == "heldNotes")
                        xml->removeChildElement (child, true);
                    child = next;
                }

                auto* off = xml->createNewChildElement ("PARAM");   // the tag the value tree gives every parameter
                off->setAttribute ("id", "heldNotes");
                off->setAttribute ("value", 0.0);
            }

            applySafeState (parameters, *xml);
            syncLatencyToQuality();
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SmoothResonanceProcessor();
}
