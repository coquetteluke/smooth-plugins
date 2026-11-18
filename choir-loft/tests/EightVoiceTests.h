#pragma once
// Voices 5 to 8 and click to hear. Included by PluginTest.cpp, inside its namespace. New voices are held to the same limits as the first four.

namespace eightvoices
{
// Nothing but the plain pitch, as clean() does, with all eight singing and only this one heard through Level.
inline std::unique_ptr<PluginProcessor> cleanEight (int mode, int only, float interval)
{
    auto p = clean (mode, 4);
    setParam (*p, "moreVoices", 4.0f);
    for (int v = 0; v < 8; ++v)
    {
        setParam (*p, vid (v, "Level"), v == only ? 0.0f : -60.0f);
        setParam (*p, vid (v, "Pan"), 0.0f);
        setParam (*p, vid (v, "Drift"), 0.0f);
        setParam (*p, vid (v, "Throat"), 100.0f);
        setParam (*p, vid (v, "Tone"), 0.0f);
        setParam (*p, vid (v, "Interval"), interval);
    }
    return p;
}

inline std::vector<float> aloneDefaults (const std::vector<float>& x, int v, int interval)
{
    auto p = make();
    setParam (*p, "mode", (float) modeFixed);
    setParam (*p, "voices", 4.0f);
    setParam (*p, "moreVoices", 4.0f);
    setParam (*p, "leadLevel", -60.0f);
    for (int i = 0; i < 8; ++i)
    {
        setParam (*p, vid (i, "Level"), i == v ? 0.0f : -60.0f);
        setParam (*p, vid (i, "Pan"), 0.0f);
        setParam (*p, vid (i, "Interval"), (float) interval);
    }
    prepare (*p, 1, SR, 256);
    return mono (render (*p, x, 256));
}

inline double peakOf (const juce::AudioBuffer<float>& b)
{
    return (double) b.getMagnitude (0, b.getNumSamples());
}

// How long a level takes to go from 90% to 10% of where it started, on short RMS frames, in ms. From a known moment, on a steady tone.
inline double fadeMs (const std::vector<float>& o, long at, double before)
{
    const int w = (int) (0.002 * SR);
    long start = -1, end = -1;
    for (long i = at - (long) (0.002 * SR); i + w < (long) o.size() && i < at + (long) (0.1 * SR); i += w / 4)
    {
        const double level = rms (o.data() + i, w);
        if (start < 0 && level < 0.9 * before) start = i;
        if (start >= 0 && level < 0.1 * before) { end = i; break; }
    }
    return start >= 0 && end >= 0 ? (double) (end - start) / SR * 1000.0 / 0.8 : -1.0;   // 90% to 10% of a straight fade is 80% of it
}
}

void testEightVoices()
{
    using namespace eightvoices;
    const auto x = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
    const double fifthUp = 220.0 * std::pow (2.0, 7.0 / 12.0);

    // ---- parameters: old IDs stay, new ones are there, and defaults use the same number of voices
    {
        auto p = make();
        check (p->voiceCount() == 2, "defaults still use two voices", p->voiceCount());
        auto* first = dynamic_cast<juce::AudioParameterInt*> (p->apvts.getParameter ("voices"));
        check (first != nullptr && first->getRange().getStart() == 1 && first->getRange().getEnd() == 4, "Voices keeps its range of 1 to 4, so old automation of it stays right");
        for (int v = 4; v < 8; ++v)
            for (const char* what : { "Interval", "Level", "Pan", "Drift", "Throat", "Tone", "On" })
                check (p->apvts.getParameter (vid (v, what)) != nullptr, "voice " + std::to_string (v + 1) + " has " + what);
        for (int v = 0; v < 8; ++v)
            check (p->apvts.getRawParameterValue (vid (v, "On"))->load() >= 0.5f, "voice " + std::to_string (v + 1) + " starts on");
        check (p->apvts.getRawParameterValue ("leadOn")->load() >= 0.5f, "the lead starts on");
    }

    // ---- presets
    {
        auto p = make();
        check (p->getProgramName (tightPreset) == "Tight", "old presets keep their places");
        int big = 0;
        for (int i = 0; i < p->getNumPrograms(); ++i)
        {
            setParam (*p, "leadOn", 0.0f);
            setParam (*p, vid (6, "On"), 0.0f);
            p->setCurrentProgram (i);
            if (p->voiceCount() > 4)
                ++big;
            bool allOn = p->apvts.getRawParameterValue ("leadOn")->load() >= 0.5f;
            for (int v = 0; v < 8; ++v)
                allOn = allOn && p->apvts.getRawParameterValue (vid (v, "On"))->load() >= 0.5f;
            check (allOn, "preset " + p->getProgramName (i).toStdString() + " puts every singer on");
        }
        check (big >= 2, "at least 2 presets use more than 4 voices", big);
    }

    // ---- pitch, Level, Pan, Drift for each new voice
    for (int v = 4; v < 8; ++v)
    {
        const auto name = "voice " + std::to_string (v + 1);
        const auto render1 = [&] (std::function<void (PluginProcessor&)> setup, int channels = 1)
        {
            auto p = cleanEight (modeFixed, v, 7.0f);
            setup (*p);
            prepare (*p, channels, SR, 256);
            return render (*p, x, 256);
        };
        const auto steadyOut = mono (render1 ([] (PluginProcessor&) {}));
        check (pitchError (steadyOut, fifthUp, 0.3, 1.9) <= limits::voicePitchCents, name + " pitch, Fixed +7, 95th percentile", pitchError (steadyOut, fifthUp, 0.3, 1.9), "cents");

        // In the key, on a C: a third up is E.
        {
            const auto c = voice::synth (voice::steady (noteHz (60.0), 2.0, SR), voice::vowelA, SR);
            auto p = cleanEight (modeScale, v, 4.0f);
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, c, 256));
            check (pitchError (o, noteHz (64.0), 0.3, 1.9) <= limits::voicePitchCents, name + " pitch, In Key, a 3rd up on C is E", pitchError (o, noteHz (64.0), 0.3, 1.9), "cents");
        }

        const double base = rmsDb (render1 ([] (PluginProcessor&) {}), 0, 48000, 40000);
        const double down = rmsDb (render1 ([&] (PluginProcessor& p) { setParam (p, vid (v, "Level"), -12.0f); }), 0, 48000, 40000) - base;
        check (std::abs (down + 12.0) <= limits::levelDb, name + " Level -12 dB measures", down, "dB");

        const auto hard = render1 ([&] (PluginProcessor& p) { setParam (p, vid (v, "Pan"), -100.0f); }, 2);
        check (rmsDb (hard, 1, 48000, 40000) - rmsDb (hard, 0, 48000, 40000) <= limits::panHardDb, name + " Pan full left: right against left", rmsDb (hard, 1, 48000, 40000) - rmsDb (hard, 0, 48000, 40000), "dB");
        const auto centred = render1 ([] (PluginProcessor&) {}, 2);
        check (std::abs (rmsDb (hard, 0, 48000, 40000) - rmsDb (centred, 0, 48000, 40000) - 3.01) <= limits::panLawDb, name + " Pan full left is 3 dB up", rmsDb (hard, 0, 48000, 40000) - rmsDb (centred, 0, 48000, 40000), "dB");

        // Drift: as far as the first four go, and no further
        {
            const auto long1 = voice::synth (voice::steady (220.0, 30.0, SR), voice::vowelA, SR);
            auto p = cleanEight (modeFixed, v, 7.0f);
            setParam (*p, vid (v, "Drift"), 100.0f);
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, long1, 256));
            std::vector<double> frames;
            for (double t = 0.3; t < 29.9; t += 0.02)
            {
                const double f = pitchNear (o.data(), (long) o.size(), (long) (t * SR), fifthUp, SR, 0.06, 3.0, 0.04);
                if (f > 0.0) frames.push_back (cents (f, fifthUp));
            }
            const double wander = percentile (absolute (frames), 99.0);
            check (wander >= limits::driftFullMinCents && wander <= limits::driftFullMaxCents, name + " Drift 100: pitch wander over 30 s", wander, "cents");
        }
    }

    // ---- decorrelation: on one interval, new voices against each other and against the old ones
    {
        const auto steady = voice::synth (voice::steady (220.0, 10.0, SR), voice::vowelA, SR);
        double worstBest = 0.0, worstZero = 0.0, worstRise = 0.0;
        const std::array<std::array<int, 2>, 6> pairs { { { 3, 4 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 0, 7 }, { 0, 4 } } };
        for (const auto& pair : pairs)
        {
            const auto s = copyScore (aloneDefaults (steady, pair[0], 7), aloneDefaults (steady, pair[1], 7), 0.5);
            worstBest = std::max (worstBest, s.best);
            worstZero = std::max (worstZero, s.zero);
            worstRise = std::max (worstRise, s.rise);
        }
        check (worstBest <= limits::copyBestLagCorrelation, "new voices on one interval: correlation at the best lag, worst of 6 pairs", worstBest);
        check (worstZero <= limits::copyZeroLagCorrelation, "  and at no lag", worstZero);
        check (worstRise <= limits::copySumRiseDb, "  level rise from adding the second voice (6 dB is a copy, 3 dB unrelated)", worstRise, "dB");
    }

    // ---- MIDI: eight notes on eight voices
    {
        const auto notes = [] (MidiChord& c) { std::array<int, 8> n {}; for (int v = 0; v < 8; ++v) n[(size_t) v] = c.noteFor (v); return n; };
        MidiChord c;
        for (int n : { 48, 55, 60, 64, 67, 72, 76, 79 }) c.noteOn (1, n);
        c.assignChord (8);
        check (notes (c) == std::array<int, 8> { 48, 55, 60, 64, 67, 72, 76, 79 }, "8 notes on 8 voices: lowest note on voice 1, in order");

        // A held note keeps its voice. Voice moves are the least in total: the same answer as trying every way, on 6 voices, where trying is still quick.
        juce::Random random (12);
        int crossings = 0, wrongNotes = 0, moved = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int trial = 0; trial < 2000; ++trial)
        {
            MidiChord d;
            std::array<int, 8> first {};
            for (int v = 0; v < 8; ++v) { first[(size_t) v] = 48 + 3 * v + random.nextInt (3); d.noteOn (1, first[(size_t) v]); }
            d.assignChord (8);
            const int change = random.nextInt (8);
            d.noteOff (1, first[(size_t) change]);
            int fresh = first[(size_t) change] + 1;
            bool clash = false;
            for (int n : first) clash = clash || n == fresh;
            d.noteOn (1, clash ? fresh + 40 : fresh);
            d.assignChord (8);
            const auto after = notes (d);
            int last = -1, count = 0;
            for (int v = 0; v < 8; ++v)
                if (after[(size_t) v] >= 0)
                {
                    ++count;
                    if (after[(size_t) v] < last) ++crossings;
                    last = after[(size_t) v];
                }
            if (count != 8) ++wrongNotes;
            for (int v = 0; v < 8; ++v)
                if (v != change && ! clash && after[(size_t) v] != first[(size_t) v]) ++moved;
        }
        const double perAssign = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count() / 4000.0;
        check (crossings == 0, "8 voices, 2000 random chord changes: no voice crossings", crossings);
        check (wrongNotes == 0, "8 voices: every note gets a voice", wrongNotes);
        check (moved == 0, "8 voices: moving one note moves only that voice", moved);
        check (perAssign < 0.0002, "8 voices: working out a chord takes well under one block", perAssign * 1.0e6, "us");

        MidiChord e;
        e.noteOn (5, 60); e.noteOn (8, 72);
        e.assignChannels (8);
        check (notes (e) == std::array<int, 8> { -1, -1, -1, -1, 60, -1, -1, 72 }, "MIDI Channel: voice 5 takes channel 5, voice 8 takes channel 8");
    }

    // ---- the mix: above 4 voices the voices turn down together
    {
        for (int more = 1; more <= 4; ++more)
        {
            auto p = cleanEight (modeFixed, 0, 7.0f);
            setParam (*p, "moreVoices", (float) more);
            prepare (*p, 1, SR, 256);
            auto q = cleanEight (modeFixed, 0, 7.0f);
            setParam (*q, "moreVoices", 0.0f);
            prepare (*q, 1, SR, 256);
            const double got = rmsDb (render (*p, x, 256), 0, 48000, 40000) - rmsDb (render (*q, x, 256), 0, 48000, 40000);
            const double want = 10.0 * std::log10 (4.0 / (4.0 + more));
            check (std::abs (got - want) <= limits::trimDb, "voices together at " + std::to_string (4 + more) + " voices turn down by", got, "dB");
        }

        // Eight against four at the same Level, on unrelated-sounding voices.
        const auto x8 = voice::synth (voice::steady (220.0, 8.0, SR), voice::vowelA, SR);
        const auto stack = [&] (int more)
        {
            auto p = make();
            setParam (*p, "voices", 4.0f);
            setParam (*p, "moreVoices", (float) more);
            setParam (*p, "leadLevel", -60.0f);
            for (int v = 0; v < 8; ++v)
                setParam (*p, vid (v, "Level"), -6.0f);
            prepare (*p, 2, SR, 256);
            return rmsDb (render (*p, x8, 256), 0, 48000, 280000);
        };
        const double rise = stack (4) - stack (0);
        check (std::abs (rise) <= limits::eightAgainstFourDb, "8 voices against 4 at one Level, RMS", rise, "dB");

        // The presets with more than four, and the defaults at 8, on a loud real vocal: input peaking at -6 dBFS.
        auto sung = human::loadCorpus ("sung_goodnews", 9.0);
        double top = 0.0;
        for (const auto v : sung) top = std::max (top, (double) std::abs (v));
        for (auto& v : sung) v = (float) (v * 0.5 / top);
        auto probe = make();
        for (int i = tightPreset + 1; i < probe->getNumPrograms(); ++i)
        {
            auto p = make();
            p->setCurrentProgram (i);
            prepare (*p, 2, SR, 256);
            juce::MidiBuffer chord;
            for (int n : { 48, 55, 60, 64, 67, 72, 76, 79 }) chord.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), 0);
            const double peak = peakOf (render (*p, sung, 256, &chord));
            check (peak < limits::newPresetPeak, "preset " + p->getProgramName (i).toStdString() + ", vocal peaking at -6 dBFS: output peak", peak);
        }
        {
            auto p = make();
            setParam (*p, "voices", 4.0f);
            setParam (*p, "moreVoices", 4.0f);
            prepare (*p, 2, SR, 256);
            info ("defaults with Voices set to 8, vocal peaking at -6 dBFS: output peak, for the report", peakOf (render (*p, sung, 256)));
            auto q = make();
            prepare (*q, 2, SR, 256);
            info ("defaults, two voices, same vocal: output peak, for the report", peakOf (render (*q, sung, 256)));
        }
    }

    // ---- click to hear
    const auto steadyLong = voice::synth (voice::steady (220.0, 6.0, SR), voice::vowelA, SR);
    const long clickFrom = (long) (0.3 * SR), clickTo = (long) (5.9 * SR);
    const auto reference = [&]
    {
        auto p = cleanEight (modeFixed, 0, 7.0f);
        for (int v = 0; v < 8; ++v) { setParam (*p, vid (v, "Level"), 0.0f); setParam (*p, vid (v, "Interval"), (float) (2 + v)); }
        prepare (*p, 1, SR, 256);
        return clickScore (mono (render (*p, steadyLong, 256)), clickFrom, clickTo);
    }();

    {
        // Each of the eight switching often, on its own, and all together, and the lead.
        const auto report = [&] (const std::string& name, const std::vector<float>& o)
        {
            const auto s = clickScore (o, clickFrom, clickTo);
            check (s.jump <= limits::clickJumpRatio * reference.jump, "no click: " + name + ", biggest step against steady", s.jump / reference.jump, "times");
            check (s.curve <= limits::clickCurveRatio * reference.curve, "no click: " + name + ", second difference against steady", s.curve / reference.curve, "times");
        };
        for (int only = 0; only < 8; ++only)
        {
            auto p = cleanEight (modeFixed, only, 7.0f);
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, steadyLong, 256, nullptr, [&] (int block) { if (block % 40 == 20) setParam (*p, vid (only, "On"), block % 80 < 40 ? 0.0f : 1.0f); }));
            report ("voice " + std::to_string (only + 1) + " switching", o);

            // Against that same voice alone and steady: much harder than against all eight.
            auto q = cleanEight (modeFixed, only, 7.0f);
            prepare (*q, 1, SR, 256);
            const auto own = clickScore (mono (render (*q, steadyLong, 256)), clickFrom, clickTo);
            const auto s = clickScore (o, clickFrom, clickTo);
            check (s.jump <= limits::clickJumpRatio * own.jump, "no click: voice " + std::to_string (only + 1) + " switching, against itself steady", s.jump / own.jump, "times");
        }
        {
            auto p = cleanEight (modeFixed, 0, 7.0f);
            for (int v = 0; v < 8; ++v) { setParam (*p, vid (v, "Level"), 0.0f); setParam (*p, vid (v, "Interval"), (float) (2 + v)); }
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, steadyLong, 256, nullptr, [&] (int block)
            {
                if (block % 30 == 15)
                    for (int v = 0; v < 8; ++v)
                        setParam (*p, vid (v, "On"), (block / 30 + v) % 2 == 0 ? 1.0f : 0.0f);
            }));
            report ("half the singers switching at once", o);
        }
        {
            auto p = make();
            setParam (*p, "voices", 1.0f);
            setParam (*p, vid (0, "Level"), -60.0f);
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, steadyLong, 256, nullptr, [&] (int block) { if (block % 40 == 20) setParam (*p, "leadOn", block % 80 < 40 ? 0.0f : 1.0f); }));
            const auto s = clickScore (o, clickFrom, clickTo);
            const auto plain = clickScore (std::vector<float> (steadyLong), clickFrom, clickTo);
            check (s.jump <= limits::clickJumpRatio * plain.jump, "no click: the lead switching, biggest step against the input", s.jump / plain.jump, "times");
        }
        {
            auto p = cleanEight (modeFixed, 0, 7.0f);
            for (int v = 0; v < 8; ++v) { setParam (*p, vid (v, "Level"), 0.0f); setParam (*p, vid (v, "Interval"), (float) (2 + v)); }
            prepare (*p, 1, SR, 256);
            const auto o = mono (render (*p, steadyLong, 256, nullptr, [&] (int block)
            {
                if (block % 40 == 20)
                {
                    setParam (*p, "voices", block % 80 < 40 ? 4.0f : 1.0f);
                    setParam (*p, "moreVoices", block % 80 < 40 ? 4.0f : 0.0f);
                }
            }));
            report ("the voice count jumping between 1 and 8", o);
        }
    }

    // How long a switch takes, and what is left after it
    {
        const auto fadeOf = [&] (const char* id, std::function<void (PluginProcessor&)> setup, bool lead)
        {
            auto p = lead ? make() : cleanEight (modeFixed, 5, 7.0f);
            setup (*p);
            prepare (*p, 1, SR, 256);
            const long at = 48000 / 256 * 256;
            juce::AudioBuffer<float> buf (1, 256);
            juce::MidiBuffer midi;
            std::vector<float> o;
            for (size_t pos = 0; pos + 256 <= x.size(); pos += 256)
            {
                if ((long) pos == at)
                    setParam (*p, id, 0.0f);
                std::copy_n (x.data() + pos, 256, buf.getWritePointer (0));
                p->processBlock (buf, midi);
                o.insert (o.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 256);
            }
            const double before = rms (o.data() + at - 24000, 24000);
            const double after = 20.0 * std::log10 (std::max (rms (o.data() + at + 9600, 24000) / before, 1.0e-12));
            return std::make_pair (fadeMs (o, at, before), after);
        };
        const auto voiceFade = fadeOf ("voice6On", [] (PluginProcessor&) {}, false);
        check (voiceFade.first >= limits::hearFadeMinMs && voiceFade.first <= limits::hearFadeMaxMs, "a voice switching off fades over", voiceFade.first, "ms");
        check (voiceFade.second <= limits::hearOffDb, "and then there is nothing left of it", voiceFade.second, "dB");
        const auto leadFade = fadeOf ("leadOn", [] (PluginProcessor& q) { setParam (q, "voices", 1.0f); setParam (q, vid (0, "Level"), -60.0f); }, true);
        check (leadFade.first >= limits::hearFadeMinMs && leadFade.first <= limits::hearFadeMaxMs, "the lead switching off fades over", leadFade.first, "ms");
        check (leadFade.second <= limits::hearOffDb, "and then there is nothing left of it", leadFade.second, "dB");
    }

    // Off is the same as Level Off for what you hear: a selection really is just those singers.
    {
        auto a = make();
        auto b = make();
        for (auto* p : { a.get(), b.get() })
        {
            setParam (*p, "voices", 4.0f);
            setParam (*p, "moreVoices", 4.0f);
        }
        for (int v : { 1, 3, 4, 6 })
        {
            setParam (*a, vid (v, "On"), 0.0f);
            setParam (*b, vid (v, "Level"), -60.0f);
        }
        setParam (*a, "leadOn", 0.0f);
        setParam (*b, "leadLevel", -60.0f);
        prepare (*a, 2, SR, 256);
        prepare (*b, 2, SR, 256);
        auto sa = render (*a, steadyLong, 256);
        const auto sb = render (*b, steadyLong, 256);
        sa.addFrom (0, 0, sb, 0, 0, sb.getNumSamples(), -1.0f);
        const double residual = toDb (rms (sa.getReadPointer (0) + 4800, sa.getNumSamples() - 4800));
        check (residual <= limits::muteNullDb, "singers off against the same singers at Level Off: what is left", residual, "dBFS");
        check (rmsDb (sb, 0, 48000, 200000) > -60.0, "and the rest are still singing", rmsDb (sb, 0, 48000, 200000), "dB");
    }

    // A voice that comes back is on the right note at once
    {
        const auto c = voice::synth (voice::steady (220.0, 4.0, SR), voice::vowelA, SR);
        for (int v : { 0, 6 })
        {
            auto p = cleanEight (modeFixed, v, 7.0f);
            setParam (*p, vid (v, "On"), 0.0f);
            prepare (*p, 1, SR, 256);
            const int backAt = 2 * 48000 / 256;
            const auto o = mono (render (*p, c, 256, nullptr, [&] (int block) { if (block == backAt) setParam (*p, vid (v, "On"), 1.0f); }));
            const double t = (double) backAt * 256.0 / SR + 0.05;
            const double f = pitchNear (o.data(), (long) o.size(), (long) ((t + 0.03) * SR), fifthUp, SR, 0.06, 3.0, 0.04);
            check (f > 0.0 && std::abs (cents (f, fifthUp)) <= limits::comeBackCents, "voice " + std::to_string (v + 1) + " coming back: pitch 50 ms later against its note", f > 0.0 ? std::abs (cents (f, fifthUp)) : 999.0, "cents");
        }
    }

    // Off singers stop using CPU
    {
        const auto x4 = voice::synth (voice::vibrato (180.0, 4.0, SR, 30.0, 5.0), voice::vowelA, SR);
        const auto timed = [&] (int off)
        {
            auto p = make();
            setParam (*p, "voices", 4.0f);
            setParam (*p, "moreVoices", 4.0f);
            for (int v = 8 - off; v < 8; ++v)
                setParam (*p, vid (v, "On"), 0.0f);
            prepare (*p, 2, SR, 256);
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer midi;
            double best = 1.0e9;
            for (int run = 0; run < 3; ++run)
            {
                const auto start = std::chrono::steady_clock::now();
                for (size_t pos = 0; pos + 256 <= x4.size(); pos += 256)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        std::copy_n (x4.data() + pos, 256, buf.getWritePointer (ch));
                    p->processBlock (buf, midi);
                }
                best = std::min (best, std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
            }
            return best / (x4.size() / SR);
        };
        const double all = timed (0), two = timed (6);
        const double share = (two - all * 2.0 / 8.0) / std::max (all * 6.0 / 8.0, 1.0e-9);
        info ("CPU, one core, eight singers on", all * 100.0, "%");
        info ("CPU, one core, two of eight on", two * 100.0, "%");
        check (share <= limits::offVoiceCpuShare, "off singers' cost, as a share of what they cost on", share);
    }

    // Saved with the session, and sessions from before keep sounding the same
    {
        auto p = make();
        setParam (*p, "moreVoices", 3.0f);
        setParam (*p, vid (6, "On"), 0.0f);
        setParam (*p, "leadOn", 0.0f);
        juce::MemoryBlock saved;
        p->getStateInformation (saved);
        auto q = make();
        q->setStateInformation (saved.getData(), (int) saved.getSize());
        check (q->voiceCount() == 5 && q->apvts.getRawParameterValue (vid (6, "On"))->load() < 0.5f && q->apvts.getRawParameterValue ("leadOn")->load() < 0.5f,
               "singer selection and voice count come back with the session");

        // A session from 1.2.0, without any of the new parameters, into a window where they had been changed: they go to their defaults.
        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        for (int i = xml->getNumChildElements(); --i >= 0;)
        {
            const auto id = xml->getChildElement (i)->getStringAttribute ("id");
            if (id == "moreVoices" || id == "leadOn" || id.endsWith ("On") || id.startsWith ("voice5") || id.startsWith ("voice6") || id.startsWith ("voice7") || id.startsWith ("voice8"))
                xml->removeChildElement (xml->getChildElement (i), true);
        }
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        auto r = make();
        setParam (*r, vid (0, "On"), 0.0f);
        setParam (*r, "moreVoices", 4.0f);
        r->setStateInformation (old.getData(), (int) old.getSize());
        bool allOn = r->apvts.getRawParameterValue ("leadOn")->load() >= 0.5f;
        for (int v = 0; v < 8; ++v)
            allOn = allOn && r->apvts.getRawParameterValue (vid (v, "On"))->load() >= 0.5f;
        check (allOn && r->voiceCount() == 2, "a session from before opens with everyone on and its own voice count", r->voiceCount());
    }

    // Reading: Voices Sounding counts only who you hear
    {
        auto p = make();
        setParam (*p, "voices", 4.0f);
        setParam (*p, "moreVoices", 4.0f);
        setParam (*p, vid (1, "On"), 0.0f);
        setParam (*p, vid (7, "On"), 0.0f);
        prepare (*p, 2, SR, 256);
        render (*p, x, 256);
        check (p->getReadings().voicesSounding == 6, "Voices Sounding counts singers you hear", p->getReadings().voicesSounding);
    }
}
