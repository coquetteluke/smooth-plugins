#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cstring>
#include <vector>

// The same halfband FIR oversampling as juce::dsp::Oversampling with filterHalfBandFIREquiripple at maximum quality:
// the same filter design, the same latency and the same sum order, so its output matches sample for sample.
// It is faster because the history slides once per call instead of once per sample, and four outputs are worked out at a time.
class FastOversampler
{
public:
    FastOversampler (size_t channels, size_t numStages, bool useIntegerLatency)
        : numChannels (channels), integerLatency (useIntegerLatency)
    {
        stages.resize (numStages);
        for (size_t n = 0; n < numStages; ++n)
        {
            const float scale = n == 0 ? 0.5f : 1.0f;
            design (stages[n].up, 0.10f * scale, -90.0f + 10.0f * (float) n);
            design (stages[n].down, 0.12f * scale, -75.0f + 10.0f * (float) n);
            stages[n].latency = (float) (stages[n].up.order + stages[n].down.order) * 0.5f;
        }
    }

    void initProcessing (size_t maxSamples)
    {
        size_t frames = maxSamples;
        for (auto& s : stages)
        {
            s.buffer.setSize ((int) numChannels, (int) frames * 2, false, false, true);
            s.up.prepare (numChannels, frames);
            s.down.prepare (numChannels, frames);
            s.ring.assign (numChannels * ((size_t) s.down.ringSize), 0.0f);
            s.ringPos.assign (numChannels, 0);
            frames *= 2;
        }
        if (stages.empty())
            passBuffer.setSize ((int) numChannels, (int) frames, false, false, true);

        delay.prepare ({ 0.0, (juce::uint32) maxSamples, (juce::uint32) numChannels });
        const float latency = getUncompensatedLatency();
        fractionalDelay = 1.0f - (latency - std::floor (latency));
        if (juce::approximatelyEqual (fractionalDelay, 1.0f))
            fractionalDelay = 0.0f;
        else if (fractionalDelay < 0.618f)
            fractionalDelay += 1.0f;
        delay.setDelay (fractionalDelay);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : stages)
        {
            s.buffer.clear();
            std::fill (s.up.history.begin(), s.up.history.end(), 0.0f);
            std::fill (s.down.history.begin(), s.down.history.end(), 0.0f);
            std::fill (s.ring.begin(), s.ring.end(), 0.0f);
            std::fill (s.ringPos.begin(), s.ringPos.end(), 0);
        }
        delay.reset();
    }

    float getLatencyInSamples() const noexcept
    {
        const float latency = getUncompensatedLatency();
        return integerLatency ? latency + fractionalDelay : latency;
    }

    juce::dsp::AudioBlock<float> processSamplesUp (const juce::dsp::AudioBlock<const float>& input) noexcept
    {
        const int n = (int) input.getNumSamples();
        if (stages.empty())
        {
            for (size_t ch = 0; ch < input.getNumChannels(); ++ch)
                passBuffer.copyFrom ((int) ch, 0, input.getChannelPointer (ch), n);
            return juce::dsp::AudioBlock<float> (passBuffer).getSubBlock (0, (size_t) n);
        }

        int frames = n;
        for (size_t st = 0; st < stages.size(); ++st)
        {
            auto& s = stages[st];
            for (size_t ch = 0; ch < input.getNumChannels(); ++ch)
            {
                const float* in = st == 0 ? input.getChannelPointer (ch) : stages[st - 1].buffer.getReadPointer ((int) ch);
                float* history = s.up.historyFor (ch);
                float* newest = history + s.up.window - 1;
                for (int i = 0; i < frames; ++i)
                    newest[i] = 2 * in[i];
                float* out = s.buffer.getWritePointer ((int) ch);
                filterFrames (history, s.up, frames, s.up.scratch.data());
                for (int i = 0; i < frames; ++i)
                {
                    out[i << 1] = s.up.scratch[(size_t) i];
                    out[(i << 1) + 1] = history[i + s.up.centreTap] * s.up.centre;
                }
                std::memmove (history, history + frames, sizeof (float) * (size_t) (s.up.window - 1));
            }
            frames *= 2;
        }
        return juce::dsp::AudioBlock<float> (stages.back().buffer).getSubBlock (0, (size_t) frames);
    }

    void processSamplesDown (juce::dsp::AudioBlock<float>& output) noexcept
    {
        const int n = (int) output.getNumSamples();
        if (stages.empty())
        {
            for (size_t ch = 0; ch < output.getNumChannels(); ++ch)
                juce::FloatVectorOperations::copy (output.getChannelPointer (ch), passBuffer.getReadPointer ((int) ch), n);
        }
        else
        {
            int frames = n << (stages.size() - 1);
            for (size_t st = stages.size(); st-- > 0;)
            {
                auto& s = stages[st];
                for (size_t ch = 0; ch < output.getNumChannels(); ++ch)
                {
                    const float* in = s.buffer.getReadPointer ((int) ch);
                    float* out = st == 0 ? output.getChannelPointer (ch) : stages[st - 1].buffer.getWritePointer ((int) ch);
                    float* history = s.down.historyFor (ch);
                    float* newest = history + s.down.window - 1;
                    for (int i = 0; i < frames; ++i)
                        newest[i] = in[i << 1];
                    filterFrames (history, s.down, frames, out);
                    float* ring = s.ring.data() + ch * (size_t) s.down.ringSize;
                    int pos = s.ringPos[ch];
                    for (int i = 0; i < frames; ++i)
                    {
                        out[i] += ring[pos] * s.down.centre;
                        ring[pos] = in[(i << 1) + 1];
                        pos = pos == 0 ? s.down.ringSize - 1 : pos - 1;
                    }
                    s.ringPos[ch] = pos;
                    std::memmove (history, history + frames, sizeof (float) * (size_t) (s.down.window - 1));
                }
                frames >>= 1;
            }
        }

        if (integerLatency && fractionalDelay > 0.0f)
        {
            juce::dsp::ProcessContextReplacing<float> context (output);
            delay.process (context);
        }
    }

private:
    // One halfband filter. Only every other tap is non zero, plus the centre, and the filter is symmetrical.
    // The window holds just the samples those taps touch, oldest first, and pair[m] applies to window[m] and window[window - 1 - m].
    struct Filter
    {
        std::vector<float> pair, history, scratch;
        float centre = 0.5f;
        int order = 0, pairs = 0, window = 0, centreTap = 0, ringSize = 1;
        size_t stride = 0;

        void prepare (size_t channels, size_t frames)
        {
            stride = (size_t) window - 1 + frames;
            history.assign (channels * stride, 0.0f);
            scratch.assign (frames, 0.0f);
        }
        float* historyFor (size_t ch) noexcept { return history.data() + ch * stride; }
    };

    struct Stage
    {
        Filter up, down;
        juce::AudioBuffer<float> buffer;
        std::vector<float> ring;
        std::vector<int> ringPos;
        float latency = 0.0f;
    };

    static void design (Filter& f, float transition, float stopbandDb)
    {
        auto coefficients = juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod (transition, stopbandDb);
        const auto* c = coefficients->getRawCoefficients();
        f.order = (int) coefficients->getFilterOrder();
        const int half = (f.order + 1) / 2;
        f.pairs = (half + 1) / 2;
        f.window = 2 * f.pairs;
        f.centreTap = (half + 1) / 2;
        f.centre = c[half];
        f.ringSize = half / 2 + 1;
        f.pair.resize ((size_t) f.pairs);
        for (int m = 0; m < f.pairs; ++m)
            f.pair[(size_t) m] = c[2 * m];
    }

    // out[i] = sum over m of (w[i + m] + w[i + window - 1 - m]) * pair[m], summed in the order of m, as the scalar form does.
    static void filterFrames (const float* w, const Filter& f, int frames, float* out) noexcept
    {
        using V = float __attribute__ ((vector_size (16), aligned (4), may_alias));
        const float* pair = f.pair.data();
        const int pairs = f.pairs, last = f.window - 1;
        int i = 0;
        for (; i + 4 <= frames; i += 4)
        {
            V sum = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int m = 0; m < pairs; ++m)
            {
                const float c = pair[m];
                sum = __builtin_elementwise_fma (*(const V*) (w + i + m) + *(const V*) (w + i + last - m), V { c, c, c, c }, sum);
            }
            *(V*) (out + i) = sum;
        }
        for (; i < frames; ++i)
        {
            float sum = 0.0f;
            for (int m = 0; m < pairs; ++m)
                sum += (w[i + m] + w[i + last - m]) * pair[m];
            out[i] = sum;
        }
    }

    float getUncompensatedLatency() const noexcept
    {
        float latency = 0.0f;
        size_t order = 1;
        for (const auto& s : stages)
        {
            order *= 2;
            latency += s.latency / (float) order;
        }
        return latency;
    }

    size_t numChannels;
    bool integerLatency;
    std::vector<Stage> stages;
    juce::AudioBuffer<float> passBuffer;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Thiran> delay { 8 };
    float fractionalDelay = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FastOversampler)
};
