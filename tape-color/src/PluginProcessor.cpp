#include "PluginProcessor.h"
#include "StateGuard.h"
#include <complex>

using namespace tapecolor;

namespace
{
    constexpr float kLowCutOff = 20.0f, kHighCutOff = 20000.0f;
    constexpr double kEps = 1.0e-10; // detector floor, about -100 dBFS

    // Head Azimuth as -1 to 1. The control moves in steps of 0.1 and its middle comes back from the host as 0.0000015, not 0. Left like
    // that, each channel read through the moving head's interpolation a hair either side of a whole sample, and left and right of a
    // mono signal differed in the last bit. Anything under half a step is the middle.
    inline float tiltAmount (float percent) noexcept { return std::abs (percent) < 0.05f ? 0.0f : percent * 0.01f; }

    // The last input that stood perfectly still, per lane, with its exact curve value.
    struct HeldCurve { double in[2] {}, out[2] {}; };

    // Where two samples are this close, the average along the curve is the curve at their middle.
    // A signal standing perfectly still gets the exact value, so silence cancels the curve's offset down to exactly zero.
    inline Lanes closeCurve (Lanes curve, LaneMask close, Lanes now, Lanes difference, Lanes before, HeldCurve& held) noexcept
    {
        Lanes middle = fastTanh (0.5 * (now + before));
        const LaneMask still = difference == 0.0;
        for (int lane = 0; lane < 2; ++lane)
        {
            if (still[lane] != 0)
            {
                if (now[lane] != held.in[lane])
                {
                    held.in[lane] = now[lane];
                    held.out[lane] = std::tanh (now[lane]);
                }
                middle[lane] = held.out[lane];
            }
        }
        return (Lanes) (((LaneMask) middle & close) | ((LaneMask) curve & ~close));
    }

    // Sum of a[k] * b[k]. Each product is 32 bit and the running sum 64 bit.
    inline double weightedSum (const float* a, const float* b, int size) noexcept
    {
        using Four = float __attribute__ ((vector_size (16), aligned (4), may_alias));
        using Two = float __attribute__ ((vector_size (8)));
        Lanes low { 0.0, 0.0 }, high { 0.0, 0.0 };
        int k = 0;
        for (; k + 4 <= size; k += 4)
        {
            const Four p = *(const Four*) (a + k) * *(const Four*) (b + k);
            low += __builtin_convertvector (Two { p[0], p[1] }, Lanes);
            high += __builtin_convertvector (Two { p[2], p[3] }, Lanes);
        }
        double sum = low[0] + low[1] + high[0] + high[1];
        for (; k < size; ++k)
            sum += (double) (a[k] * b[k]);
        return sum;
    }

    struct Preset
    {
        const char* name;
        float drive, bias; int speed; float wowAmt, flutterAmt, noiseAmt, ageAmt, low, high, mixAmt, out; bool autoOn;
        float hard, space, tilt, rnd, stereo, dropD, dropR; int comp; float enc, dec, tex; int dev; float devMix;
        bool old; // keeps 0.3.1's response: the presets whose sound hangs on Mix under 100, High Cut, the compander, or Auto Level on darkened tape
    };
    // speed: 0 = 3.75, 1 = 7.5, 2 = 15, 3 = 30 inches per second. comp: 0 off, 1 fixed, 2 soft, 3 strong.
    // dev: 0 off, 1 Small Radio, 2 Telephone, 3 Small Speaker, 4 Dictation Machine, 5 Old Console.
    const juce::Identifier windowWidthAttribute { "windowWidth" }, windowHeightAttribute { "windowHeight" }, advancedOpenAttribute { "advancedOpen" };
    // Which response a saved session uses: 1 is 0.3.1's, 2 is the lined up one. It is written after the settings, as eight bytes that
    // follow the XML, so the settings themselves read exactly as before in every version. A session from before 0.4.0 has no such bytes.
    constexpr char responseTag[4] = { 'T', 'C', 'r', 's' };
    constexpr int responseOld = 1, responseLinedUp = 2;

    int savedResponse (const void* data, int size) noexcept
    {
        if (size < 9)
            return 0;
        const auto* bytes = static_cast<const juce::uint8*> (data);
        const size_t tailAt = 8 + (size_t) juce::ByteOrder::littleEndianInt (bytes + 4) + 1; // magic, length, the XML and its closing zero
        if (tailAt + 8 > (size_t) size || std::memcmp (bytes + tailAt, responseTag, 4) != 0)
            return 0;
        return (int) bytes[tailAt + 4];
    }

    constexpr Preset presets[] = {
        //                  drive   bias sp   wow flutter noise    age  low        high         mix   out auto    hard space  tilt   rnd stereo dropD dropR comp   enc    dec   tex dev devMix old
        { "Tape Start",     12.0f,   0.0f, 2,  0.0f,  0.0f,  0.0f,   0.0f, kLowCutOff, kHighCutOff, 100.0f, 0.0f, true,  50.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f,  0.0f, 0, 100.0f, false },
        { "Clean Reel",      6.0f,   0.0f, 3,  0.0f,  0.0f,  0.0f,   0.0f, kLowCutOff, kHighCutOff, 100.0f, 0.0f, true,  35.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f,  0.0f, 0, 100.0f, false },
        { "Vocal Glue",     13.0f,  15.0f, 2,  3.0f,  4.0f,  0.0f,   0.0f, 40.0f,      kHighCutOff, 100.0f, 0.0f, true,  50.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f,  0.0f, 0, 100.0f, false },
        { "Warm Lead",      17.0f,  30.0f, 2,  4.0f,  6.0f,  5.0f,   8.0f, 70.0f,      kHighCutOff, 100.0f, 0.0f, true,  50.0f, 10.0f, 0.0f,  5.0f,  0.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f,  5.0f, 0, 100.0f, false },
        { "Airy Encode",     8.0f,   5.0f, 3,  0.0f,  3.0f,  0.0f,   0.0f, 60.0f,      kHighCutOff, 100.0f, 0.0f, true,  40.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 1.0f, 2,  60.0f,   0.0f,  0.0f, 0, 100.0f, true },
        { "Parallel Body",  26.0f,  35.0f, 2,  0.0f,  0.0f,  0.0f,   0.0f, 80.0f,      12000.0f,     40.0f, 0.0f, true,  70.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f,  0.0f, 0, 100.0f, true },
        { "Slow Reel Haze", 14.0f,  10.0f, 1, 15.0f, 12.0f, 15.0f,  20.0f, 50.0f,      kHighCutOff, 100.0f, 0.0f, true,  50.0f, 25.0f, 8.0f, 20.0f, 25.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f, 10.0f, 0, 100.0f, true },
        { "Starved Grit",   10.0f, -70.0f, 1,  5.0f, 10.0f, 10.0f,  10.0f, 90.0f,      kHighCutOff, 100.0f, 0.0f, true,  75.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f, 30.0f, 0, 100.0f, true },
        { "Worn Cassette",  15.0f, -15.0f, 0, 35.0f, 30.0f, 20.0f, 100.0f, 90.0f,      kHighCutOff, 100.0f, 0.0f, true,  60.0f, 30.0f, 15.0f, 40.0f, 30.0f, 35.0f, 0.7f, 2, 100.0f,  60.0f, 25.0f, 0, 100.0f, true },
        { "Old Radio Voice", 16.0f,  10.0f, 1, 10.0f, 15.0f, 15.0f,  25.0f, kLowCutOff, kHighCutOff, 100.0f, 0.0f, true,  60.0f, 0.0f, 0.0f, 10.0f,  0.0f, 10.0f, 0.5f, 0, 100.0f, 100.0f, 25.0f, 1, 100.0f, true },
        { "Phone Call",     18.0f,   0.0f, 2,  0.0f,  8.0f, 10.0f,   0.0f, kLowCutOff, kHighCutOff, 100.0f, 0.0f, true,  70.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 1.0f, 0, 100.0f, 100.0f, 15.0f, 2, 100.0f, false },
    };
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto percent = [&] (const char* id, const char* name)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                                           AudioParameterFloatAttributes().withLabel ("%")));
    };
    // IDs are saved in sessions and automation: never rename them once installed.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "drive", 1 }, "Drive", NormalisableRange<float> (0.0f, 30.0f, 0.01f), 12.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "bias", 1 }, "Bias", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "speed", 1 }, "Tape Speed", StringArray { "3.75 ips", "7.5 ips", "15 ips", "30 ips" }, 2));
    percent ("wow", "Wow");
    percent ("flutter", "Flutter");
    percent ("noise", "Noise");
    percent ("age", "Age");

    NormalisableRange<float> lowRange (kLowCutOff, 800.0f, 0.1f);
    lowRange.setSkewForCentre (100.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lowcut", 1 }, "Low Cut", lowRange, kLowCutOff,
        AudioParameterFloatAttributes().withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return v <= kLowCutOff + 0.01f ? String ("Off") : String (v, 1); })
            .withValueFromStringFunction ([] (const String& s) { return s.trim().equalsIgnoreCase ("off") ? kLowCutOff : s.getFloatValue(); })));

    NormalisableRange<float> highRange (1000.0f, kHighCutOff, 1.0f);
    highRange.setSkewForCentre (5000.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "highcut", 1 }, "High Cut", highRange, kHighCutOff,
        AudioParameterFloatAttributes().withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return v >= kHighCutOff - 0.5f ? String ("Off") : String (v, 0); })
            .withValueFromStringFunction ([] (const String& s) { return s.trim().equalsIgnoreCase ("off") ? kHighCutOff : s.getFloatValue(); })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "autolevel", 1 }, "Auto Level", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "hardness", 1 }, "Tape Hardness", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    percent ("spacing", "Head Spacing");
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "azimuth", 1 }, "Head Azimuth", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    percent ("random", "Random Drift");
    percent ("stereodrift", "Stereo Drift");
    percent ("dropdepth", "Dropout Depth");
    NormalisableRange<float> rateRange (0.1f, 10.0f, 0.01f);
    rateRange.setSkewForCentre (1.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "droprate", 1 }, "Dropout Rate", rateRange, 1.0f,
                                                       AudioParameterFloatAttributes().withLabel ("per s")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "compander", 1 }, "Compander", StringArray { "Off", "Fixed Emphasis", "Dynamic Soft", "Dynamic Strong" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "encode", 1 }, "Encode Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "decode", 1 }, "Decode Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    percent ("texture", "Texture");
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "device", 1 }, "Device", StringArray { "Off", "Small Radio", "Telephone", "Small Speaker", "Dictation Machine", "Old Console" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "devicemix", 1 }, "Device Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    return layout;
}

PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", makeLayout())
{
    drive = apvts.getRawParameterValue ("drive");
    bias = apvts.getRawParameterValue ("bias");
    speed = apvts.getRawParameterValue ("speed");
    wow = apvts.getRawParameterValue ("wow");
    flutter = apvts.getRawParameterValue ("flutter");
    noise = apvts.getRawParameterValue ("noise");
    age = apvts.getRawParameterValue ("age");
    lowCut = apvts.getRawParameterValue ("lowcut");
    highCut = apvts.getRawParameterValue ("highcut");
    mix = apvts.getRawParameterValue ("mix");
    output = apvts.getRawParameterValue ("output");
    hardness = apvts.getRawParameterValue ("hardness");
    spacing = apvts.getRawParameterValue ("spacing");
    azimuth = apvts.getRawParameterValue ("azimuth");
    randomDrift = apvts.getRawParameterValue ("random");
    stereoDrift = apvts.getRawParameterValue ("stereodrift");
    dropDepth = apvts.getRawParameterValue ("dropdepth");
    dropRate = apvts.getRawParameterValue ("droprate");
    compander = apvts.getRawParameterValue ("compander");
    encodeAmt = apvts.getRawParameterValue ("encode");
    decodeAmt = apvts.getRawParameterValue ("decode");
    texture = apvts.getRawParameterValue ("texture");
    device = apvts.getRawParameterValue ("device");
    deviceMix = apvts.getRawParameterValue ("devicemix");
    autoLevel = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("autolevel"));
    bypass = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));

    for (int i = 0; i < tapSize; ++i)
    {
        tapIn[(size_t) i].store (0.0f, std::memory_order_relaxed);
        tapOut[(size_t) i].store (0.0f, std::memory_order_relaxed);
    }
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PluginProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    preparedChannels = juce::jlimit (1, 2, getTotalNumOutputChannels());

    // The curve runs near 768 kHz at every session rate, so aliasing does not depend on the rate.
    // The record boost makes high tones hit the curve very hard, which is why this needs so much.
    osStages = sr <= 50000.0 ? 4 : (sr <= 100000.0 ? 3 : 2);
    osRate = sr * (double) (1 << osStages);
    oversampler = std::make_unique<FastOversampler> ((size_t) preparedChannels, (size_t) osStages, false); // linear phase, its own delay is not a whole sample
    oversampler->initProcessing ((size_t) kChunk);

    // The wet path's delay, made exactly a whole number of samples.
    // Up to 0.3.1 the oversampling filters were rounded up to a whole sample with an all pass, and nothing counted what sits between them:
    // the curve's averaging (half a sample at the high rate) and the two top end filters, which at the default tone (15 ips, Age 0) are flat
    // in the band but still 11.7 microseconds late. So the wet signal came out 0.56 sample after the reported latency at 48 kHz and 1.11
    // after it at 96 kHz. Now the filters keep their own delay, those 11.7 microseconds are counted, and a short delay at the high rate
    // fills the rest up to the next whole sample. When Tape Speed, Bias, Age or Head Spacing move their filters that short delay follows
    // (updateCoefs), so the wet path stays on the reported latency for as long as there is delay left to take off (trimRoom).
    {
        const double factor = (double) (1 << osStages);
        const double between = flatToneDelay (osRate);
        const double filtersAlone = (double) oversampler->getLatencyInSamples();
        osLatency = (int) std::ceil (filtersAlone + (between + 1.0) / factor - 1.0e-9);
        trimRoom = ((double) osLatency - filtersAlone) * factor;
        trimLinedUp = trimRoom - between; // 1 or more at the default tone

        // The old response puts the wet path back where 0.3.1 had it against the dry: the all pass, and whole samples for the rest.
        double part = 1.0 - (filtersAlone - std::floor (filtersAlone));
        if (part > 1.0 - 1.0e-6)
            part = 0.0;
        else if (part < 0.618)
            part += 1.0;
        oldAllPassUsed = part > 0.0;
        oldAllPass = (float) ((1.0 - part) / (1.0 + part));
        trimOld = ((double) osLatency - std::round (filtersAlone + part)) * factor;
        jassert (trimLinedUp >= 1.0 && trimOld >= 0.0 && std::max (trimRoom, trimOld) + 3.0 < (double) trimSize);
    }
    modCentre = (int) std::ceil (kModCentreMs * 0.001 * sr);
    setLatencySamples (osLatency + modCentre);

    dryRingSize = osLatency + modCentre + 1;
    gainRingSize = osLatency + 1;
    gainRing.assign ((size_t) gainRingSize, 1.0f);
    const int needed = modCentre + (int) std::ceil ((kModLimitMs + kAzimuthMaxMs) * 0.001 * sr) + SincTable::taps + 2;
    int size = 256;
    while (size < needed)
        size *= 2;
    lineMask = size - 1;
    for (auto& c : chans)
    {
        c.dryRing.assign ((size_t) dryRingSize, 0.0f);
        c.encodedRing.assign ((size_t) gainRingSize, 0.0f);
        c.line.assign ((size_t) size, 0.0f);
    }
    buildDeviceFilters();
    work.setSize (preparedChannels, kChunk);
    dryNear.setSize (preparedChannels, kChunk);
    dryFar.setSize (preparedChannels, kChunk);
    encodedNear.setSize (preparedChannels, kChunk);

    // Drive ramps slower than the level detector, so Auto Level keeps up with it.
    driveSm.reset (sr, 0.16);
    biasSm.reset (sr, 0.08);
    speedSm.reset (sr, 0.12);
    ageSm.reset (sr, 0.12);
    wowSm.reset (sr, 0.1);      // slower, because a depth change moves the read head and is heard as pitch
    flutterSm.reset (sr, 0.1);
    depthSm.reset (sr, 0.15);
    noiseSm.reset (sr, 0.03);
    mixSm.reset (sr, 0.02);
    outputSm.reset (sr, 0.02);
    autoSm.reset (sr, 0.03);
    onSm.reset (sr, 0.02);
    lowOnSm.reset (sr, 0.03);
    highOnSm.reset (sr, 0.03);
    lowCutSm.reset (sr, 0.03);
    highCutSm.reset (sr, 0.03);
    hardSm.reset (sr, 0.08);
    spacingSm.reset (sr, 0.05);
    azimuthSm.reset (sr, 0.1);   // moves the read head, so it is slow like Wow
    randomSm.reset (sr, 0.1);
    stereoSm.reset (sr, 0.1);
    dropSm.reset (sr, 0.05);
    encodeSm.reset (sr, 0.03);
    decodeSm.reset (sr, 0.03);
    textureSm.reset (sr, 0.03);
    deviceMixSm.reset (sr, 0.02);
    deviceFadeSm.reset (sr, 0.015);
    oldBlendSm.reset (sr, 0.03);
    oldToneSm.reset (sr, 0.02);

    dcC = onePoleCoef (4.0, sr);
    detectHpC = onePoleCoef (40.0, sr);
    detectCoefs.setBell (2500.0, sr, 4.0, 0.5);              // rough loudness weighting: more weight on the upper mids
    envC = 1.0 - std::exp (-1.0 / (0.05 * sr));
    ratioC = 1.0 - std::exp (-1.0 / (0.003 * sr));
    noiseHpC = onePoleCoef (150.0, sr);

    const auto inc = [this] (double hz) { return kTwoPi * hz / sr; };
    incWow = inc (kWowHz);
    incWow2 = inc (kWow2Hz);
    incFlutter = inc (kFlutterHz);
    incFlutter2 = inc (kFlutter2Hz);
    incWander1 = inc (0.63);
    incWander2 = inc (1.71);
    incWander3 = inc (3.3);
    wowMs = msForCents (kWowCents, kWowHz);
    flutterMs = msForCents (kFlutterCents, kFlutterHz);

    flutterRandom.prepare (kFlutterRandomValuesPerSecond, sr, 0x3C6EF372u);
    flutterRandomR.prepare (kFlutterRandomValuesPerSecond, sr, 0x6A09E667u);
    slowRandom.prepare (kRandomValuesPerSecond, sr, 0x510E527Fu);
    slowRandomR.prepare (kRandomValuesPerSecond, sr, 0x1F83D9ABu);

    grainLpC = onePoleCoef (8000.0, sr);
    grainHpC = onePoleCoef (1000.0, sr);
    grainNorm = std::sqrt (3.0 * (2.0 - grainLpC) / grainLpC);
    grainEnvC = onePoleCoef (1.0 / (kTwoPi * 0.02), sr);
    sideHpC = onePoleCoef (1000.0, sr);
    sideAttackC = 1.0 - std::exp (-1.0 / (0.002 * sr));
    sideReleaseC = 1.0 - std::exp (-1.0 / (0.08 * sr));
    companderSlewPerSample = 1.0 / (0.005 * sr);
    dropDownC = 1.0 - std::exp (-1.0 / (0.008 * sr));
    dropUpC = 1.0 - std::exp (-1.0 / (0.04 * sr));

    reset();
}

// Turns each device's shape in dB into a short minimum phase filter, through the cepstrum, so it adds no latency.
void PluginProcessor::buildDeviceFilters()
{
    if (deviceIrRate == sr)
        return;
    deviceIrRate = sr;
    deviceLength = juce::jmax (32, (int) std::ceil (kDeviceMs * 0.001 * sr));

    constexpr int order = 14, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<std::complex<float>> in ((size_t) size), out ((size_t) size);
    for (int d = 0; d < kNumDevices; ++d)
    {
        for (int k = 0; k < size; ++k)
        {
            const double hz = juce::jmax (1.0, (k <= size / 2 ? k : size - k) * sr / size);
            in[(size_t) k] = (float) (std::log (10.0) / 20.0 * deviceShapeDb (d, hz));
        }
        fft.perform (in.data(), out.data(), true);
        for (int k = 1; k < size / 2; ++k)
        {
            out[(size_t) k] *= 2.0f;
            out[(size_t) (size - k)] = 0.0f;
        }
        fft.perform (out.data(), in.data(), false);
        for (auto& v : in)
            v = std::exp (v);
        fft.perform (in.data(), out.data(), true);
        auto& ir = deviceIrs[(size_t) d];
        ir.resize ((size_t) deviceLength);
        const int fadeFrom = deviceLength * 3 / 4;
        for (int i = 0; i < deviceLength; ++i)
        {
            const double fade = i < fadeFrom ? 1.0 : 0.5 + 0.5 * std::cos (kPi * (i - fadeFrom) / (double) (deviceLength - fadeFrom));
            ir[(size_t) (deviceLength - 1 - i)] = (float) (out[(size_t) i].real() * fade);
        }
    }
    for (auto& c : chans)
        c.deviceLine.assign ((size_t) deviceLength * 2, 0.0f);
}

void PluginProcessor::reset()
{
    for (size_t ch = 0; ch < chans.size(); ++ch)
    {
        auto& c = chans[ch];
        std::fill (c.dryRing.begin(), c.dryRing.end(), 0.0f);
        std::fill (c.encodedRing.begin(), c.encodedRing.end(), 0.0f);
        std::fill (c.line.begin(), c.line.end(), 0.0f);
        std::fill (c.deviceLine.begin(), c.deviceLine.end(), 0.0f);
        c.encoder = {}; c.decoder = {};
        c.spacing1 = {}; c.spacing2 = {}; c.grainLp = {}; c.grainHp = {};
        c.grainEnv = 0.0;
        c.grainRng.state = ch == 0 ? 0x1B873593u : 0x4CF5AD43u;
        c.pre = {}; c.de = {};
        c.gap = {}; c.ageLp = {}; c.bump = {}; c.dip = {}; c.lowCutF = {}; c.highCutF = {}; c.highCutMatched = {}; c.detectIn = {}; c.detectOut = {};
        c.detectGap = {}; c.detectAge = {}; c.detectSpace = {};
        c.allPassIn = c.allPassOut = 0.0f;
        c.dc = {}; c.ageHp = {}; c.noiseLp = {}; c.noiseHp = {}; c.detectInHp = {}; c.detectOutHp = {};
        c.rng.state = ch == 0 ? 0x2F6E2B1u : 0x51ED270Bu;
    }
    if (oversampler != nullptr)
        oversampler->reset();
    for (auto& t : trimLine)
        t = Lanes { 0.0, 0.0 };
    trimPos = 0;
    dryPos = linePos = gainPos = 0;
    phaseWow = phaseWow2 = phaseFlutter = phaseFlutter2 = phaseWander1 = phaseWander2 = phaseWander3 = 0.0;
    flutterRandom.reset (0x3C6EF372u);
    flutterRandomR.reset (0x6A09E667u);
    slowRandom.reset (0x510E527Fu);
    slowRandomR.reset (0x1F83D9ABu);
    dropRng.state = 0x5BE0CD19u;
    dropEnv = 0.0;
    dropSeverity = 1.0;
    dropHold = 0;
    dropActive = false;
    devicePos = 0;
    envIn = envOut = 0.0;
    ratio = 1.0;
    snapControls();
    for (auto& c : chans)
    {
        const double start = companderBoostDb ((int) compander->load(), -200.0);
        c.encoder.boostDb = c.decoder.boostDb = start;
        const double encDb = start * (double) encodeSm.getCurrentValue(), decDb = start * (double) decodeSm.getCurrentValue();
        const double gE = std::pow (10.0, encDb / 20.0), gD = std::pow (10.0, decDb / 20.0);
        if (companderOld)
        {
            c.encoder.coefs.set (kCompanderHz / std::sqrt (gE), gE, sr);
            c.decoder.coefs.set (kCompanderHz / std::sqrt (gD), gD, sr);
        }
        else
        {
            setMatchedShelf (c.encoder.coefs, kCompanderHz / std::sqrt (gE), gE, sr);
            setMatchedShelf (c.decoder.coefs, kCompanderHz / std::sqrt (gD), gD, sr);
        }
    }
    std::fill (gainRing.begin(), gainRing.end(), juce::Decibels::decibelsToGain (driveSm.getCurrentValue()));
    for (auto& c : chans)
    {
        c.main.reset (lastOffset);
        c.dead.reset (0.0);
    }
}

// Everything the slow controls feed, worked out again only while one of them is moving.
void PluginProcessor::updateCoefs() noexcept
{
    const double pos = (double) speedSm.getCurrentValue();
    const double a = (double) ageSm.getCurrentValue();
    const double b = (double) biasSm.getCurrentValue();

    const double gapHz = lookupLog (kGapHz, pos) * overBiasGapScale (b);
    const double ageHz = ageLowpassHz (a);
    const double spaceHz = spacingHz ((double) spacingSm.getCurrentValue(), pos);
    spacingC = spaceHz <= 0.0 ? 1.0 : onePoleCoef (spaceHz, osRate);
    emphasis.set (lookupLog (kEmphHz, pos), kEmphRatio, osRate);
    gapCoefs.setLowOrHigh (gapHz, osRate);
    ageLpCoefs.setLowOrHigh (ageHz, osRate);
    // The short delay that lines the wet path up takes off what these filters have now, down to nothing.
    trimLinedUp = std::max (0.0, trimRoom - toneDelay (gapHz, ageHz, spacingC, osRate));

    const double bumpHz = lookupLog (kBumpHz, pos);
    bumpCoefs.setBell (bumpHz, sr, lookupLinear (kBumpDb, pos), kBumpQ);
    dipCoefs.setBell (bumpHz * kDipRatio, sr, kDipDb, kDipQ);

    ageHpC = onePoleCoef (ageHighpassHz (a), sr);
    wanderTarget = ageWanderDb (a);

    // The same losses for Auto Level's ear, at the session rate. Two equal one pole filters are one two pole filter with a Q of 0.5.
    detectGapOn = gapHz < kDetectToneHz;
    detectAgeOn = ageHz < kDetectToneHz;
    detectSpaceOn = spaceHz > 0.0 && spaceHz < kDetectToneHz;
    if (detectGapOn)
        detectGapCoefs.setMatchedLow (gapHz, sr);
    if (detectAgeOn)
        detectAgeCoefs.setMatchedLow (ageHz, sr);
    if (detectSpaceOn)
        detectSpaceCoefs.setMatchedLow (spaceHz, sr, 0.5);

    // Hiss is dulled by the same losses as the signal. Its gain follows from the filter, so its level holds at every setting.
    noiseLpC = onePoleCoef (spaceHz <= 0.0 ? std::min (gapHz, ageHz) : std::min ({ gapHz, ageHz, spaceHz }), sr);
    noiseNorm = std::sqrt (3.0 * (2.0 - noiseLpC) / noiseLpC);

    lowCutCoefs.setLowOrHigh ((double) lowCutSm.getCurrentValue(), sr);
    highCutCoefs.setLowOrHigh ((double) highCutSm.getCurrentValue(), sr);       // the old response: falls faster near the top of the band
    highCutMatchedCoefs.setMatchedLow ((double) highCutSm.getCurrentValue(), sr); // 12 dB per octave all the way up
}

double PluginProcessor::curveOffset() const noexcept
{
    return biasOffset ((double) biasSm.getCurrentValue(), (double) driveSm.getCurrentValue(), (double) ageSm.getCurrentValue());
}

void PluginProcessor::snapControls() noexcept
{
    const float a = age->load() * 0.01f;
    const float low = lowCut->load(), high = highCut->load();
    driveSm.setCurrentAndTargetValue (drive->load());
    biasSm.setCurrentAndTargetValue (bias->load() * 0.01f);
    speedSm.setCurrentAndTargetValue (speed->load());
    ageSm.setCurrentAndTargetValue (a);
    wowSm.setCurrentAndTargetValue (wow->load() * 0.01f + (float) ageWowAdd (a));
    flutterSm.setCurrentAndTargetValue (flutter->load() * 0.01f + (float) ageFlutterAdd (a));
    noiseSm.setCurrentAndTargetValue ((float) std::sqrt (juce::square (noiseRms (noise->load())) + juce::square (ageNoiseRms (a))));
    mixSm.setCurrentAndTargetValue (mix->load() * 0.01f);
    depthSm.setCurrentAndTargetValue (mix->load() * 0.01f);
    outputSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (output->load()));
    autoSm.setCurrentAndTargetValue (autoLevel->get() ? 1.0f : 0.0f);
    onSm.setCurrentAndTargetValue (bypass->get() ? 0.0f : 1.0f);
    lowOnSm.setCurrentAndTargetValue (low <= kLowCutOff + 0.01f ? 0.0f : 1.0f);
    highOnSm.setCurrentAndTargetValue (high >= kHighCutOff - 0.5f ? 0.0f : 1.0f);
    lowCutSm.setCurrentAndTargetValue (low);
    highCutSm.setCurrentAndTargetValue (high);

    hardSm.setCurrentAndTargetValue (hardness->load() * 0.01f);
    spacingSm.setCurrentAndTargetValue (spacing->load() * 0.01f);
    azimuthSm.setCurrentAndTargetValue (tiltAmount (azimuth->load()));
    randomSm.setCurrentAndTargetValue (randomDrift->load() * 0.01f);
    stereoSm.setCurrentAndTargetValue (stereoDrift->load() * 0.01f);
    dropSm.setCurrentAndTargetValue (dropDepth->load() * 0.01f);
    encodeSm.setCurrentAndTargetValue (encodeAmt->load() * 0.01f);
    decodeSm.setCurrentAndTargetValue (decodeAmt->load() * 0.01f);
    textureSm.setCurrentAndTargetValue ((float) textureAmount (texture->load() * 0.01));
    deviceMixSm.setCurrentAndTargetValue (deviceMix->load() * 0.01f);
    oldBlendSm.setCurrentAndTargetValue (wantsOldBlend() ? 1.0f : 0.0f);
    oldToneSm.setCurrentAndTargetValue (oldResponse.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
    companderOld = oldResponse.load (std::memory_order_relaxed);
    activeDevice = (int) device->load();
    deviceFadeSm.setCurrentAndTargetValue (activeDevice > 0 ? 1.0f : 0.0f);

    updateCoefs();
    trimNow = trimFor ((double) oldBlendSm.getCurrentValue());
    wanderDb = wanderTarget;
    lastOffset = curveOffset();
    lastTanh = std::tanh (lastOffset);
    lastNorm = juce::square (std::cosh (lastOffset));
    lastDead = deadAmount ((double) biasSm.getCurrentValue());
    lastHeadroomDb = hardnessHeadroomDb ((double) hardSm.getCurrentValue()) - ageHeadroomDb ((double) ageSm.getCurrentValue());
    lastHeadroom = kHeadroom * std::pow (10.0, lastHeadroomDb / 20.0);
    coefsDirty = false;
}

void PluginProcessor::pushTargets (bool forceBypass) noexcept
{
    const float a = age->load() * 0.01f;
    const float low = lowCut->load(), high = highCut->load();
    driveSm.setTargetValue (drive->load());
    biasSm.setTargetValue (bias->load() * 0.01f);
    speedSm.setTargetValue (speed->load());
    ageSm.setTargetValue (a);
    wowSm.setTargetValue (wow->load() * 0.01f + (float) ageWowAdd (a));
    flutterSm.setTargetValue (flutter->load() * 0.01f + (float) ageFlutterAdd (a));
    noiseSm.setTargetValue ((float) std::sqrt (juce::square (noiseRms (noise->load())) + juce::square (ageNoiseRms (a))));
    mixSm.setTargetValue (mix->load() * 0.01f);
    depthSm.setTargetValue (mix->load() * 0.01f);
    outputSm.setTargetValue (juce::Decibels::decibelsToGain (output->load()));
    autoSm.setTargetValue (autoLevel->get() ? 1.0f : 0.0f);
    onSm.setTargetValue (forceBypass || bypass->get() ? 0.0f : 1.0f);
    lowOnSm.setTargetValue (low <= kLowCutOff + 0.01f ? 0.0f : 1.0f);
    highOnSm.setTargetValue (high >= kHighCutOff - 0.5f ? 0.0f : 1.0f);
    lowCutSm.setTargetValue (low);
    highCutSm.setTargetValue (high);
    hardSm.setTargetValue (hardness->load() * 0.01f);
    spacingSm.setTargetValue (spacing->load() * 0.01f);
    azimuthSm.setTargetValue (tiltAmount (azimuth->load()));
    randomSm.setTargetValue (randomDrift->load() * 0.01f);
    stereoSm.setTargetValue (stereoDrift->load() * 0.01f);
    dropSm.setTargetValue (dropDepth->load() * 0.01f);
    encodeSm.setTargetValue (encodeAmt->load() * 0.01f);
    decodeSm.setTargetValue (decodeAmt->load() * 0.01f);
    textureSm.setTargetValue ((float) textureAmount (texture->load() * 0.01));
    deviceMixSm.setTargetValue (deviceMix->load() * 0.01f);
    oldBlendSm.setTargetValue (wantsOldBlend() ? 1.0f : 0.0f);
    oldToneSm.setTargetValue (oldResponse.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
}

// The old response only moves the wet path while there is dry signal to blend with. At Mix 100 the wet path stays on the reported latency.
bool PluginProcessor::wantsOldBlend() const noexcept
{
    return oldResponse.load (std::memory_order_relaxed) && mix->load() < 99.95f;
}

bool PluginProcessor::responseMatters() const noexcept
{
    if (mix->load() < 99.95f || highCut->load() < kHighCutOff - 0.5f || (int) compander->load() != compOff)
        return true;
    // Auto Level with a top end loss in reach of its detector
    if (! autoLevel->get())
        return false;
    const double pos = (double) speed->load(), space = spacingHz ((double) spacing->load() * 0.01, pos);
    return lookupLog (kGapHz, pos) * overBiasGapScale ((double) bias->load() * 0.01) < kDetectToneHz
        || ageLowpassHz ((double) age->load() * 0.01) < kDetectToneHz
        || (space > 0.0 && space < kDetectToneHz);
}

bool PluginProcessor::statesAreFinite() const noexcept
{
    for (int ch = 0; ch < preparedChannels; ++ch)
    {
        const auto& c = chans[(size_t) ch];
        if (! (c.gap.isFinite() && c.ageLp.isFinite() && c.bump.isFinite() && c.dip.isFinite() && c.lowCutF.isFinite() && c.highCutF.isFinite()
               && c.highCutMatched.isFinite() && std::isfinite (c.allPassOut) && c.detectIn.isFinite() && c.detectOut.isFinite()
               && c.detectGap.isFinite() && c.detectAge.isFinite() && c.detectSpace.isFinite()
               && std::isfinite (c.pre.y1) && std::isfinite (c.de.y1) && std::isfinite (c.dc.lp) && std::isfinite (c.ageHp.lp) && std::isfinite (c.noiseLp.lp)))
            return false;
    }
    return std::isfinite (envIn) && std::isfinite (envOut) && std::isfinite (ratio);
}

// The oversampled path. Both channels run side by side in the two lanes of one register. With one channel the second lane just repeats the first.
void PluginProcessor::runHighRate (float* left, float* right, int numChannels, int total, double offset1, double tanh1, double norm1, double dead1, double head1, double trimTarget) noexcept
{
    auto& cL = chans[0];
    auto& cR = chans[(size_t) numChannels - 1];
    Lanes preX { cL.pre.x1, cR.pre.x1 }, preY { cL.pre.y1, cR.pre.y1 }, deX { cL.de.x1, cR.de.x1 }, deY { cL.de.y1, cR.de.y1 };
    Lanes gap1 { cL.gap.ic1, cR.gap.ic1 }, gap2 { cL.gap.ic2, cR.gap.ic2 }, age1 { cL.ageLp.ic1, cR.ageLp.ic1 }, age2 { cL.ageLp.ic2, cR.ageLp.ic2 };
    Lanes space1 { cL.spacing1.lp, cR.spacing1.lp }, space2 { cL.spacing2.lp, cR.spacing2.lp };
    Lanes mainIn { cL.main.lastIn, cR.main.lastIn }, mainF { cL.main.lastF, cR.main.lastF }, deadIn { cL.dead.lastIn, cR.dead.lastIn }, deadF { cL.dead.lastF, cR.dead.lastF };

    const double preB0 = emphasis.b0, preB1 = emphasis.b1, preA1 = emphasis.a1, deB0 = emphasis.ib0, deB1 = emphasis.ib1, deA1 = emphasis.ia1;
    const double gapA1 = gapCoefs.a1, gapA2 = gapCoefs.a2, gapA3 = gapCoefs.a3, ageA1 = ageLpCoefs.a1, ageA2 = ageLpCoefs.a2, ageA3 = ageLpCoefs.a3;
    const double spaceC = spacingC;
    // With no ramp every setting holds for the whole piece. A curve at 0 needs no working out, and neither do Head Spacing's poles at 0.
    const bool ramp = offset1 != lastOffset || tanh1 != lastTanh || norm1 != lastNorm || dead1 != lastDead || head1 != lastHeadroom;
    const bool useDead = dead1 != 0.0 || lastDead != 0.0;
    const bool useSpacing = spaceC != 1.0;
    const double step = 1.0 / (double) total;
    double offset = offset1, headroom = head1, curveTanh = tanh1, curveNorm = norm1, deadNow = dead1;
    double headroomInverse = 1.0 / head1;
    Lanes v { 0.0, 0.0 }, lost { 0.0, 0.0 };
    HeldCurve heldMain { { offset1, offset1 }, { tanh1, tanh1 } }, heldDead {};
    // The short delay that lands the wet path on a whole sample. It only moves while the old response fades in or out.
    const bool trimMoves = trimTarget != trimNow;
    const double trimStep = (trimTarget - trimNow) * step;
    const int trimMask = trimSize - 1;
    double trimAmount = trimNow, taps[4];
    int trimFrom = 0, trimAt = trimPos;
    trimTaps (trimAmount, trimFrom, taps);

    for (int i = 0; i < total; ++i)
    {
        if (ramp)
        {
            const double f = (double) (i + 1) * step;
            offset = lastOffset + f * (offset1 - lastOffset);
            headroom = lastHeadroom + f * (head1 - lastHeadroom);
            headroomInverse = 1.0 / headroom;
            curveTanh = lastTanh + f * (tanh1 - lastTanh);
            curveNorm = lastNorm + f * (norm1 - lastNorm);
            deadNow = lastDead + f * (dead1 - lastDead);
        }

        const Lanes x { (double) left[i], (double) right[i] };
        const Lanes boosted = preB0 * x + preB1 * preX - preA1 * preY;
        preX = x;
        preY = boosted;
        v = boosted * headroomInverse;

        // The curve, as an average between this sample and the last. Close together, that average is the curve at their middle.
        const Lanes u = v + offset;
        const Lanes F = fastAntiderivative (u);
        const Lanes d = u - mainIn;
        Lanes curve = (F - mainF) / d;
        const LaneMask close = __builtin_elementwise_abs (d) < 1.0e-5;
        if ((close[0] | close[1]) != 0)
            curve = closeCurve (curve, close, u, d, mainIn, heldMain);
        mainIn = u;
        mainF = F;

        Lanes shaped = (curve - curveTanh) * curveNorm;
        if (useDead)
        {
            const Lanes w = v * (1.0 / kDeadWidth);
            const Lanes Fd = fastAntiderivative (w);
            const Lanes dd = w - deadIn;
            Lanes curveD = (Fd - deadF) / dd;
            const LaneMask closeD = __builtin_elementwise_abs (dd) < 1.0e-5;
            if ((closeD[0] | closeD[1]) != 0)
                curveD = closeCurve (curveD, closeD, w, dd, deadIn, heldDead);
            deadIn = w;
            deadF = Fd;
            shaped -= (deadNow * kDeadWidth) * curveD;
        }

        const Lanes in = shaped * headroom;
        const Lanes back = deB0 * in + deB1 * deX - deA1 * deY;
        deX = in;
        deY = back;

        // Top end losses: the gap loss, then the age loss.
        const Lanes gapV3 = back - gap2;
        const Lanes gapV1 = gapA1 * gap1 + gapA2 * gapV3;
        const Lanes gapV2 = gap2 + gapA2 * gap1 + gapA3 * gapV3;
        gap1 = 2.0 * gapV1 - gap1;
        gap2 = 2.0 * gapV2 - gap2;

        const Lanes ageV3 = gapV2 - age2;
        const Lanes ageV1 = ageA1 * age1 + ageA2 * ageV3;
        const Lanes ageV2 = age2 + ageA2 * age1 + ageA3 * ageV3;
        age1 = 2.0 * ageV1 - age1;
        age2 = 2.0 * ageV2 - age2;
        lost = ageV2;

        Lanes out = lost;
        if (useSpacing)
        {
            space1 += spaceC * (lost - space1);
            space2 += spaceC * (space1 - space2);
            out = space2;
        }

        if (trimMoves)
        {
            trimAmount += trimStep;
            trimTaps (trimAmount, trimFrom, taps);
        }
        trimLine[(size_t) trimAt] = out;
        const Lanes late = taps[0] * trimLine[(size_t) ((trimAt - trimFrom) & trimMask)] + taps[1] * trimLine[(size_t) ((trimAt - trimFrom - 1) & trimMask)]
                         + taps[2] * trimLine[(size_t) ((trimAt - trimFrom - 2) & trimMask)] + taps[3] * trimLine[(size_t) ((trimAt - trimFrom - 3) & trimMask)];
        trimAt = (trimAt + 1) & trimMask;
        right[i] = (float) late[1];
        left[i] = (float) late[0];
    }
    trimPos = trimAt;
    trimNow = trimTarget;

    if (! useSpacing)
        space1 = space2 = lost;

    for (int lane = numChannels - 1; lane >= 0; --lane)
    {
        auto& c = chans[(size_t) lane];
        c.pre.x1 = preX[lane]; c.pre.y1 = preY[lane];
        c.de.x1 = deX[lane]; c.de.y1 = deY[lane];
        c.gap.ic1 = gap1[lane]; c.gap.ic2 = gap2[lane];
        c.ageLp.ic1 = age1[lane]; c.ageLp.ic2 = age2[lane];
        c.spacing1.lp = space1[lane]; c.spacing2.lp = space2[lane];
        c.main.lastIn = mainIn[lane]; c.main.lastF = mainF[lane];
        if (useDead)
        {
            c.dead.lastIn = deadIn[lane]; c.dead.lastF = deadF[lane];
        }
        else
        {
            c.dead.reset (v[lane] / kDeadWidth); // ready for the moment it fades in
        }
    }
}

void PluginProcessor::processChunk (float* const* channels, int numChannels, int n) noexcept
{
    // 1. Slow controls for this piece. Filters only recalculate while a control is moving.
    const bool moving = biasSm.isSmoothing() || speedSm.isSmoothing() || ageSm.isSmoothing() || lowCutSm.isSmoothing() || highCutSm.isSmoothing() || spacingSm.isSmoothing();
    biasSm.skip (n);
    speedSm.skip (n);
    ageSm.skip (n);
    lowCutSm.skip (n);
    highCutSm.skip (n);
    hardSm.skip (n);
    spacingSm.skip (n);
    if (moving || coefsDirty)
    {
        updateCoefs();
        coefsDirty = false;
    }

    // A new Device fades out, swaps while silent, and fades back in, so its filter never jumps.
    {
        const int wanted = (int) device->load();
        if (wanted != activeDevice)
        {
            deviceFadeSm.setTargetValue (0.0f);
            if (deviceFadeSm.getCurrentValue() <= 0.0f)
            {
                activeDevice = wanted;
                for (auto& c : chans)
                    std::fill (c.deviceLine.begin(), c.deviceLine.end(), 0.0f);
                devicePos = 0;
            }
        }
        if (wanted == activeDevice)
            deviceFadeSm.setTargetValue (activeDevice > 0 ? 1.0f : 0.0f);
    }

    // The compander's boost moves from one piece to the next. Each side's shelf is set from what its own side chain heard.
    const int compMode = (int) compander->load();
    const double encodeNow = (double) encodeSm.skip (n), decodeNow = (double) decodeSm.skip (n);
    if (const bool old = oldResponse.load (std::memory_order_relaxed); old != companderOld)
    {
        companderOld = old;
        for (auto& c : chans)
            c.encoder.coefsSet = c.decoder.coefsSet = false;
    }
    const double compSlew = juce::jmin (1.0, companderSlewPerSample * (double) n);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& c = chans[(size_t) ch];
        const auto update = [&] (CompanderSide& side, double amount)
        {
            const double target = compMode == compOff ? 0.0 : companderBoostDb (compMode, 10.0 * std::log10 (side.env + 1.0e-12) + 3.0);
            side.boostDb += compSlew * (target - side.boostDb);
            if (side.boostDb < 1.0e-4 && compMode == compOff)
                side.boostDb = 0.0;
            const double db = side.boostDb * amount;
            if (! side.coefsSet || db != side.setFor)
            {
                const double g = std::pow (10.0, db / 20.0);
                if (companderOld)
                    side.coefs.set (kCompanderHz / std::sqrt (g), g, sr);
                else
                    setMatchedShelf (side.coefs, kCompanderHz / std::sqrt (g), g, sr);
                side.setFor = db;
                side.coefsSet = true;
            }
        };
        update (c.encoder, encodeNow);
        update (c.decoder, decodeNow);
    }

    // 2. The dry tap for Mix, the dry tap for Bypass, the encoded signal, and the driven signal, with Drive's gain kept for later.
    {
        float* dryNearOut[2] = { dryNear.getWritePointer (0), dryNear.getWritePointer (numChannels - 1) };
        float* dryFarOut[2] = { dryFar.getWritePointer (0), dryFar.getWritePointer (numChannels - 1) };
        float* encodedOut[2] = { encodedNear.getWritePointer (0), encodedNear.getWritePointer (numChannels - 1) };
        float* workOut[2] = { work.getWritePointer (0), work.getWritePointer (numChannels - 1) };
        for (int i = 0; i < n; ++i)
        {
            const float driveDb = driveSm.getNextValue();
            if (! driveGainKnown || driveDb != lastDriveDb)
            {
                lastDriveGain = juce::Decibels::decibelsToGain (driveDb);
                lastDriveDb = driveDb;
                driveGainKnown = true;
            }
            const float g = lastDriveGain;
            const int nearPos = (dryPos - osLatency + dryRingSize) % dryRingSize;
            const int farPos = dryPos == dryRingSize - 1 ? 0 : dryPos + 1;
            const int encodedRead = gainPos == gainRingSize - 1 ? 0 : gainPos + 1;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = chans[(size_t) ch];
                const float x = channels[ch][i];
                c.dryRing[(size_t) dryPos] = x;
                dryNearOut[ch][i] = c.dryRing[(size_t) nearPos];
                dryFarOut[ch][i] = c.dryRing[(size_t) farPos];

                auto& e = c.encoder;
                const double side = e.sideHp.high ((double) x, sideHpC);
                const double power = side * side;
                e.env += (power > e.env ? sideAttackC : sideReleaseC) * (power - e.env);
                const auto encoded = (float) e.filter.tick ((double) x, e.coefs.b0, e.coefs.b1, e.coefs.a1);
                c.encodedRing[(size_t) gainPos] = encoded;
                encodedOut[ch][i] = c.encodedRing[(size_t) encodedRead];
                workOut[ch][i] = encoded * g;
            }
            dryPos = farPos;
            gainRing[(size_t) gainPos] = g;
            gainPos = encodedRead;
            gainDelayed[(size_t) i] = gainRing[(size_t) gainPos];
        }
    }

    // 3. At the high rate: record boost, curve, the boost taken back out, top end losses. The curve's settings ramp across the piece.
    const double offset1 = curveOffset();
    const bool sameOffset = offset1 == lastOffset;
    const double tanh1 = sameOffset ? lastTanh : std::tanh (offset1);
    const double norm1 = sameOffset ? lastNorm : juce::square (std::cosh (offset1));
    const double dead1 = deadAmount ((double) biasSm.getCurrentValue());
    const double headroomDb = hardnessHeadroomDb ((double) hardSm.getCurrentValue()) - ageHeadroomDb ((double) ageSm.getCurrentValue());
    const double head1 = headroomDb == lastHeadroomDb ? lastHeadroom : kHeadroom * std::pow (10.0, headroomDb / 20.0);
    lastHeadroomDb = headroomDb;

    juce::dsp::AudioBlock<float> workBlock (work);
    auto high = oversampler->processSamplesUp (workBlock.getSubBlock (0, (size_t) n));
    const double oldBlendFrom = (double) oldBlendSm.getCurrentValue(), oldBlendTo = (double) oldBlendSm.skip (n);
    runHighRate (high.getChannelPointer (0), high.getChannelPointer ((size_t) numChannels - 1), numChannels, n << osStages, offset1, tanh1, norm1, dead1, head1, trimFor (oldBlendTo));
    lastOffset = offset1;
    lastTanh = tanh1;
    lastNorm = norm1;
    lastDead = dead1;
    lastHeadroom = head1;
    auto lowBlock = workBlock.getSubBlock (0, (size_t) n);
    oversampler->processSamplesDown (lowBlock);

    // 4. Back at the session rate.
    const double chanScale = 1.0 / (double) numChannels;
    const double delaySamplesPerMs = sr * 0.001;
    const double wanderStep = (wanderTarget - wanderDb) / (double) n;
    const auto& tableRef = sinc.h;

    for (int i = 0; i < n; ++i)
    {
        // Head bump, low end loss from Age, and the level detector.
        const double g = (double) gainDelayed[(size_t) i];
        const float oldBlend = (float) (oldBlendFrom + (oldBlendTo - oldBlendFrom) * (double) (i + 1) / (double) n);
        double wet[2] = { 0.0, 0.0 };
        double powerIn = 0.0, powerOut = 0.0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& c = chans[(size_t) ch];
            // The old response's all pass, on the wet path only while it is blended with dry.
            float fresh = work.getReadPointer (ch)[i];
            if (oldAllPassUsed)
            {
                const float passed = c.allPassIn + oldAllPass * (fresh - c.allPassOut);
                c.allPassIn = fresh;
                c.allPassOut = passed;
                if (oldBlend >= 1.0f)
                    fresh = passed;
                else if (oldBlend > 0.0f)
                    fresh += oldBlend * (passed - fresh);
            }
            double v = c.dc.high ((double) fresh, dcC);
            v = c.dip.bell (c.bump.bell (v, bumpCoefs), dipCoefs);
            v = c.ageHp.high (v, ageHpC);
            wet[ch] = v;
            // Auto Level compares the dry signal with the tape. Up to 0.3.1 the dry side had none of the tape's top end losses, so on sound
            // that is mostly top end (an S through Age, slow tape or Head Spacing) it read the loss as saturation and turned it back up.
            double heard = (double) encodedNear.getReadPointer (ch)[i];
            if (! companderOld)
            {
                if (detectGapOn)   heard = c.detectGap.mixed (heard, detectGapCoefs);
                if (detectAgeOn)   heard = c.detectAge.mixed (heard, detectAgeCoefs);
                if (detectSpaceOn) heard = c.detectSpace.mixed (heard, detectSpaceCoefs);
            }
            const double a = c.detectIn.bell (c.detectInHp.high (heard, detectHpC), detectCoefs);
            const double b = c.detectOut.bell (c.detectOutHp.high (v / g, detectHpC), detectCoefs);
            powerIn += a * a;
            powerOut += b * b;
        }

        // Auto level compares the encoded dry signal to the wet signal with Drive taken back out, so it only learns the saturation loss.
        // Dropping 1/Drive straight away and only learning the saturation loss keeps Drive moves from jumping in level.
        envIn += envC * (powerIn * chanScale - envIn);
        envOut += envC * (powerOut * chanScale - envOut);
        const double target = juce::jlimit (0.5, 40.0, std::sqrt ((envIn + kEps) / (envOut + kEps)));
        ratio += ratioC * (target - ratio);
        const double am = (double) autoSm.getNextValue();
        const double comp = (1.0 - am) + am * ratio / g;

        // Age: a slow, irregular level drift, shared by both channels.
        wanderDb += wanderStep;
        phaseWander1 += incWander1; if (phaseWander1 >= kTwoPi) phaseWander1 -= kTwoPi;
        phaseWander2 += incWander2; if (phaseWander2 >= kTwoPi) phaseWander2 -= kTwoPi;
        phaseWander3 += incWander3; if (phaseWander3 >= kTwoPi) phaseWander3 -= kTwoPi;
        double wander = 1.0;
        if (wanderDb > 0.0)
            wander = std::pow (10.0, wanderDb * (0.5 * std::sin (phaseWander1) + 0.3 * std::sin (phaseWander2) + 0.2 * std::sin (phaseWander3)) / 20.0);

        // Wow, Flutter and Random Drift: a read head position per channel, the same on both unless Stereo Drift or Head Azimuth pull them apart.
        // Depth follows Mix, because dry and wet go through the head together.
        const double wowAmt = (double) wowSm.getNextValue(), flutterAmt = (double) flutterSm.getNextValue(), randomAmt = (double) randomSm.getNextValue();
        phaseWow += incWow; if (phaseWow >= kTwoPi) phaseWow -= kTwoPi;
        phaseWow2 += incWow2; if (phaseWow2 >= kTwoPi) phaseWow2 -= kTwoPi;
        phaseFlutter += incFlutter; if (phaseFlutter >= kTwoPi) phaseFlutter -= kTwoPi;
        phaseFlutter2 += incFlutter2; if (phaseFlutter2 >= kTwoPi) phaseFlutter2 -= kTwoPi;
        flutterRandom.advance();
        flutterRandomR.advance();
        slowRandom.advance();
        slowRandomR.advance();
        const double depth = (double) depthSm.getNextValue();
        const double stereoAmt = (double) stereoSm.getNextValue();
        const double tilt = (double) azimuthSm.getNextValue();

        double swingMs = 0.0;
        if (wowAmt > 0.0)
            swingMs += wowAmt * (wowMs * std::sin (phaseWow) + kWow2Ms * std::sin (phaseWow2));
        if (flutterAmt > 0.0)
            swingMs += flutterAmt * (flutterMs * std::sin (phaseFlutter) + kFlutter2Ms * std::sin (phaseFlutter2) + flutterRandom.value() * kFlutterRandomMs);
        if (randomAmt > 0.0)
            swingMs += randomAmt * slowRandom.value() * kRandomRmsMs;

        double swingRightMs = swingMs;
        if (stereoAmt > 0.0)
        {
            double other = 0.0;
            if (wowAmt > 0.0)
                other += wowAmt * (wowMs * std::cos (phaseWow) + kWow2Ms * std::cos (phaseWow2));
            if (flutterAmt > 0.0)
                other += flutterAmt * (flutterMs * std::cos (phaseFlutter) + kFlutter2Ms * std::cos (phaseFlutter2) + flutterRandomR.value() * kFlutterRandomMs);
            if (randomAmt > 0.0)
                other += randomAmt * slowRandomR.value() * kRandomRmsMs;
            swingRightMs += stereoAmt * (other - swingMs);
        }

        const double tiltMs = numChannels > 1 && tilt != 0.0 ? 0.5 * azimuthMs (tilt, (double) speedSm.getCurrentValue()) : 0.0;
        lastSwingMs = swingMs * depth;
        double readDelays[2];
        readDelays[0] = (double) modCentre + (juce::jlimit (-kModLimitMs, kModLimitMs, swingMs * depth) - tiltMs) * delaySamplesPerMs;
        readDelays[1] = (double) modCentre + (juce::jlimit (-kModLimitMs, kModLimitMs, swingRightMs * depth) + tiltMs) * delaySamplesPerMs;

        // Dropouts: thin spots on the tape, shared by both channels.
        const double dropAmt = (double) dropSm.getNextValue();
        double dropGain = 1.0;
        if (dropAmt > 0.0)
        {
            const double chance = (double) dropRate->load() / sr;
            if (! dropActive && 0.5 * (dropRng.bipolar() + 1.0) < chance)
            {
                dropActive = true;
                dropSeverity = 0.7 + 0.3 * dropRng.bipolar();
                dropHold = (int) ((0.0475 + 0.0325 * dropRng.bipolar()) * sr);
            }
            if (dropActive && --dropHold <= 0)
                dropActive = false;
            // The envelope heads for this dropout's own depth. Up to 0.3.1 the depth multiplied an envelope that headed for 1, so a dropout
            // that began before the last one had recovered jumped to its new depth in one sample: a click. One dropout alone is unchanged.
            dropEnv += (dropActive ? dropDownC : dropUpC) * ((dropActive ? dropSeverity : 0.0) - dropEnv);
            dropGain = std::pow (10.0, -kDropoutMaxDb * dropAmt * dropEnv / 20.0);
        }

        const double grainAmt = (double) textureSm.getNextValue();
        const double encodeSide = (1.0 - am) * g / ratio + am;

        const double noiseAmount = (double) noiseSm.getNextValue();
        const double m = (double) mixSm.getNextValue();
        const double on = (double) onSm.getNextValue();
        const double outGain = (double) outputSm.getNextValue();
        const double lowOn = (double) lowOnSm.getNextValue(), highOn = (double) highOnSm.getNextValue();
        const double deviceAmt = (double) deviceMixSm.getNextValue() * (double) deviceFadeSm.getNextValue();
        const double oldTone = (double) oldToneSm.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& c = chans[(size_t) ch];
            double v = wet[ch] * comp * wander * dropGain;

            // Texture: grain that follows the level of the signal, so silence stays silent.
            if (grainAmt > 0.0)
            {
                c.grainEnv += grainEnvC * (std::abs (v) - c.grainEnv);
                v += grainAmt * c.grainEnv * grainNorm * c.grainHp.high (c.grainLp.low (c.grainRng.bipolar(), grainLpC), grainHpC);
            }
            if (noiseAmount > 0.0)
                v += noiseAmount * noiseNorm * c.noiseHp.high (c.noiseLp.low (c.rng.bipolar(), noiseLpC), noiseHpC);

            // The compander's decode comes after the hiss, so it turns the hiss down. Its side chain hears its own output with Drive's gain taken out.
            auto& d = c.decoder;
            v = d.filter.tick (v, d.coefs.ib0, d.coefs.ib1, d.coefs.ia1);
            const double side = d.sideHp.high (v / encodeSide, sideHpC);
            const double power = side * side;
            d.env += (power > d.env ? sideAttackC : sideReleaseC) * (power - d.env);

            v += lowOn * (c.lowCutF.high (v, lowCutCoefs) - v);
            const double cutOld = c.highCutF.low (v, highCutCoefs), cutMatched = c.highCutMatched.mixed (v, highCutMatchedCoefs);
            v += highOn * ((cutMatched + oldTone * (cutOld - cutMatched)) - v);

            // Device: direct convolution with a short filter. Only runs while one is selected.
            if (activeDevice > 0)
            {
                auto* hist = c.deviceLine.data();
                hist[devicePos] = hist[devicePos + deviceLength] = (float) v;
                const double sum = weightedSum (deviceIrs[(size_t) (activeDevice - 1)].data(), hist + devicePos + 1, deviceLength);
                if (deviceAmt > 0.0)
                    v += deviceAmt * (sum - v);
            }

            const double dry = (double) dryNear.getReadPointer (ch)[i];
            auto* line = c.line.data();
            line[linePos] = (float) (dry + m * (v - dry));

            const double readDelay = readDelays[ch];
            const int whole = (int) std::floor (readDelay);
            const double fraction = readDelay - (double) whole;
            double read = 0.0;
            if (fraction > 0.0)
            {
                const double scaled = fraction * (double) SincTable::phases;
                const int phase = juce::jmin ((int) scaled, SincTable::phases - 1);
                const float blend = (float) (scaled - (double) phase);
                const float* h0 = tableRef.data() + phase * SincTable::taps;
                const float* h1 = h0 + SincTable::taps;
                for (int k = 0; k < SincTable::taps; ++k)
                    read += (double) (h0[k] + blend * (h1[k] - h0[k])) * (double) line[(linePos - whole - k + 7) & lineMask];
            }
            else
            {
                read = (double) line[(linePos - whole) & lineMask];
            }

            const double far = (double) dryFar.getReadPointer (ch)[i];
            channels[ch][i] = (float) (far + on * (read * outGain - far));
        }
        linePos = (linePos + 1) & lineMask;
        devicePos = devicePos == deviceLength - 1 ? 0 : devicePos + 1;
    }
    wanderDb = wanderTarget;

    // Readings for the window.
    readingAutoLevelDb.store ((float) juce::Decibels::gainToDecibels (ratio), std::memory_order_relaxed);
    readingHeadMs.store ((float) (lastSwingMs), std::memory_order_relaxed);
    readingCompanderDb.store ((float) chans[0].encoder.boostDb, std::memory_order_relaxed);

    if (! statesAreFinite())
        reset();
}

void PluginProcessor::processInternal (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    sanitiseInput (buffer);
    if (oversampler == nullptr)
        return;

    pushTargets (forceBypass);

    // Only a copy for the window. The sound never reads these back.
    const bool tapping = tapWanted.load (std::memory_order_relaxed);
    const int tapChannels = juce::jmin (buffer.getNumChannels(), preparedChannels);
    if (tapping && tapChannels > 0)
    {
        writeTap (tapIn, buffer, tapChannels);
        float peak = tapPeak.load (std::memory_order_relaxed);
        for (int ch = 0; ch < tapChannels; ++ch)
            peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        tapPeak.store (peak, std::memory_order_relaxed);
    }

    readingInputPeak.store (buffer.getNumChannels() > 0 ? buffer.getMagnitude (0, 0, buffer.getNumSamples()) : 0.0f, std::memory_order_relaxed);

    const int numChannels = juce::jmin (buffer.getNumChannels(), preparedChannels);
    if (numChannels <= 0)
        return;

    float* const* data = buffer.getArrayOfWritePointers();
    float* ptrs[2] = { nullptr, nullptr };
    const int total = buffer.getNumSamples();
    for (int pos = 0; pos < total; pos += kChunk)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            ptrs[ch] = data[ch] + pos;
        processChunk (ptrs, numChannels, juce::jmin (kChunk, total - pos));
    }
    readingOutputPeak.store (buffer.getMagnitude (0, 0, total), std::memory_order_relaxed);

    if (tapping)
    {
        writeTap (tapOut, buffer, numChannels);
        tapWritten.fetch_add ((std::uint32_t) total, std::memory_order_release);
    }
}

void PluginProcessor::writeTap (std::array<std::atomic<float>, tapSize>& tap, const juce::AudioBuffer<float>& buffer, int numChannels) noexcept
{
    const std::uint32_t at = tapWritten.load (std::memory_order_relaxed);
    const float* left = buffer.getReadPointer (0);
    const float* right = buffer.getReadPointer (numChannels - 1);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        tap[(at + (std::uint32_t) i) & (tapSize - 1)].store (0.5f * (left[i] + right[i]), std::memory_order_relaxed);
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processInternal (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processInternal (buffer, true); // same path, so the delay stays the same and the switch fades
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
    const auto& v = presets[i];
    set ("drive", v.drive);
    set ("bias", v.bias);
    set ("speed", (float) v.speed);
    set ("wow", v.wowAmt);
    set ("flutter", v.flutterAmt);
    set ("noise", v.noiseAmt);
    set ("age", v.ageAmt);
    set ("lowcut", v.low);
    set ("highcut", v.high);
    set ("mix", v.mixAmt);
    set ("output", v.out);
    set ("autolevel", v.autoOn ? 1.0f : 0.0f);
    set ("hardness", v.hard);
    set ("spacing", v.space);
    set ("azimuth", v.tilt);
    set ("random", v.rnd);
    set ("stereodrift", v.stereo);
    set ("dropdepth", v.dropD);
    set ("droprate", v.dropR);
    set ("compander", (float) v.comp);
    set ("encode", v.enc);
    set ("decode", v.dec);
    set ("texture", v.tex);
    set ("device", (float) v.dev);
    set ("devicemix", v.devMix);
    oldResponse.store (v.old, std::memory_order_relaxed);
}

int PluginProcessor::programMatchingSettings() const
{
    const auto matches = [this] (const Preset& v)
    {
        const auto same = [this] (const char* id, float wanted)
        {
            const auto* p = apvts.getParameter (id);
            return std::abs (p->getValue() - p->convertTo0to1 (wanted)) < 2.0e-4f;
        };
        return same ("drive", v.drive) && same ("bias", v.bias) && same ("speed", (float) v.speed) && same ("wow", v.wowAmt) && same ("flutter", v.flutterAmt)
            && same ("noise", v.noiseAmt) && same ("age", v.ageAmt) && same ("lowcut", v.low) && same ("highcut", v.high) && same ("mix", v.mixAmt)
            && same ("output", v.out) && same ("autolevel", v.autoOn ? 1.0f : 0.0f) && same ("hardness", v.hard) && same ("spacing", v.space)
            && same ("azimuth", v.tilt) && same ("random", v.rnd) && same ("stereodrift", v.stereo) && same ("dropdepth", v.dropD) && same ("droprate", v.dropR)
            && same ("compander", (float) v.comp) && same ("encode", v.enc) && same ("decode", v.dec) && same ("texture", v.tex) && same ("device", (float) v.dev)
            && same ("devicemix", v.devMix) && (! responseMatters() || oldResponse.load (std::memory_order_relaxed) == v.old);
    };

    if (juce::isPositiveAndBelow (currentProgram, (int) std::size (presets)) && matches (presets[currentProgram]))
        return currentProgram;

    for (int i = 0; i < (int) std::size (presets); ++i)
        if (matches (presets[i]))
            return i;

    return -1;
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute (windowWidthAttribute, windowWidth.load (std::memory_order_relaxed));
        xml->setAttribute (windowHeightAttribute, windowHeight.load (std::memory_order_relaxed));
        xml->setAttribute (advancedOpenAttribute, advancedOpen.load (std::memory_order_relaxed));
        copyXmlToBinary (*xml, dest);
        const char tail[8] = { responseTag[0], responseTag[1], responseTag[2], responseTag[3],
                               (char) (oldResponse.load (std::memory_order_relaxed) ? responseOld : responseLinedUp), 0, 0, 0 };
        dest.append (tail, sizeof (tail));
    }
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // Older sessions have no window state. A damaged size goes back in range. Neither is a parameter, so they stay out of the value tree.
            if (xml->hasAttribute (windowWidthAttribute))
                windowWidth.store (juce::jlimit (minWindowWidth, maxWindowWidth, xml->getIntAttribute (windowWidthAttribute, defaultWindowWidth)), std::memory_order_relaxed);
            if (xml->hasAttribute (windowHeightAttribute))
                windowHeight.store (juce::jlimit (minWindowHeight, maxWindowHeight, xml->getIntAttribute (windowHeightAttribute, defaultWindowHeight)), std::memory_order_relaxed);
            advancedOpen.store (xml->getBoolAttribute (advancedOpenAttribute, false), std::memory_order_relaxed);
            xml->removeAttribute (windowWidthAttribute);
            xml->removeAttribute (windowHeightAttribute);
            xml->removeAttribute (advancedOpenAttribute);
            const int response = savedResponse (data, size);
            applySafeState (apvts, *xml);
            // A session saved before 0.4.0 says nothing about its response. It keeps 0.3.1's if it uses anything the two treat differently,
            // so it sounds as it did. With Mix at 100, High Cut off and the compander off there is nothing to keep.
            oldResponse.store (response == responseOld || (response != responseLinedUp && responseMatters()), std::memory_order_relaxed);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PluginProcessor(); }
