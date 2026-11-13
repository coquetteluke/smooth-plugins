#pragma once
// Phase 2 (1.6.0): one check or more for each bug the hunt kept, written to fail on 1.5.4 and pass after. Included by PluginTest.cpp inside
// its namespace, after WindowTests.h. Run alone: PluginTest phase2. The ids are the hunt's, in ~/Bug Fixing/smooth-plugins/fix/harmony.json.

namespace phase2
{
using namespace windowtests;
inline double largestDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from = 0)
{
    double worst = 0.0;
    for (int ch = 0; ch < juce::jmin (a.getNumChannels(), b.getNumChannels()); ++ch)
        for (int i = from; i < juce::jmin (a.getNumSamples(), b.getNumSamples()); ++i)
            worst = std::max (worst, (double) std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    return worst;
}

inline double maxJumpIn (const juce::AudioBuffer<float>& b, int from, int to)
{
    double m = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = juce::jmax (1, from); i < juce::jmin (to, b.getNumSamples()); ++i)
            m = std::max (m, (double) std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1)));
    return m;
}

inline double rmsDbIn (const juce::AudioBuffer<float>& b, int from, int len)
{
    double sum = 0.0;
    int n = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = from; i < juce::jmin (from + len, b.getNumSamples()); ++i, ++n)
            sum += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return 10.0 * std::log10 (std::max (sum / juce::jmax (1, n), 1.0e-24));
}

inline juce::File baselineFile (const juce::String& name) { return juce::File (juce::String (SMOOTH_HARMONY_CORPUS)).getParentDirectory().getChildFile ("baseline").getChildFile (name); }

inline std::vector<float> readFloats (const juce::String& name)
{
    juce::MemoryBlock m;
    if (! baselineFile (name).loadFileAsData (m)) return {};
    const auto* f = static_cast<const float*> (m.getData());
    return { f, f + m.getSize() / sizeof (float) };
}

// A sung vowel at a level, for a number of seconds
inline std::vector<float> vowelAt (double hz, double seconds, double levelDb, const voice::Vowel& v = voice::vowelA)
{
    return voice::synth (voice::steady (hz, seconds, SR), v, SR, -60.0, levelDb);
}

inline std::vector<float> join (std::initializer_list<std::vector<float>> parts)
{
    std::vector<float> out;
    for (const auto& p : parts) out.insert (out.end(), p.begin(), p.end());
    return out;
}

inline std::vector<float> zeros (double seconds) { return std::vector<float> ((size_t) (seconds * SR), 0.0f); }

// ----------------------------------------------------------------------------------------------------- host Reset (choir-dsp-r1-1)

inline void testReset()
{
    const auto a = join ({ voice::synth (voice::phrase ({ 220.0, 247.0, 277.2, 330.0 }, 0.4, SR), voice::vowelA, SR, -50.0, -14.0), zeros (0.05) });
    const auto b = join ({ voice::synth (voice::phrase ({ 196.0, 233.1, 261.6 }, 0.5, SR), voice::vowelO, SR, -50.0, -14.0), zeros (0.3) });

    {
        auto fresh = make();
        prepare (*fresh, 2, SR, 512);
        const auto want = render (*fresh, b, 512);
        auto p = make();
        prepare (*p, 2, SR, 512);
        render (*p, a, 512);
        p->reset();
        const double worst = largestDifference (render (*p, b, 512), want);
        check (worst == 0.0, "choir-dsp-r1-1: play, reset, play: the second part equals a new instance playing it, largest difference", worst);
    }
    {
        auto p = make();
        prepare (*p, 2, SR, 512);
        render (*p, a, 512);
        p->reset();
        const auto out = render (*p, zeros (0.3), 512);
        const double pk = std::max (out.getMagnitude (0, 0, out.getNumSamples()), out.getMagnitude (1, 0, out.getNumSamples()));
        check (pk == 0.0, "after a reset, silence in gives silence out: largest sample", pk);
        render (*p, a, 512);
        const long before = testkit::allocs.load();
        p->reset();
        const long made = testkit::allocs.load() - before;
        check (made == 0, "reset allocates nothing, allocations", (double) made);
    }
    {
        auto never = make();
        never->reset();
        check (true, "a reset before the first prepare is harmless");
    }
    // JUCE's AU wrapper resets straight after the host turns its own bypass on. Clearing then would cut the voices and the delayed lead off
    // at once instead of the 20 ms fade to the dry lead, so that one reset is left out.
    {
        auto p = make(), q = make();
        prepare (*p, 2, SR, 256);
        prepare (*q, 2, SR, 256);
        const auto withReset = render (*p, a, 256, nullptr, [&] (int idx) { if (idx == 100) { setParam (*p, "bypass", 1.0f); p->reset(); } });
        const auto without = render (*q, a, 256, nullptr, [&] (int idx) { if (idx == 100) setParam (*q, "bypass", 1.0f); });
        check (largestDifference (withReset, without) == 0.0, "the reset that comes with host bypass switching on changes nothing: the fade to the dry lead goes on, largest difference",
               largestDifference (withReset, without));
    }
    {
        auto p = make(), q = make();
        prepare (*p, 2, SR, 256);
        prepare (*q, 2, SR, 256);
        const auto step = [] (PluginProcessor& x, int idx, bool resetToo)
        {
            if (idx == 50) setParam (x, "bypass", 1.0f);
            if (idx == 100 && resetToo) x.reset();
            if (idx == 101) setParam (x, "bypass", 0.0f);
        };
        const auto withReset = render (*p, a, 256, nullptr, [&] (int idx) { step (*p, idx, true); });
        const auto without = render (*q, a, 256, nullptr, [&] (int idx) { step (*q, idx, false); });
        check (largestDifference (withReset, without) > 0.0, "a reset later on while bypassed is not left out");
    }
}

// ----------------------------------------------------------------------------------------------- Ceiling switched (choir-dsp-r1-2)

inline void testCeilingSwitch()
{
    int fullStack = 3;
    // A held vowel peaking at -1 dBFS, on Full Stack, where the ceiling holds the gain well down
    auto x = vowelAt (220.0, 2.0, -12.0);
    {
        const double pk = peak (x.data(), (int) x.size());
        for (auto& v : x) v = (float) (v * std::pow (10.0, -1.0 / 20.0) / pk);
    }
    const int at = (int) (1.2 * SR) / 256;   // block index of the switch, in the held note
    for (const bool toOn : { false, true })
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        p->setCurrentProgram (fullStack);
        setParam (*p, "ceiling", toOn ? 0.0f : 1.0f);
        const auto out = render (*p, x, 256, nullptr, [&] (int idx) { if (idx == at) setParam (*p, "ceiling", toOn ? 1.0f : 0.0f); });
        const int s = at * 256;
        const double steady = maxJumpIn (out, s - (int) (0.3 * SR), s - 256);
        const double around = maxJumpIn (out, s - 64, s + (int) (0.03 * SR));
        check (around <= 2.0 * steady, std::string ("choir-dsp-r1-2: Ceiling switched ") + (toOn ? "on" : "off") + " in a held note at -1 dBFS on Full Stack: largest step against the note's own, times",
               around / std::max (steady, 1.0e-9));
    }
}

// ---------------------------------------------------------------------------------------------- Human jumped to 0 (choir-dsp-r2-1)

inline void testHumanJump()
{
    // A tone that swells by 9 dB three times a second, so the lead's quick and slow levels are far apart at each swell.
    std::vector<float> x ((size_t) (3.0 * SR));
    double phase = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / SR;
        const double swell = std::pow (10.0, (-9.0 + 9.0 * std::max (0.0, std::sin (2.0 * M_PI * 3.0 * t))) / 20.0);
        phase += 2.0 * M_PI * 220.0 / SR;
        x[i] = (float) (0.3 * swell * std::sin (phase));
    }
    const auto setUp = [] (PluginProcessor& p)
    {
        setParam (p, "leadOn", 0.0f);
        setParam (p, "human", 100.0f);
    };
    // Switch while a swell rises: by then the quick level is well over the slow one where the voice reads (latency and lag behind), and the
    // voice is held about 2 to 3 dB down. Swells rise from 1.0 s; the switch is 115 ms in.
    const int at = (int) (1.115 * SR) / 64;
    auto p = make(), q = make();
    setUp (*p);
    setUp (*q);
    prepare (*p, 2, SR, 64);
    prepare (*q, 2, SR, 64);
    const auto switched = render (*p, x, 64, nullptr, [&] (int idx) { if (idx == at) setParam (*p, "human", 0.0f); });
    const auto stayed = render (*q, x, 64);
    // The largest sample-to-sample step in the voices in the 7.5 ms after the switch (where the next 1 ms tick lands), against the same
    // render left at 100 over the same samples. (Two first versions: switched at the swell's very start, where the quick and slow levels
    // are still alike, it read 1.000 on 1.5.4, measuring nothing; read as a level ratio every 0.5 ms it read the voices' grains moving off
    // Human's pitch at once, 0.88 dB after the fix, which is not a click.)
    const int s = at * 64;
    const double jump = maxJumpIn (switched, s - 120, s + 360), own = maxJumpIn (stayed, s - 120, s + 360);
    check (jump <= 2.0 * own, "choir-dsp-r2-1: Human jumped from 100 to 0 while a swell rises: largest step around it against the same render left at 100, times", jump / std::max (own, 1.0e-9));

    // And back on after a while at 0: the voices start from their level, not from a ratio left from when Human was last on.
    {
        std::vector<float> y;
        const auto loud = vowelAt (220.0, 0.9, -8.0), quiet = vowelAt (220.0, 2.0, -26.0);
        y = join ({ loud, quiet });
        auto r = make();
        setUp (*r);
        prepare (*r, 2, SR, 64);
        const int offAt = (int) (0.95 * SR) / 64, onAt = (int) (2.0 * SR) / 64;   // off just after the drop, on again 1 s into the quiet note
        const auto out = render (*r, y, 64, nullptr, [&] (int idx)
        {
            if (idx == offAt) setParam (*r, "human", 0.0f);
            if (idx == onAt) setParam (*r, "human", 100.0f);
        });
        const int s = onAt * 64;
        const double before = rmsDbIn (out, s - (int) (0.03 * SR), (int) (0.03 * SR));
        double worst = 0.0;
        for (int k = 0; k < 10; ++k)
            worst = std::max (worst, std::abs (rmsDbIn (out, s + k * (int) (0.03 * SR), (int) (0.03 * SR)) - before));
        check (worst <= 2.0, "and Human back on in a steady note keeps the voices' level over the next 300 ms, largest change (dB)", worst);
    }
}

// --------------------------------------------------------------------------------------------------------- tail (choir-dsp-r1-3)

inline void testTail()
{
    // A phrase that ends in an "s", then nothing
    std::vector<float> s ((size_t) (0.12 * SR));
    noise (s.data(), (int) s.size(), 0.05, 7);
    const auto x = join ({ voice::synth (voice::phrase ({ 220.0, 247.0 }, 0.5, SR), voice::vowelA, SR, -50.0, -14.0), s, zeros (0.8) });
    const int inputEnd = (int) (1.12 * SR);
    for (const int preset : { -1, 12 })
    {
        auto p = make();
        if (preset >= 0) p->setCurrentProgram (preset);
        if (preset >= 0)
        {
            setParam (*p, "humanize", 100.0f);
            setParam (*p, "consonantScatter", 100.0f);
            setParam (*p, "human", 100.0f);
            for (int v = 0; v < 8; ++v) setParam (*p, vid (v, "Drift"), 100.0f);
        }
        prepare (*p, 2, SR, 256);
        const auto out = render (*p, x, 256);
        int last = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < out.getNumSamples(); ++i)
                if (std::abs (out.getSample (ch, i)) > std::pow (10.0, -90.0 / 20.0)) last = std::max (last, i);
        const double after = (double) (last - inputEnd - p->getLatencySamples()) / SR;
        check (after <= p->getTailLengthSeconds(), std::string ("choir-dsp-r1-3: the output after the lead has stopped fits the reported tail, ")
                   + (preset < 0 ? "defaults" : "Eight Part Choir with Drift, Humanize, Scatter and Human at 100") + ", seconds after", after);
    }
}

// ----------------------------------------------------------------------------------------------- readings (choir-dsp-r1-4, paramtext)

inline void testReadings()
{
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        const auto x = vowelAt (220.0, 4.0, -14.0);
        render (*p, std::vector<float> (x.begin(), x.begin() + (int) SR), 256);
        setParam (*p, vid (1, "Level"), -60.0f);
        render (*p, std::vector<float> (x.begin() + (int) SR, x.end()), 256);
        const auto r = p->getReadings();
        check (r.voiceNote[1] == -1 && r.voicesSounding == 1, "choir-dsp-r1-4: voice 2 turned Off while singing reads no note and is not counted: note, sounding",
               r.voiceNote[1] * 10.0 + r.voicesSounding);
    }
    // HostStress paramtext: each reading's own text reads back as itself
    {
        auto p = make();
        int bad = 0, total = 0;
        std::string first;
        for (auto* prm : p->getParameters())
        {
            auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm);
            if (w == nullptr || ! (w->paramID.endsWith ("Note") || w->paramID == "activeKey")) continue;
            for (int k = 0; k <= 20; ++k)
            {
                const float v = (float) k / 20.0f;
                const auto text = prm->getText (v, 64);
                const float back = prm->getValueForText (text);
                ++total;
                if (prm->getText (back, 64) != text)
                {
                    if (first.empty()) first = (w->paramID + ": " + text + " reads back as " + prm->getText (back, 64)).toStdString();
                    ++bad;
                }
            }
        }
        check (bad == 0, "paramtext: every note and key reading's text reads back as itself, failures of " + std::to_string (total) + (first.empty() ? "" : ", first " + first), bad);
    }
}

// -------------------------------------------------------------------------------------------- Chord Hold and the pedal (choir-dsp-r1-5)

inline void testHoldAndPedal()
{
    auto p = make();
    setParam (*p, "mode", (float) modeMidi);
    setParam (*p, "voices", 3.0f);
    setParam (*p, "chordHold", 1.0f);
    prepare (*p, 2, SR, 256);
    const auto x = vowelAt (220.0, 1.6, -14.0);
    std::vector<Timed> ev;
    ev.push_back ({ 0, juce::MidiMessage::controllerEvent (1, 64, 127) });
    for (int n : { 60, 64, 67 }) ev.push_back ({ 256, juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) });
    for (int n : { 60, 64, 67 }) ev.push_back ({ (int) (0.3 * SR), juce::MidiMessage::noteOff (1, n) });
    const int holdOffAt = (int) (0.6 * SR) / 256;
    juce::AudioBuffer<float> out (2, (int) x.size());
    {
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        for (int pos = 0, idx = 0; pos + 256 <= (int) x.size(); pos += 256, ++idx)
        {
            if (idx == holdOffAt) setParam (*p, "chordHold", 0.0f);
            for (int ch = 0; ch < 2; ++ch) std::copy_n (x.data() + pos, 256, buf.getWritePointer (ch));
            midi.clear();
            for (const auto& e : ev)
                if (e.sample >= pos && e.sample < pos + 256) midi.addEvent (e.message, e.sample - pos);
            p->processBlock (buf, midi);
        }
    }
    const auto r = p->getReadings();
    std::set<int> notes;
    for (int v = 0; v < 3; ++v) notes.insert (r.voiceNote[(size_t) v]);
    check (notes == std::set<int> { 60, 64, 67 }, "choir-dsp-r1-5: Chord Hold switched off while the pedal is down keeps the chord the pedal holds, voice 1 note",
           r.voiceNote[0]);
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
    for (int k = 0; k < (int) (0.1 * SR) / 256; ++k)
    {
        for (int ch = 0; ch < 2; ++ch) std::copy_n (x.data(), 256, buf.getWritePointer (ch));
        p->processBlock (buf, midi);
        midi.clear();
    }
    check (p->getReadings().voiceNote[0] == -1, "and the pedal coming up lets them go", p->getReadings().voiceNote[0]);
}

// ------------------------------------------------------------------------------------- a sleeping voice switched off (choir-dsp-r2-2)

inline void testSleepingVoiceOff()
{
    auto p = make();
    setParam (*p, "voices", 2.0f);
    setParam (*p, "leadOn", 0.0f);
    setParam (*p, vid (0, "On"), 0.0f);
    setParam (*p, vid (1, "On"), 0.0f);
    prepare (*p, 2, SR, 256);
    std::vector<float> x ((size_t) (0.9 * SR));
    sine (x.data(), (int) x.size(), 220.0, SR, 0.25);
    const int offAt = (int) (0.5 * SR) / 256, countAt = (int) (0.7 * SR) / 256;
    const auto out = render (*p, x, 256, nullptr, [&] (int idx)
    {
        if (idx == offAt) setParam (*p, vid (2, "On"), 0.0f);
        if (idx == countAt) setParam (*p, "voices", 3.0f);
    });
    const double pk = std::max (out.getMagnitude (0, countAt * 256, out.getNumSamples() - countAt * 256), out.getMagnitude (1, countAt * 256, out.getNumSamples() - countAt * 256));
    check (toDb (pk) <= -80.0, "choir-dsp-r2-2: a singer switched Off while past the voice count stays silent when Voices brings it in, peak", toDb (pk), "dBFS");
}

// ---------------------------------------------------------------------------------- the history count's memory order (r1-4 / r3-6)

inline void testHistoryOrder()
{
    // The fault is a reordering arm64 is allowed to make and a test cannot force. So the source itself is read: the count is published
    // with release after the point's fields and read with acquire, which makes a reader that sees the count see the fields too.
    const auto src = juce::File (juce::String (SMOOTH_HARMONY_CORPUS)).getParentDirectory().getParentDirectory().getChildFile ("src");
    const auto cpp = src.getChildFile ("PluginProcessor.cpp").loadFileAsString(), h = src.getChildFile ("PluginProcessor.h").loadFileAsString();
    check (cpp.contains ("pitchHistoryCount.store (at + 1, std::memory_order_release)") && h.contains ("pitchHistoryCount.load (std::memory_order_acquire)"),
           "choir-window-r1-4 / choir-dsp-r3-6: the newest pitch point is published with release and read with acquire");

    // And on this machine, a reader on another thread never sees a newest point older than one it saw before.
    auto p = make();
    setParam (*p, "leadLevel", -60.0f);
    prepare (*p, 2, SR, 256);
    std::vector<double> f ((size_t) (12.0 * SR));
    for (size_t i = 0; i < f.size(); ++i) f[i] = 110.0 * std::pow (2.0, 2.5 * (double) i / (double) f.size());   // always rising
    const auto x = voice::synth (f, voice::vowelA, SR, -60.0, -14.0);
    std::atomic<bool> done { false };
    std::atomic<int> backwards { 0 };
    std::thread reader ([&]
    {
        float lastSeen = 0.0f;
        while (! done.load())
        {
            const auto count = p->getPitchHistoryCount();
            if (count < 1) continue;
            const float lead = p->getPitchPoint (count - 1).lead.load (std::memory_order_relaxed);
            if (lead > 0.0f && lastSeen > 0.0f && lead < lastSeen - 1.0f) ++backwards;
            if (lead > 0.0f) lastSeen = lead;
        }
    });
    render (*p, x, 256);
    done = true;
    reader.join();
    info ("a reader thread saw the newest pitch point go back by more than a semitone, times", backwards.load());
}

// ---------------------------------------------------------------- a phrase an octave from the last, after a pause (choir-dsp-r3-1)

// The tracker alone. One fed the last phrase, a pause and the new phrase; one fed the same pause and the new phrase only. Over the new phrase
// they should read the same: what was sung before a pause has no say in where the next phrase starts.
inline double trackerAfterPause (int lastMidi, int lastKind, int nextMidi, int nextKind, double pauseSec)
{
    juce::ScopedNoDenormals noDenormals;
    std::vector<int> measured;
    const auto last = onsets::makeKind (lastKind, lastMidi, 11, measured).x;
    const auto next = onsets::makeKind (nextKind, nextMidi, 12, measured).x;
    const auto withLast = join ({ last, zeros (pauseSec), next });
    const auto without = join ({ std::vector<float> (last.size(), 0.0f), zeros (pauseSec), next });
    smoothharmony::PitchTracker a, b;
    for (auto* t : { &a, &b })
    {
        t->prepare (SR);
        PluginProcessor::prepareTrackerLike (*t, altoTenor);
    }
    const size_t from = last.size() + (size_t) (pauseSec * SR);
    int differ = 0;
    for (size_t i = 0; i < withLast.size(); ++i)
    {
        a.push (withLast[i]);
        b.push (without[i]);
        if (i < from) continue;
        const double pa = a.period(), pb = b.period();
        if (a.isVoiced() != b.isVoiced() || (pa > 0.0 && pb > 0.0 && std::abs (pa / pb - 1.0) > 0.03)) ++differ;
    }
    return 1000.0 * differ / SR;
}

// Every kind of start against every kind of last phrase, an octave and a fifth either way, for finding cases. PluginTest r31probe.
inline void probePhraseAfterPause()
{
    for (int lastKind : { 0, 1, 4, 5, 8 })
        for (int nextKind = 0; nextKind <= 17; ++nextKind)
            for (int base : { 45, 50, 55 })
                for (int move : { 12, -12, 7, -7, 24 })
                    for (double pause : { 0.3, 1.0 })
                    {
                        const int lastMidi = move > 0 ? base : base - move, nextMidi = move > 0 ? base + move : base;
                        if (nextMidi > 81 || lastMidi > 81) continue;
                        const double ms = trackerAfterPause (lastMidi, lastKind, nextMidi, nextKind, pause);
                        if (ms > 0.0)
                            std::printf ("PROBE last %d kind %d, next %d kind %d, pause %.1f: %.1f ms differ\n", lastMidi, lastKind, nextMidi, nextKind, pause, ms);
                    }
    std::printf ("PROBE done\n");
}

inline void testPhraseAfterPause()
{
    // kinds from onsets::makeKind: 0 hard start, 1 soft start, 4 breathy start, 5 creaky start, 10 vibrato at once. The hunt's own octave
    // cases (A2 then a breathy A3, A3 then a hummed A2) read the same on 1.5.4 (PluginTest r31probe tries 2,700 pairs); these are the
    // pairs where the phrase before the pause did change the start on 1.5.4, by one analysis.
    struct Case { int lastMidi, lastKind, nextMidi, nextKind; const char* name; };
    const Case cases[] = {
        { 50, 0, 74, 1, "D3, a pause, then D5 two octaves up with a soft start" },
        { 50, 5, 74, 1, "D3 from a creak, a pause, then D5 with a soft start" },
        { 55, 4, 79, 1, "G3 breathy, a pause, then G5 with a soft start" },
        { 55, 0, 67, 10, "G3, a pause, then G4 an octave up with vibrato at once" },
        { 45, 0, 57, 4, "A2, a pause, then A3 with a breathy start (the hunt's case)" },
    };
    for (const auto& c : cases)
        for (const double pause : { 0.3, 1.0 })
        {
            const double ms = trackerAfterPause (c.lastMidi, c.lastKind, c.nextMidi, c.nextKind, pause);
            check (ms == 0.0, std::string ("choir-dsp-r3-1: ") + c.name + " after " + juce::String (pause, 1).toStdString()
                                  + " s: ms of the new phrase the tracker reads otherwise than one that never heard the last", ms, "ms");
        }
}

// ------------------------------------------------ a voice behind the lead sizes its grains for what it reads (choir-dsp-r3-2)

// One voice in unison (Fixed, +0), standing 20 to 40 ms behind the lead (Drift 100, nothing else moving), on a lead that drops an octave
// legato. For the voice's lag after the drop it still reads the high note while the tracker already has the low one. Its level there,
// in 5 ms frames, against its own level on the low note once settled.
inline void traceLaggedGrains()
{
    const auto x = voice::synth (voice::phrase ({ 220.0, 110.0 }, 0.7, SR, 0.0, 0.0), voice::vowelA, SR, -60.0, -16.0);
    auto p = clean (modeFixed, 1);
    setParam (*p, vid (0, "Interval"), 0.0f);
    setParam (*p, vid (0, "Drift"), 100.0f);
    setParam (*p, "consonants", 0.0f);
    prepare (*p, 1, SR, 256);
    const auto out = render (*p, x, 256);
    const double lag = p->getReadings().voiceLagMs[0];
    const int frame = (int) (0.005 * SR), lat = p->getLatencySamples(), lagS = (int) (lag * 0.001 * SR);
    juce::AudioBuffer<float> in (1, (int) x.size());
    std::copy (x.begin(), x.end(), in.getWritePointer (0));
    std::printf ("lag %.1f ms\n", lag);
    for (int at = (int) (0.6 * SR) + lat; at < (int) (0.95 * SR) + lat; at += frame)
        std::printf ("%7.1f ms  out %6.1f  read %6.1f\n", 1000.0 * (at - lat - (int) (0.7 * SR)) / SR, rmsDbIn (out, at, frame), rmsDbIn (in, at - lat - lagS, frame));
}

inline void testLaggedGrains()
{
    const auto x = voice::synth (voice::phrase ({ 220.0, 110.0 }, 0.7, SR, 0.0, 0.0), voice::vowelA, SR, -60.0, -16.0);
    auto p = clean (modeFixed, 1);
    setParam (*p, vid (0, "Interval"), 0.0f);
    setParam (*p, vid (0, "Drift"), 100.0f);
    setParam (*p, "consonants", 0.0f);
    prepare (*p, 1, SR, 256);
    const auto out = render (*p, x, 256);
    // Against its own level on the high note just before: the drop's first 25 ms are still the high note where the voice reads.
    // (A first version compared with the low note once settled, which the synth makes 9 dB quieter: it read 11.8 dB on 1.5.4 and 9.9 after,
    // the level of the old note and not the fault.)
    const int frame = (int) (0.005 * SR);
    const int stepOut = (int) (0.7 * SR) + p->getLatencySamples();
    double before = 0.0;
    int n = 0;
    for (int at = stepOut - (int) (0.06 * SR); at < stepOut - (int) (0.005 * SR); at += frame, ++n)
        before += rmsDbIn (out, at, frame);
    before /= juce::jmax (1, n);
    double over = -100.0;
    for (int at = stepOut; at < stepOut + (int) (0.025 * SR); at += frame)
        over = std::max (over, rmsDbIn (out, at, frame) - before);
    check (over <= 1.0, "choir-dsp-r3-2: a voice behind the lead, the lead dropping an octave legato: its loudest 5 ms while it still reads the high note, over its level on it just before (dB)", over);
}

// ------------------------------------------------------------------------------------------------- the chosen preset (choir-window-r2-2)

inline void testSavedPreset()
{
    const auto reloaded = [] (PluginProcessor& from)
    {
        juce::MemoryBlock st;
        from.getStateInformation (st);
        auto to = make();
        to->setStateInformation (st.getData(), (int) st.getSize());
        return to;
    };
    {
        auto a = make();
        a->setCurrentProgram (12);
        check (reloaded (*a)->getCurrentProgram() == 12, "choir-window-r2-2: a session saved after choosing Eight Part Choir reloads naming it", reloaded (*a)->getCurrentProgram());
        setParam (*a, "mix", 40.0f);
        auto b = reloaded (*a);
        check (std::abs (b->apvts.getRawParameterValue ("mix")->load() - 40.0f) < 0.06f, "the preset is not applied again on reload: a Mix changed after choosing it stays");
        check (! b->apvts.state.hasProperty ("program"), "the preset number stays out of the sound's value tree");
        auto w = openWindow (*b);
        w->tick (1.0 / 30.0);
        auto* box = w->getPanel().findChildWithID ("presets");
        check (box != nullptr, "the preset box is there");
    }
    {
        auto fresh = make();
        check (reloaded (*fresh)->getCurrentProgram() == -1, "a session where no preset was chosen reloads naming none", reloaded (*fresh)->getCurrentProgram());
    }
    for (const juce::String bad : { "99", "-5", "x" })
    {
        auto a = make();
        a->setCurrentProgram (2);
        juce::MemoryBlock st;
        a->getStateInformation (st);
        auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        xml->setAttribute ("program", bad);
        juce::MemoryBlock damaged;
        juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
        auto b = make();
        b->setStateInformation (damaged.getData(), (int) damaged.getSize());
        check (juce::isPositiveAndBelow (b->getCurrentProgram(), b->getNumPrograms()), ("a damaged preset number (" + bad + ") is kept in range").toStdString(), b->getCurrentProgram());
    }

    // What the installed 1.5.4 saved, read out of its Audio Unit through the host API, and what that Audio Unit played with it
    // (tests/baseline/capture.mm, run once before 1.6.0 was installed). It names no preset and sounds as it did.
    const auto in = readFloats ("input.f32");
    check (in.size() == (size_t) (5 * SR), "the 1.5.4 input file loads", (double) in.size());
    for (const juce::String name : { "v1.5.4-default", "v1.5.4-full-stack", "v1.5.4-tight", "v1.5.4-eight-part-choir", "v1.5.4-midi-choir", "v1.5.4-human-100" })
    {
        juce::MemoryBlock state;
        const bool found = baselineFile (name + ".state").loadFileAsData (state);
        const auto want = readFloats (name + ".f32");
        auto p = make();
        p->setStateInformation (state.getData(), (int) state.getSize());
        check (found && p->getCurrentProgram() < 0, (name + ": names no preset, since 1.5.4 did not save which one").toStdString(), p->getCurrentProgram());
        prepare (*p, 2, SR, 512);
        const auto out = render (*p, in, 512);
        double worst = 0.0;
        const int n = (int) in.size();
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n && want.size() == (size_t) (2 * n); ++i)
                worst = std::max (worst, (double) std::abs (out.getSample (ch, i) - want[(size_t) (ch * n + i)]));
        // Three fixes in 1.6.0 change what the voices sing on any session: a voice sizes its grains for the audio it reads (choir-dsp-r3-2)
        // and the tracker forgets the last phrase after a pause and breaks the longer period's row at a gap (r3-1, r3-4). With those three
        // taken out, every one of these sessions plays exactly what 1.5.4 played (the 1.6.0 report has that run). MIDI Choir takes its notes
        // from MIDI, and with no MIDI here sings nothing, so it is held to exact.
        const auto what = (name + ": the session the installed 1.5.4 saved, against what 1.5.4 played with it, largest difference").toStdString();
        if (name == "v1.5.4-midi-choir")
            check (want.size() == (size_t) (2 * n) && worst == 0.0, what, worst > 0.0 ? toDb (worst) : -240.0, "dBFS");
        else
            info (what + " (the voices' grains and starts, see above)", worst > 0.0 ? toDb (worst) : -240.0, "dBFS");
    }
}

// ------------------------------------------------------------------------------------------------------------------ the window

inline void testWindowFixes()
{
    using namespace windowtests;

    // choir-window-r1-1: two quick clicks on an arrow step twice, not three times
    {
        auto p = make();
        setParam (*p, "mode", (float) modeFixed);
        setParam (*p, "voice1Interval", 4.0f);
        auto e = openWindow (*p);
        e->tick (1.0 / 30.0);
        auto* field = findById<ui::IntervalField> (*e, "voice1Interval");
        const juce::Point<float> up ((float) field->getWidth() - 5.0f, 5.0f);
        field->mouseDown (mouseAt (*field, up, 1));
        field->mouseUp (mouseAt (*field, up, 1));
        field->mouseDown (mouseAt (*field, up, 2));
        field->mouseUp (mouseAt (*field, up, 2));
        field->mouseDoubleClick (mouseAt (*field, up, 2));
        check (juce::roundToInt (field->getValue()) == 6, "choir-window-r1-1: two quick clicks on the up arrow in Fixed go from +4 to +6", field->getValue());
    }

    // choir-window-r2-1: in a key without seven notes, and in Chord Tones, each press moves one semitone
    {
        auto p = make();
        setParam (*p, "mode", (float) modeScale);
        setParam (*p, "scale", (float) scaleChromatic);
        setParam (*p, "voice1Interval", 5.0f);
        auto e = openWindow (*p);
        e->tick (1.0 / 30.0);
        auto* field = findById<ui::IntervalField> (*e, "voice1Interval");
        field->step (1);
        const int first = juce::roundToInt (field->getValue());
        field->step (1);
        const int second = juce::roundToInt (field->getValue());
        check (first == 6 && second == 7, "choir-window-r2-1: In Key on a Chromatic scale, two presses up from +5 go to +6 and +7 (a fifth)", second);
        setParam (*p, "scale", (float) scaleBlues);
        setParam (*p, "voice1Interval", 5.0f);
        e->tick (1.0 / 30.0);
        field->step (1);
        check (juce::roundToInt (field->getValue()) == 6, "on Blues, one press up from +5 goes to +6", field->getValue());
        setParam (*p, "mode", (float) modeChord);
        setParam (*p, "voice1Interval", 3.0f);
        e->tick (1.0 / 30.0);
        field->step (1);
        check (juce::roundToInt (field->getValue()) == 4, "in Chord Tones, one press up from +3 goes to +4", field->getValue());
        setParam (*p, "mode", (float) modeScale);
        setParam (*p, "scale", (float) scaleMajor);
        setParam (*p, "voice1Interval", 4.0f);
        e->tick (1.0 / 30.0);
        field->step (1);
        check (juce::roundToInt (field->getValue()) == 5, "in a seven note key a press still moves to the next name: +4 up goes to +5");
    }

    // choir-window-r3-1: a window saved at a size taller than the screen opens to fit it
    {
        auto p = make();
        p->windowWidth.store (PluginProcessor::maxWindowWidth);
        std::unique_ptr<HarmonyEditor> e (dynamic_cast<HarmonyEditor*> (p->createEditor()));
        e->addToDesktop (0);
        e->tick (1.0 / 30.0);
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (e->getScreenBounds());
        const int room = display != nullptr ? display->userArea.getHeight() : 0;
        check (display != nullptr && e->getHeight() + 80 <= room, "choir-window-r3-1: a window saved 1900 wide opens with room for the host's header on this screen, height", e->getHeight());
        check (std::abs ((double) e->getWidth() / e->getHeight() - (double) HarmonyEditor::designWidth / HarmonyEditor::designHeight) < 0.005, "and keeps its shape");
        e->removeFromDesktop();
    }

    // choir-window-r1-2: opened between phrases, the lines already sung scroll on and nothing is printed over them
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        // A phrase, then a second of nothing, then the window opens: the phrase is still on screen.
        // (A first version opened it right at the phrase's end, where the tracker's last few ms of pitch were still arriving: 0 on 1.5.4.)
        render (*p, join ({ vowelAt (220.0, 1.0, -14.0), zeros (1.0) }), 256);
        auto e = openWindow (*p);
        juce::AudioBuffer<float> silent (2, 256);
        juce::MidiBuffer midi;
        int placeholder = 0;
        for (int k = 0; k < 100; ++k)
        {
            silent.clear();
            p->processBlock (silent, midi);
            if (k % 5 == 4)
            {
                e->tick (1.0 / 37.5);
                if (e->getStage().showsPlaceholder()) ++placeholder;
            }
        }
        check (placeholder == 0, "choir-window-r1-2: opened a second after a phrase, \"Play your vocal\" is not printed over its lines, frames that show it", placeholder);
    }

    // choir-window-r1-3: the light opens without a peak from while the window was shut
    {
        auto p = make();
        p->setCurrentProgram (3);
        prepare (*p, 2, SR, 256);
        auto x = vowelAt (220.0, 1.0, -12.0);
        const double pk = peak (x.data(), (int) x.size());
        for (auto& v : x) v = (float) (v * std::pow (10.0, -1.0 / 20.0) / pk);
        render (*p, join ({ x, zeros (2.0) }), 256);
        auto e = openWindow (*p);
        auto* light = outputLightIn (e->getPanel());
        check (light != nullptr && ! light->isHot(), "choir-window-r1-3: opened after a loud passage and 2 s of silence, the output light is not lit", light != nullptr ? light->peakDb() : 0.0, "dB");
    }

    // choir-window-r2-3: grabbing a singer off its centre and dragging straight up keeps its pan
    {
        auto p = make();
        setParam (*p, vid (0, "Pan"), -35.0f);
        auto e = openWindow (*p);
        for (int k = 0; k < 30; ++k) e->tick (1.0 / 30.0);
        auto& stage = e->getStage();
        const auto centre = stage.singerPosition (0);
        const auto grab = centre + juce::Point<float> (14.0f, 0.0f);
        stage.mouseDown (mouseAt (stage, grab));
        stage.mouseDrag (mouseAt (stage, grab + juce::Point<float> (0.0f, -40.0f)));
        stage.mouseUp (mouseAt (stage, grab + juce::Point<float> (0.0f, -40.0f)));
        const float pan = p->apvts.getRawParameterValue ("voice1Pan")->load();
        check (std::abs (pan + 35.0f) <= 1.0f, "choir-window-r2-3: grabbed 14 px right of its centre and dragged straight up, voice 1 keeps pan -35", pan);
    }

    // choir-window-r2-4: Pan and Tone fill from the middle
    {
        auto p = make();
        setParam (*p, vid (0, "Pan"), 0.0f);
        auto e = openWindow (*p);
        auto* bar = findById<ui::BarSlider> (*e, "voice1Pan");
        juce::Image img (juce::Image::ARGB, bar->getWidth(), bar->getHeight(), true);
        {
            juce::Graphics g (img);
            bar->paintEntireComponent (g, false);
        }
        const int y = bar->getHeight() - 3;
        int lit = 0;
        for (int x = 2; x < bar->getWidth() / 4; ++x)
        {
            const auto c = img.getPixelAt (x, y);
            if (c.getSaturation() > 0.3f && c.getAlpha() > 100) ++lit;
        }
        check (lit == 0, "choir-window-r2-4: a centred Pan draws no fill on the left quarter of its bar, coloured pixels", lit);
    }

    // choir-window-r2-5: All stays dark when every singer on the stage is on
    {
        const auto allLook = [] (bool hideOne)
        {
            auto p = make();
            setParam (*p, "voices", 4.0f);
            setParam (*p, "moreVoices", 4.0f);
            if (hideOne) setParam (*p, vid (6, "On"), 0.0f);
            setParam (*p, "moreVoices", 0.0f);
            setParam (*p, "voices", 2.0f);
            auto e = openWindow (*p);
            e->tick (1.0 / 30.0);
            auto& all = e->getStage().getAllButton();
            juce::Image img (juce::Image::ARGB, all.getWidth(), all.getHeight(), true);
            juce::Graphics g (img);
            all.paintEntireComponent (g, false);
            return img;
        };
        const auto plain = allLook (false), hidden = allLook (true);
        int differ = 0;
        for (int y = 0; y < plain.getHeight(); ++y)
            for (int x = 0; x < plain.getWidth(); ++x)
                if (plain.getPixelAt (x, y) != hidden.getPixelAt (x, y)) ++differ;
        check (differ == 0, "choir-window-r2-5: voice 7 off and past the count of 2 leaves All as it is with everyone on, pixels that differ", differ);
    }
}

// --------------------------------------------------------------------------------------------------------------------- Follow Key

inline void testFollowKeyPhase2()
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("smooth-harmony-key-phase2.json");
    file.deleteFile();

    // key-readers item 3: a fresh Manual key (listening false, confidence 1) is taken as a Locked one is
    {
        smoothharmony::KeyFollower f;
        f.setPath (file.getFullPathName().toRawUTF8());
        file.replaceWithText ("{\"key\":\"D\",\"scale\":\"minor\",\"confidence\":1.00,\"time\":" + juce::String (nowUnix(), 1) + ",\"listening\":false,\"source\":\"manual\"}");
        check (f.poll (nowUnix()) && f.key() == 2 && f.minor(), "key-readers: a fresh Manual key (listening false) is followed");
    }

    // key-file-link-r2-1: in an offline bounce the key file is read by audio time. The file changes, then 0.4 s of audio is rendered as fast
    // as it goes: the new key is in use by the end, however little wall time that took.
    {
        auto p = make (file.getFullPathName());
        setParam (*p, "followKey", 1.0f);
        p->setNonRealtime (true);
        prepare (*p, 2, SR, 512);
        file.replaceWithText (keyJson ("C", "major", 0.9, nowUnix()));
        p->pollKeyFileNow();
        const auto x = vowelAt (220.0, 0.5, -14.0);
        render (*p, x, 512);
        file.replaceWithText (keyJson ("E", "major", 0.9, nowUnix()));
        render (*p, std::vector<float> (x.begin(), x.begin() + (int) (0.4 * SR)), 512);
        check (p->getReadings().activeKey == 4, "key-file-link-r2-1: offline, a key change in the file is in use 0.4 s of audio later, active key", p->getReadings().activeKey);
    }

    // Improvement: the window says whose key it is and how old
#ifndef PHASE2_BEFORE
    {
        auto p = make (file.getFullPathName());
        setParam (*p, "followKey", 1.0f);
        file.replaceWithText (keyJson ("E", "major", 0.9, nowUnix() - 300.0));
        p->pollKeyFileNow();
        auto e = openWindow (*p);
        e->tick (1.0 / 30.0);
        check (e->getFollowText() == "From Key, 5 min ago", "the window says the key is Key's and was written 5 min ago: " + e->getFollowText().toStdString());
        setParam (*p, "followKey", 0.0f);
        p->pollKeyFileNow();
        e->tick (1.0 / 30.0);
        check (e->getFollowText().isEmpty(), "and says nothing with Follow Key off");
        auto q = make ("/nonexistent/smooth-harmony-test.json");
        setParam (*q, "followKey", 1.0f);
        auto w = openWindow (*q);
        w->tick (1.0 / 30.0);
        check (w->getFollowText() == "Waiting for Key, using this key", "before any key from Key it says the window's own key is in use: " + w->getFollowText().toStdString());
    }
#endif
    file.deleteFile();
}

// ---------------------------------------------------------------------------------------------------------------- the new preset

inline void testHumanPreset()
{
    auto p = make();
    const int n = p->getNumPrograms();
    check (n == 15 && p->getProgramName (n - 1) == "Human Thirds And Fifths", "a preset that shows Human off, added last", n);
    const char* before[] = { "Pop Thirds And Fifths", "Third Above", "Third Below", "Full Stack", "Gang Unison", "Wide Octaves", "Power Fifths", "MIDI Choir",
                             "MIDI Chords Only", "Chord Tones Pad", "Tight", "Six Part Stack", "Eight Part Choir", "MIDI Eight Part Choir" };
    bool same = true;
    for (int i = 0; i < 14; ++i) same = same && p->getProgramName (i) == before[i];
    check (same, "every older preset keeps its number");
    p->setCurrentProgram (n - 1);
    check (std::abs (p->apvts.getRawParameterValue ("human")->load() - 65.0f) < 0.06f, "it sets Human to 65");
    auto q = make();
    setParam (*q, "human", 40.0f);
    q->setCurrentProgram (0);
    check (std::abs (q->apvts.getRawParameterValue ("human")->load() - 40.0f) < 0.06f, "and the others still leave Human alone");
}
}

void testPhase2()
{
    phase2::testReset();
    phase2::testCeilingSwitch();
    phase2::testHumanJump();
    phase2::testTail();
    phase2::testReadings();
    phase2::testHoldAndPedal();
    phase2::testSleepingVoiceOff();
    phase2::testHistoryOrder();
    phase2::testPhraseAfterPause();
    phase2::testLaggedGrains();
    phase2::testSavedPreset();
    phase2::testWindowFixes();
    phase2::testFollowKeyPhase2();
    phase2::testHumanPreset();
}
