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
        cepstrumFft = std::make_unique<juce::dsp::FFT> (order + 1);
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
        mapBefore.assign ((size_t) synthHalf + 1, 0.0f);
        breathShape.assign ((size_t) synthHalf + 1, 0.0f);
        wantedOnSynthGrid.assign ((size_t) synthHalf + 1, 0.0f);
        envelopeShift.assign ((size_t) synthHalf + 1, 0.0f);
        logPower.assign ((size_t) analysisHalf + 1, 0.0f);
        voicedEnvelope.assign ((size_t) analysisHalf + 1, 0.0f);
        cepstrumWork.assign ((size_t) analysisSize * 2, 0.0f);

        for (int i = 0; i < phaseTableSize; ++i)
        {
            phaseTable[(size_t) i][0] = (float) std::cos (juce::MathConstants<double>::twoPi * i / phaseTableSize);
            phaseTable[(size_t) i][1] = (float) std::sin (juce::MathConstants<double>::twoPi * i / phaseTableSize);
        }

        // Control changes settle in about 40 ms.
        settle = (float) (1.0 - std::exp (-(double) hop / (0.04 * sr)));
        levelTimeCoef = 1.0 - std::exp (-(double) hop / (2.0 * sr));
        recentCoef = 1.0 - std::exp (-(double) hop / (0.1 * sr));
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
        recentEnergyIn = 0.0;
        recentHistory.fill (0.0);
        recentPos = 0;
        framesSinceStart = 1000;
        levelAmount = target.levelMatch ? 1.0f : 0.0f;
        framesSinceControlMoved = 1000;
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
    // Above highBandFromHz the gain may be at most this much over what moving the envelope itself would give. See shapeFrame.
    // From this many times the pitch up the move starts to take the harmonics' own envelope instead of the model, fully at twice that.
    static constexpr float envelopeFromHarmonic = 4.0f;
    static constexpr float highBandFromHz = 6000.0f;
    // Phrase starts (see shapeFrame): a rise is judged against the level as it was this many frames back (43 ms at 48 kHz), and for
    // quickFrames frames (320 ms) after a rise of 6 dB the quick level step follows each frame's real spectrum closely.
    static constexpr int startLookback = 8, quickFrames = 60;
    static constexpr double startRise = 4.0, phraseRise = 10.0;
    // 11 dB in 0.4.1, the least that leaves the 0.2.0 renders bit for bit. On the branch 8 dB: at 11 dB Length 1.50 still had 1.8% of
    // voiced frames on takes 01 and 16 more than 12 dB over the tube model from 6 to 12 kHz, mostly where the level match lifts the
    // whole frame. At 8 dB: 0.16%.
    static constexpr float highBandHeadroom = 8.0f * nepersPerDb;

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
        const float tiltBefore = smoothedTilt, breathBefore = smoothedBreath, wideningBefore = smoothedWidening, highPassBefore = smoothedHighPass;
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
        if (moved)
        {
            std::copy (readHz.begin(), readHz.end(), mapBefore.begin());
            mapIsCurrent = false;
        }
        refreshMap();

        // While a control is on its way somewhere the level match has to keep up with it (see shapeFrame). Counted in frames.
        // Only a move that changes the sound counts. A position does nothing while the widths around it are 1.00, and the breath's
        // high pass nothing while Breathiness is 0; before 0.7.1 moving either still restarted the level match's slow average and
        // changed the level for up to 320 ms. So the tract counts when the map it gives changes, or the width, which also sets how
        // narrow the formants are.
        const bool reshaped = moved && (before.tract.width != current.tract.width || ! std::equal (readHz.begin(), readHz.end(), mapBefore.begin()));
        const bool shaping = reshaped || tiltBefore != smoothedTilt || breathBefore != smoothedBreath
                          || wideningBefore != smoothedWidening || (highPassBefore != smoothedHighPass && smoothedBreath > 1.0e-4f);
        if (shaping) framesSinceControlMoved = 0;
        else if (framesSinceControlMoved < 1000) ++framesSinceControlMoved;
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


        // On voiced frames the move above 4 times the pitch reads a plain cepstral envelope: the log spectrum with every quefrency
        // under 0.6 of the period kept as it is. The envelope through the harmonic tips, and a lifter that tapers, are smoother than
        // that, and a smoother envelope moves less of a formant's shape: a move of a few percent (the width points) came out about
        // 0.6 of the way the tube says on real vocals, against 0.8 for a plain mover. Unvoiced frames keep the tips envelope, whose
        // steadiness Throat Precision sets.
        // The voice source's fall stays in this envelope, so a move carries it: on a plain -6 dB per octave source the highs come out
        // 2.9 dB brighter at Length 0.70 and 2.7 dB darker at 1.50, from 4 to 8 times the pitch up (throat-r1-1, open). 0.7.1 took the
        // fall off here as fitModel does, and on Luke's takes that broke 7 real vocal checks that 0.7.0 passed: consonant smear at
        // Length 1.20, 1.50 and Big Chest, late phrase starts at 1.50, and the width points' move (Lips 0.60 0.80 to 0.76 of the tube,
        // limit 0.8). The real vocal checks read a move against the whole envelope, source slope and all; a fall taken off only part of
        // the way (60%), or only on clearly voiced frames, still broke them. So 0.7.2 is back to 0.7.0 here.
        float voicedShare = 0.0f;
        if (! identityMap && f0 > 60.0f)
        {
            const float v = juce::jlimit (0.0f, 1.0f, (voicing - 0.3f) / 0.3f);
            voicedShare = v * v * (3.0f - 2.0f * v);
        }
        if (voicedShare > 0.0f)
        {
            float top = 0.0f;
            for (int k = 0; k <= analysisHalf; ++k) top = std::max (top, power[(size_t) k]);
            const float floorPower = std::max (top * 1.0e-9f, 1.0e-16f);
            std::fill (cepstrumWork.begin(), cepstrumWork.end(), 0.0f);
            for (int k = 0; k <= analysisHalf; ++k)
                cepstrumWork[(size_t) (2 * k)] = 0.5f * std::log (std::max (power[(size_t) k], floorPower));
            cepstrumFft->performRealOnlyInverseTransform (cepstrumWork.data());
            const int cut = std::max (8, (int) (0.6f * (float) sr / f0));
            for (int q = cut; q <= analysisSize - cut; ++q)
                cepstrumWork[(size_t) q] = 0.0f;
            std::fill (cepstrumWork.begin() + analysisSize, cepstrumWork.end(), 0.0f);
            cepstrumFft->performRealOnlyForwardTransform (cepstrumWork.data());
            for (int k = 0; k <= analysisHalf; ++k)
                voicedEnvelope[(size_t) k] = cepstrumWork[(size_t) (2 * k)];
        }

        // Where the move changes from the model to the envelope (4 to 8 times the pitch, below). On a frame that is not voiced the
        // pitch is only the last one sung, so it is used fully from a voicing of 0.3, as sourceTop is, and not at all under 0.2, as
        // the whistle guard. Before 0.7.1 a consonant was shaped by the note before it: a hiss band passed 1 to 3 kHz came out up to
        // 1.2 dB different after a 900 Hz note than after a 100 Hz one. Under a voicing of 0.2 the change sits as for a 250 Hz note, the
        // same after any note. 0.7.1 put it as for 60 Hz (the envelope from 240 Hz up), and on Luke's takes Big Chest's consonant smear
        // after quiet went over its limit (p90 2.96 to 3.01 dB, limit 3); as for 150, 250 or 400 Hz it read 2.98 to 2.99.
        constexpr float unvoicedPitchHz = 250.0f;
        const float voicedPitch = juce::jlimit (0.0f, 1.0f, (voicing - 0.2f) / 0.1f);
        const float crossoverHz = unvoicedPitchHz + voicedPitch * voicedPitch * (3.0f - 2.0f * voicedPitch) * (std::max (f0, 60.0f) - unvoicedPitchHz);

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
            // What the flattened envelope itself, read at the same place, says this bin should get.
            float shift = flat[a] + (at - (float) a) * (flat[a + 1] - flat[a]) - flat[2 * k];
            if (voicedShare > 0.0f)
            {
                const float* c = voicedEnvelope.data();
                shift += voicedShare * (c[a] + (at - (float) a) * (c[a + 1] - c[a]) - c[2 * k] - shift);
            }
            if (tilting)
                shift += tiltNepers * tiltCurve ((float) (k * binHz), corner);
            // Where many harmonics sit on each formant (from 4 times the pitch up, fully from 8 times), the envelope through them shows
            // the formants as they are, and moving it moves them exactly as far as the tube says. The model is smoother than that: on
            // real vocals its formants came out about 0.7 to 0.8 of the way and scattered (0.5.0, Length 0.70: 9% of frames 9% off).
            // Low down, with two or three harmonics on a formant, the envelope cannot place it and the model can, so it stays there.
            // The width's sharpening is kept in both.
            float gRaw = w - reference;
            if (! identityMap)
            {
                const float u = juce::jlimit (0.0f, 1.0f, ((float) (k * binHz) / crossoverHz - envelopeFromHarmonic) / envelopeFromHarmonic);
                const float fromEnvelope = u * u * (3.0f - 2.0f * u);
                const float mRead = m[a] + (at - (float) a) * (m[a + 1] - m[a]);
                const float detail = readFrom[a] + (at - (float) a) * (readFrom[a + 1] - readFrom[a]) - mRead;
                gRaw += fromEnvelope * (shift + detail - gRaw);
            }
            const float g = juce::jlimit (-maxLogGain, maxLogGain, gRaw);
            envelopeShift[(size_t) k] = juce::jlimit (-maxLogGain, maxLogGain, shift);
            logGain[(size_t) k] = std::isfinite (g) ? g : 0.0f;
            wantedOnSynthGrid[(size_t) k] = heard[2 * k] + logGain[(size_t) k];
            biggest = std::max (biggest, std::abs (logGain[(size_t) k]));
        }

        // No harmonic may whistle. The model's peaks can be narrower than the space between two harmonics, and when a harmonic lands
        // on one it comes out far above its neighbours: on real vocals 1 to 10% of voiced frames had a harmonic 12 dB or more over
        // the two next to it and 6 dB more than any harmonic of the input stood out. So each harmonic from 500 Hz up is held to
        // standing out by no more than 8 dB, or 2 dB more than the input's most prominent one if that is more. What is over comes off
        // that harmonic alone, half a harmonic spacing either side.
        // The harmonics are looked for at whole multiples of the pitch, within a quarter of the spacing, in the long window's spectrum.
        // (Up to 0.5.0 the envelope's tips were used, but those are found one from the last, and on a gliding note they wander off the
        // harmonics: on a rap take at Giant a harmonic at 1640 Hz came out 20 dB over its neighbours while the guard looked at 1770 Hz.)
        int count = 0;
        if (f0 > 60.0f && voicing > 0.2f)
        {
            const float step = f0 * (float) analysisSize / (float) sr;
            const int top = std::min (analysisHalf - 1, (int) (9000.0f * (float) analysisSize / (float) sr));
            for (float next = step; next < (float) top && count < (int) harmonicIn.size(); next += step)
            {
                const int from = std::max (1, (int) std::ceil (next - 0.25f * step)), to = std::min (analysisHalf - 1, (int) (next + 0.25f * step));
                int best = from;
                for (int j = from; j <= to; ++j)
                    if (power[(size_t) j] > power[(size_t) best]) best = j;
                harmonicBin[(size_t) count] = (float) best;
                harmonicIn[(size_t) count] = 0.5f * std::log (power[(size_t) best] + 1.0e-30f);
                ++count;
            }
        }
        if (! identityMap && count >= 3 && f0 > 60.0f)
        {
            const float spacing = f0 * (float) synthSize / (float) sr;
            // What a harmonic really gets: the short window spreads it over about two bins either side of its centre, so where the gain is
            // steep (a narrow peak of the model) it picks up the gain of the bins beside it too. Weighted by the window's spectrum.
            const auto effectiveGain = [this] (float centre)
            {
                double sum = 0.0, weight = 0.0;
                for (int k = (int) std::ceil (centre - 2.0f); k <= (int) std::floor (centre + 2.0f); ++k)
                {
                    if (k < 0 || k > synthHalf) continue;
                    const float d = (float) k - centre;
                    const auto dirichlet = [] (float x) { return std::abs (x) < 1.0e-4f ? 1.0f : std::sin (juce::MathConstants<float>::pi * x) / (juce::MathConstants<float>::pi * x); };
                    const float w = 0.5f * dirichlet (d) + 0.25f * dirichlet (d - 1.0f) + 0.25f * dirichlet (d + 1.0f);
                    sum += (double) (w * w) * std::exp ((double) logGain[(size_t) k]);
                    weight += (double) (w * w);
                }
                // The gains add up in amplitude, as the overlap add does: an average of powers would read a harmonic beside a steep
                // gain as brighter than it comes out, and its neighbour as less prominent than it is.
                return weight > 0.0 ? (float) std::log (sum / weight) : 0.0f;
            };
            for (int t = 0; t < count; ++t)
                harmonicOut[(size_t) t] = harmonicIn[(size_t) t] + effectiveGain (std::min (0.5f * harmonicBin[(size_t) t], (float) synthHalf));   // the harmonics are on the long window's grid
            const float lowest = 500.0f * (float) synthSize / (float) sr, highest = 8000.0f * (float) synthSize / (float) sr;
            float most = 0.0f;
            for (int t = 1; t + 1 < count; ++t)
                if (0.5f * harmonicBin[(size_t) t] >= lowest)
                    most = std::max (most, harmonicIn[(size_t) t] - 0.5f * (harmonicIn[(size_t) t - 1] + harmonicIn[(size_t) t + 1]));
            const float cap = std::max (8.0f * nepersPerDb, most + 2.0f * nepersPerDb);
            // The cut is shaped, so it takes a little less than the excess off the harmonic itself: three passes.
            for (int pass = 0; pass < 3; ++pass)
                for (int t = 1; t + 1 < count; ++t)
                {
                    const float centre = 0.5f * harmonicBin[(size_t) t];
                    if (centre < lowest || centre > highest) continue;
                    const float excess = harmonicOut[(size_t) t] - 0.5f * (harmonicOut[(size_t) t - 1] + harmonicOut[(size_t) t + 1]) - cap;
                    if (excess <= 0.0f) continue;
                    const float halfWidth = 0.5f * spacing;
                    for (int k = (int) std::ceil (centre - halfWidth); k <= (int) (centre + halfWidth); ++k)
                        if (k >= 0 && k <= synthHalf)
                        {
                            logGain[(size_t) k] -= excess * 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * ((float) k - centre) / halfWidth));
                            wantedOnSynthGrid[(size_t) k] = heard[2 * k] + logGain[(size_t) k];
                        }
                    harmonicOut[(size_t) t] = harmonicIn[(size_t) t] + effectiveGain (centre);
                    harmonicOut[(size_t) t - 1] = harmonicIn[(size_t) t - 1] + effectiveGain (0.5f * harmonicBin[(size_t) t - 1]);
                    harmonicOut[(size_t) t + 1] = harmonicIn[(size_t) t + 1] + effectiveGain (std::min (0.5f * harmonicBin[(size_t) t + 1], (float) synthHalf));
                }
        }

        // Moving formants must not change the level. This takes two steps.
        // A quicker one, over about 150 ms, compares the input's envelope before and after the gain this frame really gets, so each
        // sound keeps its own level from one moment to the next. The envelope is smooth between harmonics, so this does not iron out
        // what a harmonic does as it crosses a formant. (Up to 0.6.0 it read the formant model instead, which can be far off: on a real
        // vocal at Lips 0.60 the moved first formant landed on the strong second harmonic, the frame really went up 13 dB, and the
        // model said nothing changed.)
        // A slow one, over about two seconds, then compares the frame's real spectrum before and after, plus the breath to be added,
        // against what the quick step wants for this frame, so it only holds what the envelope cannot see (where the harmonics fall).
        // It is too slow to pump.
        // At a phrase start both are too slow: the first 200 ms of a phrase came out up to 10 dB over or 5 dB under the dry, which
        // moved the phrase's onset by 20 to 100 ms. So for 320 ms after the level rises 6 dB over where it was 43 ms before, the quick
        // step reads each frame's real spectrum and goes half way to it every frame (about 10 ms). The same while a control moves,
        // and for 320 ms after it stops: the slow step only holds what the quick one cannot see, so the quick one has to keep up. All the way every frame moved the
        // first harmonic of speech by 2.1 cents (limit 2), because the gain then changed too much from one frame to the next.
        levelAmount += 0.3f * ((target.levelMatch ? 1.0f : 0.0f) - levelAmount);
        float fastWanted = 0.0f, slowWanted = 0.0f;
        if ((biggest >= 1.0e-6f || breathing) && levelAmount > 1.0e-3f)
        {
            const int low = (int) (60.0 / binHz), high = std::min (synthHalf, (int) (16000.0 / binHz));
            const int lowBin = (int) (60.0 / (binHz * 0.5)), highBin = std::min (analysisHalf - 1, (int) (16000.0 / (binHz * 0.5)));
            const float noiseWeight = noiseToVoice * gate * gate;
            const auto spectrumWithGain = [&] (double& in, double& out)
            {
                for (int j = lowBin; j <= highBin; ++j)
                {
                    const int k = j / 2;
                    const float g = (j & 1) == 0 ? logGain[(size_t) k] : 0.5f * (logGain[(size_t) k] + logGain[(size_t) k + 1]);
                    in += power[(size_t) j];
                    out += power[(size_t) j] * std::exp (2.0f * g) * (1.0f + (breathing ? noiseWeight * breathShape[(size_t) k] : 0.0f));
                }
            };
            double quickIn = 1.0e-30, quickOut = 1.0e-30;
            if (framesSinceStart < quickFrames)
                spectrumWithGain (quickIn, quickOut);
            else
                for (int k = low; k <= high; ++k)
                {
                    const float before = 2.0f * heard[2 * k];
                    quickIn += std::exp (before);
                    quickOut += std::exp (before + 2.0f * logGain[(size_t) k]);
                }
            fastWanted = juce::jlimit (-1.4f, 1.4f, (float) (0.5 * std::log (quickIn / quickOut)));

            // Both steps are paced for a voice moving through a fixed setting. When the setting itself moves (a control dragged or
            // automated, a preset chosen while audio plays) the gain changes within 40 ms, and the slow step still held the two
            // second average of the old setting: on a real vocal the level ran up to 8 dB over for a tenth of a second. So while a
            // control is moving the slow average starts again at every frame, which makes it match the level of that frame alone,
            // and when the control arrives it builds up as it does from a fresh start. A still setting never gets here, so its sound
            // is the same to the last bit.
            if (framesSinceControlMoved < 2)
                slowEnergyIn = slowEnergyOut = 0.0;
            double energyIn = 0.0, energyOut = 0.0;
            spectrumWithGain (energyIn, energyOut);
            energyOut *= std::exp (2.0f * levelAmount * fastWanted);
            // The slow step holds a two second average, weighted by energy, so the loud vowels of one phrase decide it. On real vocals
            // the next phrase then started with the last phrase's correction: its first 30 ms came out 2 dB quieter (median) at
            // settings that lower the formants, 5 dB at worst. So a frame that stands 10 dB or more over the 100 ms level as it was
            // 43 ms before, which is how a phrase starts after a pause, begins its own average. (Against the level of the frame
            // before, a start that rises over 30 ms was missed.) Inside a phrase nothing changes.
            const double before = recentHistory[(size_t) ((recentPos + 1) % startLookback)];
            if (energyIn > phraseRise * before)
                slowEnergyIn = slowEnergyOut = 0.0;
            if (energyIn > startRise * before || framesSinceControlMoved < 2)
                framesSinceStart = 0;
            else if (framesSinceStart < 1000)
                ++framesSinceStart;
            recentEnergyIn += recentCoef * (energyIn - recentEnergyIn);
            recentPos = (recentPos + 1) % startLookback;
            recentHistory[(size_t) recentPos] = recentEnergyIn;
            slowEnergyIn += levelTimeCoef * (energyIn - slowEnergyIn);
            slowEnergyOut += levelTimeCoef * (energyOut - slowEnergyOut);
            slowWanted = slowEnergyIn > 1.0e-12 ? juce::jlimit (-1.0f, 1.0f, (float) (0.5 * std::log ((slowEnergyIn + 1.0e-20) / (slowEnergyOut + 1.0e-20)))) : slowLevelFix;
        }
        else
        {
            slowEnergyIn = slowEnergyOut = 0.0;
        }
        fastLevelFix += (framesSinceStart < quickFrames ? 0.5f : 0.04f) * (fastWanted - fastLevelFix);
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

        // The formant model is an all-pole fit up to 8 kHz. With no harmonic between the last one and the top of that band to answer
        // to, the fit can put a peak there that the voice does not have: on a real vocal up to 36 dB over the envelope, for 50 ms and
        // more. Read through the map, and carried on above 8 kHz as an offset, that became gain for everything from about 6 kHz up,
        // up to the 26 dB ceiling: a hiss burst on sung vowels at settings that lengthen the tract (take 01 of the Rode takes, 54.5 s,
        // Length 1.20: 6 to 10 kHz 22 to 26 dB over the input). So above 6 kHz the gain, level match included, is held to at most
        // 11 dB over what moving the envelope itself would give. A frame that stays within that is not touched at all.
        if (! identityMap)
        {
            const int from = (int) std::ceil (highBandFromHz / binHz);
            for (int k = from; k <= synthHalf; ++k)
            {
                const float most = envelopeShift[(size_t) k] + highBandHeadroom;
                if (logGain[(size_t) k] > most)
                {
                    wantedOnSynthGrid[(size_t) k] -= logGain[(size_t) k] - most;
                    logGain[(size_t) k] = most;
                }
            }
        }
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
    std::unique_ptr<juce::dsp::FFT> synthFft, analysisFft, cepstrumFft;
    SpectralEnvelope envelope;
    TractModel tract;
    Controls target, current;
    float smoothedTilt = 0.0f, smoothedBreath = 0.0f, smoothedHighPass = 1500.0f, smoothedWidening = 20.0f, settle = 0.1f;
    float levelFix = 0.0f, fastLevelFix = 0.0f, slowLevelFix = 0.0f, levelAmount = 1.0f, tiltVoicing = 0.0f;
    double slowEnergyIn = 0.0, slowEnergyOut = 0.0, levelTimeCoef = 0.03, recentEnergyIn = 0.0, recentCoef = 0.05;
    bool mapIsCurrent = false, identityMap = true, frameIsSilent = true;
    int framesSinceControlMoved = 1000, framesSinceStart = 1000, recentPos = 0;
    std::array<double, startLookback> recentHistory {};
    std::array<std::vector<float>, maxChannels> inputRing, overlap;
    std::vector<juce::dsp::Complex<float>> analysisIn, analysisOut;
    std::vector<float> logPower, voicedEnvelope, cepstrumWork, synthWindow, analysisWindow, power, spectrum, noiseSpectrum, logGain, wanted, finerEnvelope, readHz, mapBefore, breathShape, wantedOnSynthGrid, envelopeShift;
    std::array<std::array<float, 2>, phaseTableSize> phaseTable {};
    std::array<float, maxChannels> channelShare { 1.0f, 1.0f };
    std::array<float, SpectralEnvelope::maxTips> harmonicIn {}, harmonicOut {}, harmonicBin {};
    std::uint32_t noiseState = 0x9E3779B9u;
    Analysis readout;
};
} // namespace formant
