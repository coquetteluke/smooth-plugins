#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "SpectralEnvelope.h"
#include "TractModel.h"

// Formant analysis and resynthesis: one or two channels in, the same number out, later by latencySamples().
//
// Every frame:
//   1. A long window of both channels gives one power spectrum, and from it the formant model M(f), the envelope, the pitch and the voicing.
//   2. M is read through the tract's frequency map, then the voice source's tilt is changed.
//   3. The audio passes through a shorter window and gets (M read through the map) - M(f) as a gain per bin. That flattens the old
//      resonances and makes the new ones.
//
// That gain is real and smooth across frequency, so harmonics stay exactly where they were (the pitch does not move), and phases
// are untouched (a dry copy at the same delay blends without combing). When nothing is asked of it the gain is exactly 1 and the
// overlap adds back to the input.
//
// Both channels share one envelope, so the stereo image holds. Nothing here knows about parameters or hosts: another plug-in can
// use this whole class through Controls, or just SpectralEnvelope and TractModel.
namespace formant
{
class FormantEngine
{
public:
    struct Controls
    {
        TractShape tract;
        float tiltDbPerOctave = 0.0f;   // Voice source change. Negative is darker. Only acts while voiced.
        float breath = 0.0f;            // 0 to 1
        float breathHighPassHz = 1500.0f;
        PitchRange range;
        float peakWideningHz = 20.0f;   // Formant model detail. Lower follows narrower formants, higher is steadier.
        bool levelMatch = true;         // Hold each frame's envelope energy, so moving formants does not change the level.
    };

    // For a window. Written by the audio thread with plain atomics, never blocks. A reader might see one frame's
    // curve half updated, which a graph cannot show.
    struct Analysis
    {
        static constexpr int numPoints = 96;
        std::array<std::atomic<float>, numPoints> inputDb {}, outputDb {}; // Log spaced, pointHz(i). Envelope in dB, unweighted.
        std::array<std::atomic<float>, SpectralEnvelope::maxPeaks> formantHz {};
        std::atomic<float> pitchHz { 0.0f }, voicing { 0.0f };
        static float pointHz (int i) noexcept { return 60.0f * std::pow (16000.0f / 60.0f, (float) i / (float) (numPoints - 1)); }
    };

    void prepare (double sampleRate, int channels)
    {
        sr = sampleRate;
        numChannels = juce::jlimit (1, maxChannels, channels);
        const int order = sr <= 50000.0 ? 10 : sr <= 100000.0 ? 11 : 12; // About 21 ms at every rate
        synthSize = 1 << order;
        synthHalf = synthSize / 2;
        hop = synthSize / 4;
        analysisSize = synthSize * 2;
        analysisHalf = analysisSize / 2;
        latency = synthSize + (analysisSize - synthSize) / 2;

        synthFft = std::make_unique<juce::dsp::FFT> (order);
        analysisFft = std::make_unique<juce::dsp::FFT> (order + 1);
        envelope.prepare (sr, order + 1);

        synthWindow.resize ((size_t) synthSize);
        for (int i = 0; i < synthSize; ++i)
            synthWindow[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) synthSize));
        analysisWindow.resize ((size_t) analysisSize);
        for (int i = 0; i < analysisSize; ++i)
            analysisWindow[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) analysisSize));

        for (int c = 0; c < maxChannels; ++c)
        {
            inputRing[(size_t) c].assign ((size_t) analysisSize, 0.0f);
            overlap[(size_t) c].assign ((size_t) synthSize, 0.0f);
        }
        analysisIn.assign ((size_t) analysisSize, {});
        analysisOut.assign ((size_t) analysisSize, {});
        power.assign ((size_t) analysisHalf + 1, 0.0f);
        spectrum.assign ((size_t) synthSize * 2, 0.0f);
        noiseSpectrum.assign ((size_t) synthSize * 2, 0.0f);
        logGain.assign ((size_t) synthHalf + 1, 0.0f);
        wanted.assign ((size_t) analysisHalf + 1, 0.0f);
        finerEnvelope.assign ((size_t) analysisHalf + 1, 0.0f);
        readHz.assign ((size_t) synthHalf + 1, 0.0f);
        breathShape.assign ((size_t) synthHalf + 1, 0.0f);
        wantedOnSynthGrid.assign ((size_t) synthHalf + 1, 0.0f);
        modelGain.assign ((size_t) synthHalf + 1, 0.0f);

        for (int i = 0; i < phaseTableSize; ++i)
        {
            phaseTable[(size_t) i][0] = (float) std::cos (juce::MathConstants<double>::twoPi * i / phaseTableSize);
            phaseTable[(size_t) i][1] = (float) std::sin (juce::MathConstants<double>::twoPi * i / phaseTableSize);
        }

        // Control changes settle in about 40 ms.
        settle = (float) (1.0 - std::exp (-(double) hop / (0.04 * sr)));
        levelTimeCoef = 1.0 - std::exp (-(double) hop / (2.0 * sr));
        reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < maxChannels; ++c)
        {
            std::fill (inputRing[(size_t) c].begin(), inputRing[(size_t) c].end(), 0.0f);
            std::fill (overlap[(size_t) c].begin(), overlap[(size_t) c].end(), 0.0f);
        }
        std::fill (logGain.begin(), logGain.end(), 0.0f);
        writePos = 0;
        overlapPos = 0;
        samplesUntilFrame = hop;
        current = target;
        smoothedTilt = target.tiltDbPerOctave;
        smoothedBreath = target.breath;
        smoothedHighPass = target.breathHighPassHz;
        smoothedWidening = target.peakWideningHz;
        levelFix = fastLevelFix = slowLevelFix = 0.0f;
        slowEnergyIn = slowEnergyOut = 0.0;
        levelAmount = target.levelMatch ? 1.0f : 0.0f;
        tiltVoicing = 0.0f;
        noiseState = 0x9E3779B9u;
        channelShare = { 1.0f, 1.0f };
        envelope.reset();
        mapIsCurrent = false;
        refreshMap();
    }

    void setControls (const Controls& c) noexcept { target = c; }
    int latencySamples() const noexcept { return latency; }
    const Analysis& analysis() const noexcept { return readout; }
    const SpectralEnvelope& envelopeAnalyser() const noexcept { return envelope; }

    // In place. channels[0 .. numChannels - 1], any block size.
    void process (float* const* channels, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < numChannels; ++c)
            {
                inputRing[(size_t) c][(size_t) writePos] = channels[c][i];
                auto& o = overlap[(size_t) c];
                channels[c][i] = o[(size_t) overlapPos];
                o[(size_t) overlapPos] = 0.0f;
            }
            writePos = (writePos + 1) & (analysisSize - 1);
            overlapPos = (overlapPos + 1) & (synthSize - 1);
            if (--samplesUntilFrame == 0)
            {
                samplesUntilFrame = hop;
                runFrame();
            }
        }
    }

private:
    static constexpr int maxChannels = 2, phaseTableSize = 1024;
    static constexpr float maxLogGain = 3.0f;     // 26 dB either way
    static constexpr float nepersPerDb = 0.115129f;
    static constexpr float resynthesisFull = 0.05f; // A move of this much, as a log ratio, takes the harmonics' levels fully from the model

    static float tiltCurve (float hz, float cornerHz) noexcept
    {
        const float f = std::min (hz, 12000.0f) / cornerHz;
        return 0.5f * std::log2 (1.0f + f * f); // Octaves above the corner, levelling off at 12 kHz
    }

    void smoothControls() noexcept
    {
        const auto towardsLog = [this] (float& v, float to)
        {
            const float next = v * std::exp (settle * std::log (to / v));
            v = std::abs (next - to) < 1.0e-5f * to ? to : next;
        };
        const auto towards = [this] (float& v, float to, float close)
        {
            const float next = v + settle * (to - v);
            v = std::abs (next - to) < close ? to : next;
        };
        const auto before = current;
        towardsLog (current.tract.length, target.tract.length);
        towardsLog (current.tract.width, target.tract.width);
        for (size_t p = 0; p < TractShape::numPoints; ++p)
        {
            towardsLog (current.tract.widths[p], target.tract.widths[p]);
            towards (current.tract.places[p], target.tract.places[p], 1.0e-4f);
        }
        towards (smoothedTilt, target.tiltDbPerOctave, 1.0e-3f);
        towards (smoothedBreath, target.breath, 1.0e-4f);
        towards (smoothedWidening, target.peakWideningHz, 1.0e-2f);
        towardsLog (smoothedHighPass, target.breathHighPassHz);

        bool moved = before.tract.length != current.tract.length || before.tract.width != current.tract.width;
        for (size_t p = 0; p < TractShape::numPoints; ++p)
            moved = moved || before.tract.widths[p] != current.tract.widths[p] || before.tract.places[p] != current.tract.places[p];
        if (moved) mapIsCurrent = false;
        refreshMap();
    }

    void refreshMap() noexcept
    {
        if (mapIsCurrent) return;
        identityMap = current.tract.isIdentity();
        const double binHz = sr / (double) synthSize;
        if (identityMap)
            for (int k = 0; k <= synthHalf; ++k) readHz[(size_t) k] = (float) (k * binHz);
        else
            tract.buildMap (current.tract, binHz, synthHalf + 1, readHz.data());
        mapIsCurrent = true;
    }

    void analyseFrame() noexcept
    {
        // Both channels through one complex transform: the power of the pair can be read from it directly.
        const auto& left = inputRing[0];
        const auto& right = inputRing[numChannels > 1 ? 1 : 0];
        double energy[2] = { 0.0, 0.0 };
        const bool stereo = numChannels > 1;
        for (int i = 0; i < analysisSize; ++i)
        {
            const int at = (writePos + i) & (analysisSize - 1);
            const float w = analysisWindow[(size_t) i];
            const float l = left[(size_t) at] * w, r = stereo ? right[(size_t) at] * w : 0.0f;
            analysisIn[(size_t) i] = { l, r };
            energy[0] += (double) l * l;
            energy[1] += (double) r * r;
        }
        analysisFft->perform (analysisIn.data(), analysisOut.data(), false);
        const float scale = stereo ? 0.5f : 1.0f;
        power[0] = std::norm (analysisOut[0]) * scale;
        power[(size_t) analysisHalf] = std::norm (analysisOut[(size_t) analysisHalf]) * scale;
        for (int k = 1; k < analysisHalf; ++k)
            power[(size_t) k] = (std::norm (analysisOut[(size_t) k]) + std::norm (analysisOut[(size_t) (analysisSize - k)])) * 0.5f * scale;

        frameIsSilent = energy[0] + energy[1] < 1.0e-13 * analysisSize;
        if (stereo)
        {
            const double mean = 0.5 * (energy[0] + energy[1]) + 1.0e-20;
            channelShare[0] = (float) std::sqrt (energy[0] / mean);
            channelShare[1] = (float) std::sqrt (energy[1] / mean);
        }

        envelope.analyse (power.data(), target.range, smoothedWidening);
    }

    // Leaves the gain for this frame in logGain, and the output envelope in wantedOnSynthGrid (nepers, synth grid).
    // Returns true when the gain is exactly 1 at every bin.
    bool shapeFrame() noexcept
    {
        const float* m = envelope.formantModel();
        const float* heard = envelope.envelope();
        const float* flat = envelope.flatEnvelope();
        const float voicing = envelope.voicing();
        const float f0 = envelope.fundamentalHz();
        const double binHz = sr / (double) synthSize;
        const float envelopeBinsPerHz = (float) analysisSize / (float) sr;

        tiltVoicing += 0.35f * (voicing - tiltVoicing);
        const float width = current.tract.width;
        const float sharpen = std::pow (width, 0.35f) - 1.0f;   // Wider tube, less wall loss, narrower formants
        const bool sharpening = std::abs (sharpen) > 1.0e-3f;
        const float* readFrom = m;
        if (sharpening)
        {
            // Only the detail is scaled, around a smooth version of the model.
            envelope.lifter (m, 0.0007f * (float) sr, finerEnvelope.data());
            for (int k = 0; k <= analysisHalf; ++k)
                wanted[(size_t) k] = m[k] + sharpen * (m[k] - finerEnvelope[(size_t) k]);
            readFrom = wanted.data();
        }

        const bool tilting = std::abs (smoothedTilt) > 1.0e-3f && f0 > 0.0f;
        const float tiltNepers = smoothedTilt * nepersPerDb * tiltVoicing;
        const float corner = std::max (1.5f * f0, 500.0f); // The lowest few harmonics stay, and the upper ones turn

        const float breath = smoothedBreath;
        const bool breathing = breath > 1.0e-4f && ! frameIsSilent;
        // Harmonics to noise ratio in dB across the breath's band: 30 dB as it starts, 12 dB at 50, -6 dB at 100.
        const float noiseToVoice = breathing ? std::pow (10.0f, -(30.0f - 36.0f * breath) / 10.0f) * std::min (1.0f, breath * 10.0f) * std::min (1.0f, breath * 10.0f) : 0.0f;
        const float gate = 0.25f + 0.75f * voicing;
        const float breathCorner = smoothedHighPass;
        if (breathing)
            for (int k = 0; k <= synthHalf; ++k)
            {
                const float f = (float) (k * binHz) / breathCorner, f6 = f * f * f * f * f * f;
                breathShape[(size_t) k] = f6 / (1.0f + f6); // Power, a slope of 18 dB per octave
            }

        // The fundamental and what lies under it belong to the voice source, not the tract, so the map lets go there.
        const float sourceTop = voicing > 0.3f && f0 > 0.0f ? f0 : 60.0f;
        const float keepBelow = 1.05f * sourceTop, moveAbove = 1.4f * sourceTop;


        float biggest = 0.0f;
        for (int k = 0; k <= synthHalf; ++k)
        {
            float at = (float) (2 * k);
            float resynthesis = 0.0f;
            if (! identityMap)
            {
                const float hz = (float) (k * binHz);
                float read = readHz[(size_t) k];
                if (hz < moveAbove)
                {
                    const float t = hz <= keepBelow ? 0.0f : (hz - keepBelow) / (moveAbove - keepBelow);
                    read = hz + t * t * (3.0f - 2.0f * t) * (read - hz);
                }
                at = juce::jlimit (0.0f, (float) analysisHalf, read * envelopeBinsPerHz);
                // The model is too smooth to take a narrow formant off completely, and what it leaves stays behind as a ghost of the old
                // formant. So the further things move, the more the harmonics take their level from the model alone. Nearly unmoved, a
                // harmonic keeps its own detail, and the change from that to none at all at a setting of 1 is smooth.
                // The first two harmonics keep more of their own: the voice source shapes them more than the tract does.
                resynthesis = hz > 0.0f ? std::min (1.0f, std::abs (std::log (std::max (read, 1.0f) / hz)) / resynthesisFull) : 0.0f;
                if (hz < 1.8f * sourceTop)
                    resynthesis *= juce::jlimit (0.0f, 1.0f, (hz - moveAbove) / (1.8f * sourceTop - moveAbove));
            }
            const int a = std::min ((int) at, analysisHalf - 1);
            float w = readFrom[a] + (at - (float) a) * (readFrom[a + 1] - readFrom[a]);
            if (tilting)
                w += tiltNepers * tiltCurve ((float) (k * binHz), corner);
            const float reference = m[2 * k] + resynthesis * (flat[2 * k] - m[2 * k]);
            const float g = juce::jlimit (-maxLogGain, maxLogGain, w - reference);
            modelGain[(size_t) k] = juce::jlimit (-maxLogGain, maxLogGain, w - m[2 * k]);
            logGain[(size_t) k] = std::isfinite (g) ? g : 0.0f;
            wantedOnSynthGrid[(size_t) k] = heard[2 * k] + logGain[(size_t) k];
            biggest = std::max (biggest, std::abs (logGain[(size_t) k]));
        }

        // Moving formants must not change the level. This takes two steps.
        // A quicker one, over about 150 ms, compares the model's energy before and after, with the source's fall on it, so each sound keeps its own level from
        // one moment to the next. It knows nothing of where the harmonics are, so it does not iron out what a harmonic does as it crosses
        // a formant.
        // A slow one, over about two seconds, then compares the frame's real spectrum before and after, plus the breath to be added.
        // How much energy really passes depends on where the harmonics fall, which the model cannot say. It is too slow to pump.
        levelAmount += 0.3f * ((target.levelMatch ? 1.0f : 0.0f) - levelAmount);
        float fastWanted = 0.0f, slowWanted = 0.0f;
        if ((biggest >= 1.0e-6f || breathing) && levelAmount > 1.0e-3f)
        {
            const int low = (int) (60.0 / binHz), high = std::min (synthHalf, (int) (16000.0 / binHz));
            double modelIn = 1.0e-30, modelOut = 1.0e-30;
            for (int k = low; k <= high; ++k)
            {
                const float source = heard[2 * k] - flat[2 * k];
                const float before = 2.0f * (m[2 * k] + source);
                modelIn += std::exp (before);
                modelOut += std::exp (before + 2.0f * modelGain[(size_t) k]);
            }
            fastWanted = juce::jlimit (-1.4f, 1.4f, (float) (0.5 * std::log (modelIn / modelOut)));

            const int lowBin = (int) (60.0 / (binHz * 0.5)), highBin = std::min (analysisHalf - 1, (int) (16000.0 / (binHz * 0.5)));
            const float noiseWeight = noiseToVoice * gate * gate;
            const float afterFast = std::exp (2.0f * levelAmount * fastLevelFix);
            double energyIn = 0.0, energyOut = 0.0;
            for (int j = lowBin; j <= highBin; ++j)
            {
                const int k = j / 2;
                const float g = (j & 1) == 0 ? logGain[(size_t) k] : 0.5f * (logGain[(size_t) k] + logGain[(size_t) k + 1]);
                energyIn += power[(size_t) j];
                energyOut += power[(size_t) j] * std::exp (2.0f * g) * afterFast * (1.0f + (breathing ? noiseWeight * breathShape[(size_t) k] : 0.0f));
            }
            slowEnergyIn += levelTimeCoef * (energyIn - slowEnergyIn);
            slowEnergyOut += levelTimeCoef * (energyOut - slowEnergyOut);
            slowWanted = slowEnergyIn > 1.0e-12 ? juce::jlimit (-1.0f, 1.0f, (float) (0.5 * std::log ((slowEnergyIn + 1.0e-20) / (slowEnergyOut + 1.0e-20)))) : slowLevelFix;
        }
        else
        {
            slowEnergyIn = slowEnergyOut = 0.0;
        }
        fastLevelFix += 0.04f * (fastWanted - fastLevelFix);
        slowLevelFix += 0.5f * (slowWanted - slowLevelFix);
        if (std::abs (fastLevelFix) < 1.0e-6f && fastWanted == 0.0f) fastLevelFix = 0.0f;
        if (std::abs (slowLevelFix) < 1.0e-6f && slowWanted == 0.0f) slowLevelFix = 0.0f;
        levelFix = levelAmount * (fastLevelFix + slowLevelFix);

        if (biggest < 1.0e-6f && levelFix == 0.0f)
        {
            std::fill (logGain.begin(), logGain.end(), 0.0f);
            return ! breathing;
        }
        for (int k = 0; k <= synthHalf; ++k)
            logGain[(size_t) k] += levelFix;
        return false;
    }

    // Breath, shaped by the output envelope so it passes through the same modelled tract as the voice. One noise goes to both
    // channels, sized by each channel's share, so it stays where the voice is in the stereo image.
    void makeBreath() noexcept
    {
        const float f0 = envelope.fundamentalHz();
        const float voicing = envelope.voicing();
        const double binHz = sr / (double) synthSize;
        const float breath = smoothedBreath;
        const float noiseToVoice = std::pow (10.0f, -(30.0f - 36.0f * breath) / 10.0f) * std::min (1.0f, breath * 10.0f) * std::min (1.0f, breath * 10.0f);
        // The envelope rests on harmonics, and sparser harmonics carry less energy under the same envelope. Scaling by their spacing
        // makes the harmonics to noise ratio the same on a low voice and a high one.
        const float spacingBins = (voicing > 0.2f && f0 > 0.0f ? f0 : 150.0f) / (float) binHz;
        const float gate = 0.25f + 0.75f * voicing;
        // The 3 is measured: the envelope between harmonics lies lower than their tips, which the plain sum would miss.
        const float level = std::sqrt (3.0f * 6.0f * noiseToVoice / spacingBins) * gate;
        // Envelope values are on the analysis window's scale.
        const float toSynthScale = 0.5f;
        for (int k = 0; k <= synthHalf; ++k)
        {
            noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
            const auto& phase = phaseTable[(size_t) (noiseState >> 22)];
            const float magnitude = level * toSynthScale * std::sqrt (breathShape[(size_t) k])
                                  * std::exp (juce::jlimit (-40.0f, 20.0f, wantedOnSynthGrid[(size_t) k] + levelFix));
            noiseSpectrum[(size_t) (2 * k)] = magnitude * phase[0];
            noiseSpectrum[(size_t) (2 * k + 1)] = magnitude * phase[1];
        }
        noiseSpectrum[1] = 0.0f;
        noiseSpectrum[(size_t) (2 * synthHalf + 1)] = 0.0f;
    }

    void runFrame() noexcept
    {
        smoothControls();
        analyseFrame();
        const bool unity = shapeFrame();
        const bool breathing = smoothedBreath > 1.0e-4f && ! frameIsSilent;
        if (breathing) makeBreath();

        const int startInRing = (writePos + (analysisSize - synthSize) / 2) & (analysisSize - 1);
        const float normalise = 1.0f / 1.5f; // Hann times Hann at a quarter frame hop adds to exactly 1.5
        for (int c = 0; c < numChannels; ++c)
        {
            const auto& ring = inputRing[(size_t) c];
            auto& o = overlap[(size_t) c];
            if (unity)
            {
                for (int i = 0; i < synthSize; ++i)
                {
                    const float w = synthWindow[(size_t) i];
                    o[(size_t) ((overlapPos + i) & (synthSize - 1))] += ring[(size_t) ((startInRing + i) & (analysisSize - 1))] * w * w * normalise;
                }
                continue;
            }
            for (int i = 0; i < synthSize; ++i)
                spectrum[(size_t) i] = ring[(size_t) ((startInRing + i) & (analysisSize - 1))] * synthWindow[(size_t) i];
            std::fill (spectrum.begin() + synthSize, spectrum.end(), 0.0f);
            synthFft->performRealOnlyForwardTransform (spectrum.data());
            const float share = numChannels > 1 ? channelShare[(size_t) c] : 1.0f;
            for (int k = 0; k <= synthHalf; ++k)
            {
                const float g = std::exp (logGain[(size_t) k]);
                spectrum[(size_t) (2 * k)] *= g;
                spectrum[(size_t) (2 * k + 1)] *= g;
                if (breathing)
                {
                    spectrum[(size_t) (2 * k)] += share * noiseSpectrum[(size_t) (2 * k)];
                    spectrum[(size_t) (2 * k + 1)] += share * noiseSpectrum[(size_t) (2 * k + 1)];
                }
            }
            std::fill (spectrum.begin() + synthSize + 2, spectrum.end(), 0.0f);
            synthFft->performRealOnlyInverseTransform (spectrum.data());
            for (int i = 0; i < synthSize; ++i)
                o[(size_t) ((overlapPos + i) & (synthSize - 1))] += spectrum[(size_t) i] * synthWindow[(size_t) i] * normalise;
        }
        publish();
    }

    void publish() noexcept
    {
        readout.pitchHz.store (envelope.fundamentalHz(), std::memory_order_relaxed);
        readout.voicing.store (envelope.voicing(), std::memory_order_relaxed);
        for (int p = 0; p < SpectralEnvelope::maxPeaks; ++p)
            readout.formantHz[(size_t) p].store (p < envelope.numPeaks() ? envelope.peakHz (p) : 0.0f, std::memory_order_relaxed);
        const float* e = envelope.envelope();
        const float binsPerHz = (float) synthSize / (float) sr;
        constexpr float dbPerNeper = 8.685889f;
        for (int i = 0; i < Analysis::numPoints; ++i)
        {
            const int k = std::min (synthHalf, (int) (Analysis::pointHz (i) * binsPerHz));
            readout.inputDb[(size_t) i].store (e[2 * k] * dbPerNeper, std::memory_order_relaxed);
            readout.outputDb[(size_t) i].store ((e[2 * k] + logGain[(size_t) k]) * dbPerNeper, std::memory_order_relaxed);
        }
    }

    double sr = 48000.0;
    int numChannels = 2, synthSize = 1024, synthHalf = 512, hop = 256, analysisSize = 2048, analysisHalf = 1024, latency = 1536;
    int writePos = 0, overlapPos = 0, samplesUntilFrame = 256;
    std::unique_ptr<juce::dsp::FFT> synthFft, analysisFft;
    SpectralEnvelope envelope;
    TractModel tract;
    Controls target, current;
    float smoothedTilt = 0.0f, smoothedBreath = 0.0f, smoothedHighPass = 1500.0f, smoothedWidening = 20.0f, settle = 0.1f;
    float levelFix = 0.0f, fastLevelFix = 0.0f, slowLevelFix = 0.0f, levelAmount = 1.0f, tiltVoicing = 0.0f;
    double slowEnergyIn = 0.0, slowEnergyOut = 0.0, levelTimeCoef = 0.03;
    bool mapIsCurrent = false, identityMap = true, frameIsSilent = true;
    std::array<std::vector<float>, maxChannels> inputRing, overlap;
    std::vector<juce::dsp::Complex<float>> analysisIn, analysisOut;
    std::vector<float> synthWindow, analysisWindow, power, spectrum, noiseSpectrum, logGain, wanted, finerEnvelope, readHz, breathShape, wantedOnSynthGrid, modelGain;
    std::array<std::array<float, 2>, phaseTableSize> phaseTable {};
    std::array<float, maxChannels> channelShare { 1.0f, 1.0f };
    std::uint32_t noiseState = 0x9E3779B9u;
    Analysis readout;
};
} // namespace formant
