#pragma once
// Shared offline test helpers. Copied into each plug-in repo. Define SMOOTH_COUNT_ALLOCS in exactly one .cpp before including.
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifdef SMOOTH_COUNT_ALLOCS
#include <new>
#include <cstdlib>
namespace testkit { inline std::atomic<long> allocs { 0 }; inline thread_local long threadAllocs = 0; }
void* operator new (std::size_t n) { testkit::allocs.fetch_add (1, std::memory_order_relaxed); ++testkit::threadAllocs; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
#else
namespace testkit { inline std::atomic<long> allocs { 0 }; inline thread_local long threadAllocs = 0; }
#endif

namespace testkit
{
    inline int failures = 0;

    // Prints a line for results.txt. Use for every measured claim.
    inline void check (bool ok, const std::string& what, double measured = std::numeric_limits<double>::quiet_NaN(), const char* unit = "")
    {
        if (! ok) ++failures;
        if (std::isnan (measured)) std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        else std::printf ("%s  %s = %.3f %s\n", ok ? "PASS" : "FAIL", what.c_str(), measured, unit);
        std::fflush (stdout);
    }

    inline void info (const std::string& what, double value, const char* unit = "")
    {
        std::printf ("INFO  %s = %.3f %s\n", what.c_str(), value, unit);
    }

    inline int finish() { std::printf ("%s  %d failures\n", failures ? "RESULT FAIL" : "RESULT PASS", failures); return failures ? 1 : 0; }

    inline double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }

    inline double rms (const float* x, int n)
    {
        double s = 0; for (int i = 0; i < n; ++i) s += (double) x[i] * x[i];
        return std::sqrt (s / std::max (n, 1));
    }
    inline double rmsDb (const juce::AudioBuffer<float>& b, int ch = 0, int from = 0, int len = -1)
    {
        if (len < 0) len = b.getNumSamples() - from;
        return toDb (rms (b.getReadPointer (ch) + from, len));
    }
    inline double peak (const float* x, int n) { double m = 0; for (int i = 0; i < n; ++i) m = std::max (m, (double) std::abs (x[i])); return m; }

    // Level of one frequency in dB relative to a full scale sine (windowed Goertzel).
    inline double toneDb (const float* x, int n, double freq, double sr)
    {
        const double w = juce::MathConstants<double>::twoPi * freq / sr;
        double re = 0, im = 0, win = 0;
        for (int i = 0; i < n; ++i)
        {
            const double h = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1));
            re += h * x[i] * std::cos (w * i); im += h * x[i] * std::sin (w * i); win += h;
        }
        return toDb (2.0 * std::sqrt (re * re + im * im) / win);
    }

    inline void sine (float* x, int n, double freq, double sr, double amp = 1.0, double phase = 0.0)
    {
        for (int i = 0; i < n; ++i) x[i] = (float) (amp * std::sin (phase + juce::MathConstants<double>::twoPi * freq * i / sr));
    }
    inline void noise (float* x, int n, double amp = 1.0, unsigned seed = 1)
    {
        std::mt19937 g (seed); std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        for (int i = 0; i < n; ++i) x[i] = (float) amp * d (g);
    }

    // Largest sample to sample step: a click shows as a step far above the signal's own slope.
    inline double maxJump (const float* x, int n) { double m = 0; for (int i = 1; i < n; ++i) m = std::max (m, (double) std::abs (x[i] - x[i - 1])); return m; }

    inline long countNonFinite (const juce::AudioBuffer<float>& b)
    {
        long c = 0;
        for (int ch = 0; ch < b.getNumChannels(); ++ch) for (int i = 0; i < b.getNumSamples(); ++i) if (! std::isfinite (b.getSample (ch, i))) ++c;
        return c;
    }
    inline long countDenormals (const juce::AudioBuffer<float>& b)
    {
        long c = 0;
        for (int ch = 0; ch < b.getNumChannels(); ++ch) for (int i = 0; i < b.getNumSamples(); ++i) if (std::fpclassify (b.getSample (ch, i)) == FP_SUBNORMAL) ++c;
        return c;
    }

    inline void prepare (juce::AudioProcessor& p, int channels, double sr, int block)
    {
        p.releaseResources();
        auto layout = p.getBusesLayout();
        const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        if (! layout.inputBuses.isEmpty()) layout.inputBuses.getReference (0) = set;
        layout.outputBuses.getReference (0) = set;
        p.setBusesLayout (layout);
        p.setRateAndBufferSizeDetails (sr, block);
        p.prepareToPlay (sr, block);
    }

    // Runs mono input through in blocks (copied to every input channel, side-chain included). Returns output, channel count = outputs.
    inline juce::AudioBuffer<float> render (juce::AudioProcessor& p, const std::vector<float>& input, int block, juce::MidiBuffer* midiFirstBlock = nullptr,
                                            std::function<void (int blockIndex)> beforeBlock = nullptr)
    {
        const int numOut = p.getTotalNumOutputChannels();
        const int numBuf = std::max (p.getTotalNumInputChannels(), numOut);
        const int n = (int) input.size();
        juce::AudioBuffer<float> out (numOut, n), buf (numBuf, block);
        juce::MidiBuffer midi;
        int idx = 0;
        for (int pos = 0; pos < n; pos += block, ++idx)
        {
            const int len = std::min (block, n - pos);
            if (beforeBlock) beforeBlock (idx);
            buf.setSize (numBuf, len, false, false, true);
            for (int ch = 0; ch < numBuf; ++ch) std::copy_n (input.data() + pos, len, buf.getWritePointer (ch));
            midi.clear();
            if (idx == 0 && midiFirstBlock != nullptr) midi = *midiFirstBlock;
            p.processBlock (buf, midi);
            for (int ch = 0; ch < numOut; ++ch) out.copyFrom (ch, pos, buf, ch, 0, len);
        }
        return out;
    }

    struct GenericOptions
    {
        bool sendMidiNote = false;              // instruments and MIDI-played effects
        int minPresets = 2;
        double clickLimit = 0.30;               // biggest allowed step when a control jumps, on a -18 dBFS 220 Hz sine
        std::map<std::string, double> clickLimitFor; // per parameter ID override. Say why in the test file.
        std::vector<std::string> skipClick;     // parameter IDs left out of the click test. Say why in the test file.
        std::vector<std::string> readOnly;      // meters: not touched by sweeps and exempt from the state test
    };

    inline bool has (const std::vector<std::string>& v, const std::string& s) { return std::find (v.begin(), v.end(), s) != v.end(); }

    inline std::string idOf (juce::AudioProcessorParameter* p)
    {
        if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (p)) return w->paramID.toStdString();
        return p->getName (64).toStdString();
    }

    inline bool hasEmDash (const juce::String& s) { return s.containsChar ((juce::juce_wchar) 0x2014); }

    // What every plug-in must pass: names, presets, state, every layout, rate and block size, automation sweeps, clicks, allocations.
    inline void runGenericChecks (std::function<std::unique_ptr<juce::AudioProcessor>()> make, GenericOptions opt = {})
    {
        auto proc = make();
        juce::MidiBuffer note;
        note.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

        // Names
        std::map<std::string, int> seen;
        bool namesOk = true;
        for (auto* prm : proc->getParameters())
        {
            const auto name = prm->getName (64);
            if (name.isEmpty() || hasEmDash (name) || hasEmDash (prm->getLabel()) || seen[name.toStdString()]++ > 0) { namesOk = false; std::printf ("  bad name: %s\n", name.toRawUTF8()); }
        }
        check (namesOk, "parameter names are clear, unique, no em dash");
        info ("parameter count", (double) proc->getParameters().size());

        // Presets
        const int numPresets = proc->getNumPrograms();
        check (numPresets >= opt.minPresets, "factory presets", numPresets);
        bool presetNamesOk = true, presetsDiffer = false;
        juce::MemoryBlock firstState;
        for (int i = 0; i < numPresets; ++i)
        {
            const auto name = proc->getProgramName (i);
            if (name.isEmpty() || hasEmDash (name)) presetNamesOk = false;
            proc->setCurrentProgram (i);
            juce::MemoryBlock st; proc->getStateInformation (st);
            if (i == 0) firstState = st; else if (! (st == firstState)) presetsDiffer = true;
        }
        check (presetNamesOk, "preset names are not empty, no em dash");
        check (presetsDiffer || numPresets < 2, "presets change the state");

        // State round trip
        {
            std::mt19937 g (7); std::uniform_real_distribution<float> d (0.0f, 1.0f);
            for (auto* prm : proc->getParameters()) if (! has (opt.readOnly, idOf (prm))) prm->setValueNotifyingHost (prm->isBoolean() ? std::round (d (g)) : d (g));
            juce::MemoryBlock st; proc->getStateInformation (st);
            auto other = make();
            other->setStateInformation (st.getData(), (int) st.getSize());
            bool same = proc->getParameters().size() == other->getParameters().size();
            for (int i = 0; same && i < proc->getParameters().size(); ++i)
                if (! has (opt.readOnly, idOf (proc->getParameters()[i])) && std::abs (proc->getParameters()[i]->getValue() - other->getParameters()[i]->getValue()) > 1.0e-3f)
                {
                    same = false;
                    std::printf ("  state mismatch: %s saved %.4f restored %.4f\n", idOf (proc->getParameters()[i]).c_str(), proc->getParameters()[i]->getValue(), other->getParameters()[i]->getValue());
                }
            check (same, "state save and restore");
            for (auto* prm : proc->getParameters()) if (! has (opt.readOnly, idOf (prm))) prm->setValueNotifyingHost (prm->getDefaultValue());
        }

        // A damaged saved state must never reach the audio path
        for (const char* badValue : { "nan", "inf", "-inf", "-1e30", "1e30", "-1", "garbage" })
        {
            auto p = make();
            juce::MemoryBlock st; p->getStateInformation (st);
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize()))
            {
                for (auto* e : xml->getChildIterator())
                    if (e->hasAttribute ("value")) e->setAttribute ("value", badValue);
                juce::MemoryBlock bad; juce::AudioProcessor::copyXmlToBinary (*xml, bad);
                p->setStateInformation (bad.getData(), (int) bad.getSize());
                prepare (*p, 2, 48000.0, 512);
                std::vector<float> in (48000);
                sine (in.data(), 48000, 440.0, 48000.0, 0.25);
                auto out = render (*p, in, 512, opt.sendMidiNote ? &note : nullptr);
                double pk = 0; for (int ch = 0; ch < out.getNumChannels(); ++ch) pk = std::max (pk, peak (out.getReadPointer (ch), out.getNumSamples()));
                check (countNonFinite (out) == 0 && pk <= 16.0, std::string ("damaged state value '") + badValue + "' gives finite output");
                bool inRange = true;
                for (auto* prm : p->getParameters()) if (! std::isfinite (prm->getValue()) || prm->getValue() < 0.0f || prm->getValue() > 1.0f) inRange = false;
                check (inRange, std::string ("damaged state value '") + badValue + "' leaves controls in range");
            }
        }

        // Every layout, rate and block size, with all controls moving at once
        for (int channels : { 1, 2 })
        {
            auto p = make();
            auto l = p->getBusesLayout();
            const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
            if (! l.inputBuses.isEmpty()) l.inputBuses.getReference (0) = set;
            l.outputBuses.getReference (0) = set;
            if (! p->checkBusesLayoutSupported (l)) { info (channels == 1 ? "mono not supported" : "stereo not supported", 0); continue; }

            for (double sr : { 44100.0, 48000.0, 96000.0 })
                for (int block : { 32, 512, 4096 })
                {
                    prepare (*p, channels, sr, block);
                    const int n = (int) (sr * 1.0);
                    std::vector<float> in ((size_t) n);
                    noise (in.data(), n, 0.1);
                    std::vector<float> tone ((size_t) n);
                    sine (tone.data(), n, 440.0, sr, 0.2);
                    for (int i = 0; i < n; ++i) in[(size_t) i] += tone[(size_t) i];
                    const int blocks = n / block + 1;
                    long worstAllocs = 0;
                    juce::AudioBuffer<float> out;
                    {
                        long before = 0; bool armed = false; long total = 0;
                        out = render (*p, in, block, opt.sendMidiNote ? &note : nullptr, [&] (int idx)
                        {
                            if (armed) total += allocs.load() - before;
                            int k = 0;
                            for (auto* prm : p->getParameters())
                            {
                                if (! has (opt.readOnly, idOf (prm)))
                                    prm->setValueNotifyingHost (0.5f + 0.5f * (float) std::sin (0.13 * (k + 1) + 6.283 * idx / (double) blocks));
                                ++k;
                            }
                            armed = idx > 0;
                            before = allocs.load();
                        });
                        worstAllocs = total;
                    }
                    const auto tag = std::to_string (channels) + "ch " + std::to_string ((int) sr) + " Hz block " + std::to_string (block);
                    const long bad = countNonFinite (out);
                    if (bad > 0) check (false, "NaN or infinity at " + tag, (double) bad, "samples");
                    if (worstAllocs > 0) check (false, "allocations during audio at " + tag, (double) worstAllocs);
                    double pk = 0; for (int ch = 0; ch < out.getNumChannels(); ++ch) pk = std::max (pk, peak (out.getReadPointer (ch), out.getNumSamples()));
                    if (pk > 16.0) check (false, "runaway level at " + tag, toDb (pk), "dBFS");
                }
            check (true, std::string ("sweeps finished, ") + (channels == 1 ? "mono" : "stereo"));
        }

        // Denormals: signal, then silence, with default settings
        {
            auto p = make();
            prepare (*p, 2, 48000.0, 512);
            std::vector<float> in (48000 * 6, 0.0f);
            sine (in.data(), 48000, 1000.0, 48000.0, 0.5);
            auto out = render (*p, in, 512, opt.sendMidiNote ? &note : nullptr);
            check (countNonFinite (out) == 0, "no NaN after silence");
            check (countDenormals (out) == 0, "no denormals decaying into silence", (double) countDenormals (out), "samples");
        }

        // Bad input from the host or an upstream plug-in must not stick: burst of NaN, inf and huge values, then recovery
        {
            auto p = make();
            prepare (*p, 2, 48000.0, 512);
            std::vector<float> in (48000 * 6, 0.0f);
            sine (in.data(), 24000, 440.0, 48000.0, 0.25);
            const float badInput[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                       1.0e38f, std::numeric_limits<float>::max() * 0.5f, -1.0e38f };
            for (int i = 0; i < 512; ++i) in[(size_t) (24000 + i)] = badInput[(size_t) i % 6];
            sine (in.data() + 48000 * 5, 48000, 440.0, 48000.0, 0.25);
            auto out = render (*p, in, 512, opt.sendMidiNote ? &note : nullptr);
            juce::AudioBuffer<float> tail (out.getNumChannels(), 48000);
            for (int ch = 0; ch < out.getNumChannels(); ++ch) tail.copyFrom (ch, 0, out, ch, 48000 * 5, 48000);
            double pk = 0; for (int ch = 0; ch < tail.getNumChannels(); ++ch) pk = std::max (pk, peak (tail.getReadPointer (ch), 48000));
            check (countNonFinite (tail) == 0 && pk <= 16.0, "recovers after NaN, inf and huge input, peak in the last second", pk);
        }

        // Clicks: jump each control to its ends during a steady low sine
        {
            auto p = make();
            prepare (*p, 2, 48000.0, 256);
            std::vector<float> in (48000);
            sine (in.data(), 48000, 220.0, 48000.0, 0.125);
            render (*p, in, 256, opt.sendMidiNote ? &note : nullptr);
            auto base = render (*p, in, 256);
            info ("click test baseline step", maxJump (base.getReadPointer (0), base.getNumSamples()));
            for (auto* prm : p->getParameters())
            {
                const auto id = idOf (prm);
                if (has (opt.skipClick, id) || has (opt.readOnly, id)) continue;
                const float def = prm->getValue();
                auto out = render (*p, in, 256, nullptr, [&] (int idx) { if (idx % 37 == 18) prm->setValueNotifyingHost (idx % 74 < 37 ? 1.0f : 0.0f); });
                prm->setValueNotifyingHost (def);
                render (*p, in, 256);
                const double limit = opt.clickLimitFor.count (id) ? opt.clickLimitFor[id] : opt.clickLimit;
                const double j = maxJump (out.getReadPointer (0), out.getNumSamples());
                check (j <= limit, "no click when " + prm->getName (64).toStdString() + " jumps, biggest step", j);
            }
        }
    }
}
