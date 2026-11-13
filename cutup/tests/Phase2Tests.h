#pragma once
// Phase 2 (2026-11-13): one test per bug kept from the bug hunt, written and run on 1.2.0 before any fix, and the sessions the
// installed 1.2.0 saved (tests/baseline, captured before 1.2.1 was installed). Run alone with `PluginTest phase2`; also part of the full run.
// Included at the end of PluginTest.cpp, so it uses that file's helpers.

namespace phase2
{
    using namespace windowtests;

    juce::File baselineDir() { return juce::File (__FILE__).getParentDirectory().getChildFile ("baseline"); }

    float raw (PluginProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

    juce::MemoryBlock stateOf (PluginProcessor& p) { juce::MemoryBlock m; p.getStateInformation (m); return m; }

    void restore (PluginProcessor& p, const juce::MemoryBlock& m) { p.setStateInformation (m.getData(), (int) m.getSize()); }

    float textWidth (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

    std::unique_ptr<SliceEditor> openWindow (PluginProcessor& p, int w = 1160, int h = 740)
    {
        std::unique_ptr<SliceEditor> window (dynamic_cast<SliceEditor*> (p.createEditor()));
        window->setVisible (true);
        window->setSize (w, h);
        window->refresh();
        return window;
    }

    // The same score the capture tool plays through the installed component: 16 notes, one every 10 blocks of 512.
    constexpr int kScoreBlock = 512, kScoreBlocks = 200;
    struct ScoreEvent { int block; int status, data1, data2; };
    std::vector<ScoreEvent> score()
    {
        std::vector<ScoreEvent> s;
        for (int k = 0; k < 16; ++k)
        {
            s.push_back ({ 4 + k * 10, 0x90, 60 + k, 100 });
            s.push_back ({ 4 + k * 10 + 6, 0x80, 60 + k, 0 });
        }
        return s;
    }

    std::uint64_t fnv (std::uint64_t h, const float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            std::uint32_t w;
            std::memcpy (&w, x + i, 4);
            h = (h ^ w) * 1099511628211ull;
        }
        return h;
    }

    // Plays the score through a processor already holding a state. Left then right, hashed the way the capture tool hashes.
    std::uint64_t playScore (PluginProcessor& p, double* peakOut = nullptr)
    {
        juce::AudioBuffer<float> buffer (2, kScoreBlock);
        juce::MidiBuffer midi;
        std::vector<float> left, right;
        const auto ev = score();
        for (int b = 0; b < kScoreBlocks; ++b)
        {
            buffer.clear();
            midi.clear();
            for (const auto& e : ev)
                if (e.block == b)
                    midi.addEvent (juce::MidiMessage (e.status, e.data1, e.data2), 0);
            p.processBlock (buffer, midi);
            left.insert (left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + kScoreBlock);
            right.insert (right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + kScoreBlock);
        }
        if (peakOut != nullptr) *peakOut = std::max (peak (left.data(), (int) left.size()), peak (right.data(), (int) right.size()));
        auto h = fnv (14695981039346656037ull, left.data(), (int) left.size());
        return fnv (h, right.data(), (int) right.size());
    }

    // ---- the seeds the capture tool starts from: written by this suite at 1.2.0, then loaded into the installed 1.2.0 ----

    void writeSeeds (const juce::File& dir)
    {
        dir.createDirectory();
        double rate = 0.0;
        auto speech = readAudioMono (juce::File (__FILE__).getParentDirectory().getChildFile ("corpus/say-phrase.wav"), rate);
        speech.resize ((size_t) (2.0 * rate));
        const auto file = writeWav ("seed-speech.wav", { speech }, rate);

        const auto write = [&] (PluginProcessor& p, const char* name)
        {
            const auto m = stateOf (p);
            dir.getChildFile (name).replaceWithData (m.getData(), m.getSize());
            std::printf ("seed %s: %d bytes, %d slices, mode %d\n", name, (int) m.getSize(), p.getShownTable().count, (int) raw (p, "slice_mode"));
        };

        PluginProcessor p;
        load (p, file);
        setP (p, "s02_pitch", 3.5f);
        setP (p, "s03_level", -6.0f);
        setP (p, "s04_pan", -40.0f);
        setP (p, "s05_reverse", 1.0f);
        setP (p, "s06_attack", 50.0f);
        setP (p, "s07_release", 200.0f);
        setP (p, "s20_level", -3.0f);
        p.windowWidth.store (1200);
        p.windowHeight.store (700);
        p.advancedOpen.store (true);
        write (p, "seed-default.state");

        auto t = p.getShownTable();
        p.moveMarker (2, t.start[2] + (t.start[3] - t.start[2]) / 3);
        t = p.getShownTable();
        p.addMarker ((t.start[4] + t.start[5]) / 2);
        write (p, "seed-manual.state");

        PluginProcessor g;
        load (g, file);
        setP (g, "slice_mode", (float) PluginProcessor::modeGrid);
        setP (g, "grid_type", (float) PluginProcessor::gridNote);
        setP (g, "grid_note", 3.0f);
        setP (g, "grid_tempo", 97.5f);
        setP (g, "slice_start", 150.0f);
        setP (g, "key_mode", (float) PluginProcessor::keyChromatic);
        setP (g, "selected", 3.0f);
        setP (g, "play_mode", (float) PluginProcessor::playLoop);
        setP (g, "root_note", 57.0f);
        setP (g, "s03_pitch", -2.25f);
        write (g, "seed-grid.state");
    }

    // ---- bugs ----

    void aliasing()
    {
        // cutup-dsp-r1-1. A tone whose pitched up copy lies above the host's Nyquist must come out as nothing, not fold back.
        const auto run = [] (double fileRate, double toneHz, int note)
        {
            PluginProcessor p;
            load (p, writeWav ("alias.wav", { tone ((int) (fileRate * 2.0), toneHz, fileRate, 0.5) }, fileRate));
            setP (p, "key_mode", (float) PluginProcessor::keyChromatic);
            setP (p, "vel_sens", 0.0f);
            setP (p, "root_note", 60.0f);
            prepare (p, 2, 48000.0, 256);
            const auto out = play (p, 8192, { { 0, on (note) } });
            const double level = rms (out.getReadPointer (0) + 1024, 4096) / (0.5 / std::sqrt (2.0));
            return toDb (level);
        };
        const double at48 = run (48000.0, 2500.0, 108);
        check (at48 <= -60.0, "Phase 2: a 2.5 kHz tone in a 48 kHz file played 48 st up (40 kHz, above Nyquist) comes out under the tone by", at48, "dB");
        const double at42 = run (48000.0, 2500.0, 102);
        check (at42 <= -60.0, "Phase 2: the same tone played 42 st up (28 kHz) comes out under the tone by", at42, "dB");
        const double at96 = run (96000.0, 5000.0, 96);
        check (at96 <= -60.0, "Phase 2: a 5 kHz tone in a 96 kHz file played 36 st up in a 48 kHz session (40 kHz) comes out under the tone by", at96, "dB");
        const double heard = run (48000.0, 2000.0, 84);
        check (std::abs (heard) < 0.5, "Phase 2:   while 24 st up (8 kHz, below Nyquist) still plays at the tone's level, dB", heard, "dB");
    }

    void zoomDrags()
    {
        // cutup-window-r1-2: a marker dragged 10 px in Zoom slice lands 10 px away.
        {
            PluginProcessor p;
            load (p, writeWav ("zoom.wav", { toneSegments (8, 0.25, 48000.0, 300.0, 170.0) }, 48000.0));
            toGrid (p, 4);
            auto window = openWindow (p);
            setP (p, "selected", 4.0f);
            window->setZoomed (true);
            window->refresh();
            auto& wave = window->getWaveform();
            const int before = p.getShownTable().start[3];
            const double framesPerPx = (double) (wave.getViewTo() - wave.getViewFrom()) / wave.getWidth();
            const float x0 = wave.xForFrame (before);
            wave.mouseDown (mouseAt (wave, { x0, 40.0f }, { x0, 40.0f }, false));
            for (int i = 1; i <= 10; ++i)
            {
                wave.mouseDrag (mouseAt (wave, { x0 - (float) i, 40.0f }, { x0, 40.0f }, true));
                window->refresh();
            }
            wave.mouseUp (mouseAt (wave, { x0 - 10.0f, 40.0f }, { x0, 40.0f }, true));
            const double moved = (before - p.getShownTable().start[3]) / framesPerPx;
            check (std::abs (moved - 10.0) <= 2.0, "Phase 2: in Zoom slice, a marker dragged 10 px left moves, px", moved, "px");
        }

        // cutup-window-r2-3: the Start tab dragged in Zoom slice stays under the pointer.
        {
            PluginProcessor p;
            load (p, writeWav ("zoomstart.wav", { toneSegments (8, 0.25, 48000.0, 300.0, 170.0) }, 48000.0));
            toGrid (p, 4);
            auto window = openWindow (p);
            setP (p, "selected", 1.0f);
            setP (p, "slice_start", 500.0f);
            window->setZoomed (true);
            window->refresh();
            auto& wave = window->getWaveform();
            const int from0 = wave.getViewFrom(), span0 = wave.getViewTo() - wave.getViewFrom();
            const auto tab = wave.startHandlePosition();
            wave.mouseDown (mouseAt (wave, tab, tab, false));
            double worst = 0.0;
            for (int i = 1; i <= 10; ++i)
            {
                const juce::Point<float> at (tab.x + 2.0f * (float) i, tab.y);
                wave.mouseDrag (mouseAt (wave, at, tab, true));
                window->refresh();
                const double underPointerMs = (from0 + at.x / wave.getWidth() * span0) / 48000.0 * 1000.0;
                worst = std::max (worst, std::abs (raw (p, "slice_start") - underPointerMs));
            }
            wave.mouseUp (mouseAt (wave, tab, tab, true));
            const double msPerPx = span0 / 48000.0 * 1000.0 / wave.getWidth();
            check (worst <= 2.0 * msPerPx + 1.0, "Phase 2: in Zoom slice, the dragged Start tab stays under the pointer, worst ms off", worst, "ms");
        }
    }

    void savedSlicesMatchSavedSettings()
    {
        // cutup-dsp-r2-1: Slice Start moving during a save must not bring the session back in Manual.
        std::vector<int> truth;
        PluginProcessor p;
        load (p, writeWav ("race.wav", { onsetFile (48000.0, truth) }, 48000.0));
        toGrid (p, 4);
        std::atomic<bool> stop { false };
        std::thread mover ([&]
        {
            auto* prm = p.apvts.getParameter ("slice_start");
            for (int i = 0; ! stop.load(); ++i)
                prm->setValueNotifyingHost (prm->convertTo0to1 (i % 2 == 0 ? 0.0f : 500.0f));
        });
        int manual = 0;
        for (int i = 0; i < 100; ++i)
        {
            const auto st = stateOf (p);
            PluginProcessor q;
            restore (q, st);
            if ((int) raw (q, "slice_mode") != PluginProcessor::modeGrid) ++manual;
        }
        stop = true;
        mover.join();
        check (manual == 0, "Phase 2: 100 saves while Slice Start automation moves: sessions that come back in Manual", manual);
    }

    void manualFromTheHost()
    {
        const auto file = writeWav ("manual.wav", { toneSegments (8, 0.25, 48000.0, 300.0, 170.0) }, 48000.0);

        // cutup-window-r2-1 (round 3): a host setting Manual gets the slices the processor holds, the same with the window
        // open as closed. Set through the parameter, as a host sets it, on 8 grid slices that differ from the default transient ones.
        {
            struct Result { SliceTable table, held; bool edited = false; juce::MemoryBlock state; };
            const auto hostSetsManual = [&] (bool windowOpen)
            {
                PluginProcessor p;
                load (p, file);
                toGrid (p, 4);
                std::unique_ptr<SliceEditor> window;
                if (windowOpen) window = openWindow (p);
                setP (p, "slice_mode", (float) PluginProcessor::modeManual);
                if (window != nullptr)
                {
                    window->refresh();
                    window->refresh();
                }
                return Result { p.getShownTable(), p.getSample()->initialManual, p.isManualEdited(), stateOf (p) };
            };
            const auto open = hostSetsManual (true), closed = hostSetsManual (false);
            int differ = std::abs (open.table.count - closed.table.count);
            for (int i = 0; i <= std::min (open.table.count, closed.table.count); ++i)
                differ += open.table.start[i] != closed.table.start[i] ? 1 : 0;
            check (open.table == closed.table && open.edited == closed.edited && open.table == open.held && ! open.edited,
                   "Phase 2: Manual set by the host gives the slices the processor holds with the window open as closed, slice starts that differ", differ);
            check (open.state == closed.state, "Phase 2:   and the session saved after it is the same bytes with the window open as closed");
        }

        // Picking Manual in the window still starts from the slices on show.
        {
            PluginProcessor p;
            load (p, file);
            toGrid (p, 4);
            auto window = openWindow (p);
            const auto onShow = p.getShownTable();
            auto* segments = findById<ui::Segments> (*window, "slice_mode");
            click (*segments, segments->centreOf (PluginProcessor::modeManual));
            window->refresh();
            check ((int) raw (p, "slice_mode") == PluginProcessor::modeManual && p.getShownTable() == onShow && p.isManualEdited(),
                   "Phase 2:   while picking Manual in the window starts from the 8 grid slices on show");
        }

        // cutup-window-r1-1: a state saved in Manual (not edited) restored while the window shows another mode keeps its slices.
        {
            PluginProcessor q;
            load (q, file);
            setP (q, "slice_mode", (float) PluginProcessor::modeManual);
            const auto savedTable = q.getShownTable();
            const auto a = stateOf (q);
            auto window = openWindow (q);
            setP (q, "slice_mode", (float) PluginProcessor::modeTransient);
            setP (q, "sensitivity", 90.0f);
            window->refresh();
            restore (q, a);
            window->refresh();
            window->refresh();
            check (q.getShownTable() == savedTable && ! q.isManualEdited(), "Phase 2: a Manual session restored with the window open keeps the slices it was saved with");
        }
    }

    void typedValues()
    {
        PluginProcessor p;
        auto* transpose = p.apvts.getParameter ("transpose");
        auto* root = p.apvts.getParameter ("root_note");
        const auto plainOf = [] (juce::RangedAudioParameter* prm, const char* text) { return prm->convertFrom0to1 (prm->getValueForText (text)); };

        // cutup-window-r2-2 (claimed): "+5" in Transpose.
        check (plainOf (transpose, "+5") == 5.0f && plainOf (transpose, "+5 st") == 5.0f, "Phase 2: Transpose reads +5 and +5 st as 5 (guard: the claim was not real)",
               plainOf (transpose, "+5 st"));

        // cutup-window-r3-2, cutup-dsp-r2-6: Root Note from text.
        const float eb3 = plainOf (root, "Eb3"), bb2 = plainOf (root, "Bb2"), db3 = plainOf (root, "Db3"), eb2 = plainOf (root, "Eb2");
        const float decimal = plainOf (root, "51.0"), empty = plainOf (root, ""), sharp = plainOf (root, "D#3"), plain = plainOf (root, "62");
        check (eb3 == 63.0f && bb2 == 58.0f && db3 == 61.0f && eb2 == 51.0f, "Phase 2: Root Note reads flat names Eb3 Bb2 Db3 Eb2, Eb3 reads as", eb3);
        check (decimal == 51.0f, "Phase 2: Root Note reads 51.0 as 51", decimal);
        check (empty == 60.0f, "Phase 2: Root Note reads empty text as its default, not C-2", empty);
        check (sharp == 63.0f && plain == 62.0f, "Phase 2:   and still reads D#3 and 62 (guard)");

        auto window = openWindow (p);
        auto* rootCell = findById<ui::ValueCell> (*window, "root_note");
        auto* transposeCell = findById<ui::ValueCell> (*window, "transpose");
        rootCell->applyTyped ("D3");
        rootCell->applyTyped ("Eb3");
        check (raw (p, "root_note") == 63.0f, "Phase 2: typing Eb3 into the Root cell sets", raw (p, "root_note"));
        rootCell->applyTyped ("nonsense 9");
        check (raw (p, "root_note") == 63.0f, "Phase 2: typing text that is no note leaves Root alone", raw (p, "root_note"));
        transposeCell->applyTyped ("+7 st");
        check (raw (p, "transpose") == 7.0f, "Phase 2: typing +7 st into Transpose sets 7 (guard)", raw (p, "transpose"));
    }

    void hostReset()
    {
        // cutup-dsp-r1-2.
        const auto file = writeWav ("reset.wav", { tone (96000, 330.0, 48000.0, 0.5) }, 48000.0);
        {
            PluginProcessor p;
            load (p, file);
            setP (p, "play_mode", (float) PluginProcessor::playOneShot);
            prepare (p, 2, 48000.0, 256);
            play (p, 256, { { 0, on (60) } });
            const auto allocsBefore = testkit::allocs.load();
            p.reset();
            const auto allocsInReset = testkit::allocs.load() - allocsBefore;
            const auto after = play (p, 2048, {});
            const double tail = peak (after.getReadPointer (0) + 240, 2048 - 240);
            check (tail == 0.0, "Phase 2: a One Shot, a host Reset, then 5 ms on: peak", toDb (tail), "dB");
            check (allocsInReset == 0, "Phase 2:   and the Reset allocates nothing, allocations", (double) allocsInReset);
        }
        {
            PluginProcessor p;
            load (p, file);
            prepare (p, 2, 48000.0, 256);
            play (p, 1024, { { 0, juce::MidiMessage::controllerEvent (1, 64, 127) }, { 10, on (60) }, { 500, off (60) } });
            p.reset();
            const auto out = play (p, 24000, { { 0, on (61) }, { 2000, off (61) } });
            const double late = peak (out.getReadPointer (0) + 2000 + 4800, 4800);
            check (late == 0.0, "Phase 2: after a Reset the pedal is up, so a note released 100 ms ago is silent, peak", toDb (late), "dB");
        }
        {
            // Play, reset, play gives what a new instance gives.
            PluginProcessor a, b;
            load (a, file);
            load (b, file);
            for (auto* p : { &a, &b }) { setP (*p, "play_mode", (float) PluginProcessor::playOneShot); prepare (*p, 2, 48000.0, 256); }
            play (a, 4096, { { 0, on (60) }, { 100, juce::MidiMessage::pitchWheel (1, 12000) } });
            a.reset();
            play (b, 256, {});
            const std::vector<Ev> notes { { 0, on (60) }, { 3000, off (60) } };
            const auto oa = play (a, 12000, notes), ob = play (b, 12000, notes);
            int differ = 0;
            for (int i = 0; i < 12000; ++i) differ += oa.getSample (0, i) != ob.getSample (0, i) ? 1 : 0;
            check (differ == 0, "Phase 2: play, Reset, play equals a new instance, samples that differ", differ);
        }
        {
            // The Reset JUCE's AU wrapper sends as the host's bypass switches on is left out, so the sound fades instead of stopping.
            PluginProcessor a, b;
            load (a, file);
            load (b, file);
            for (auto* p : { &a, &b }) { setP (*p, "play_mode", (float) PluginProcessor::playOneShot); prepare (*p, 2, 48000.0, 256); play (*p, 2048, { { 0, on (60) } }); }
            for (auto* p : { &a, &b }) p->getBypassParameter()->setValueNotifyingHost (1.0f);
            a.reset();
            const auto oa = play (a, 2048, {}), ob = play (b, 2048, {});
            int differ = 0;
            for (int i = 0; i < 2048; ++i) differ += oa.getSample (0, i) != ob.getSample (0, i) ? 1 : 0;
            check (differ == 0, "Phase 2: the Reset that comes with host bypass switching on changes nothing, samples that differ", differ);
        }
    }

    void leadInKeepsStrongest()
    {
        // cutup-dsp-r1-3: 40 candidates, strongest first, the strongest the latest of all, and a lead-in before the first.
        std::vector<slicecore::Candidate> c;
        const int frames = 48000 * 30;
        c.push_back ({ 48000 * 28, 1.0f });                        // strongest, latest
        for (int i = 0; i < 39; ++i) c.push_back ({ 48000 + i * 24000, 0.9f - 0.01f * (float) i });
        const auto t = slicecore::pickTransients (c.data(), (int) c.size(), 0, frames, 50.0f, 2400, 0);
        bool kept = false;
        for (int i = 0; i < t.count; ++i) kept = kept || t.start[i] == 48000 * 28;
        bool weakestKept = false;
        for (int i = 0; i < t.count; ++i) weakestKept = weakestKept || t.start[i] == 48000 + 30 * 24000;
        // Not fixed: keeping the strongest instead changes where a long take is cut, and on Luke's takes that gave Vowel Hold two new
        // loop clicks (takes 10 and 14) that 1.2.0 does not have. Real vocal lines that passed may not fail, so 1.2.0's choice stays.
        info ("Phase 2: NOT FIXED cutup-dsp-r1-3, with a lead-in slice the strongest onset keeps its cut and the weakest of the 32 goes (1 yes, 0 no)",
              kept && ! weakestKept && t.start[0] == 0 && t.count == 32 ? 1.0 : 0.0);
    }

    void keyboardSettles()
    {
        // cutup-window-r1-3.
        PluginProcessor p;
        load (p, writeWav ("keys.wav", { toneSegments (8, 0.25, 48000.0, 300.0, 170.0) }, 48000.0));
        toGrid (p, 4);
        auto window = openWindow (p);
        auto& keys = window->getKeyMap();
        const auto from = keys.rootTabCentre();
        const auto to = keys.keyCentre (keys.getLastKey());
        keys.mouseDown (mouseAt (keys, from, from, false));
        keys.mouseDrag (mouseAt (keys, to, from, true));
        window->refresh();
        keys.mouseUp (mouseAt (keys, to, from, true));
        (void) keys.createComponentSnapshot (keys.getLocalBounds());
        window->refresh();
        const int rootNow = (int) raw (p, "root_note");
        check (keys.getLastKey() >= rootNow + 7, "Phase 2: after a Root drag the keyboard settles to show all 8 slices, last key minus root", keys.getLastKey() - rootNow);
    }

    void longSamples()
    {
        // cutup-window-r1-4, cutup-dsp-r2-4, cutup-window-r3-5: an 1800 s file at 8 kHz, inside the load limit.
        const double rate = 8000.0;
        std::vector<float> x ((size_t) (1800 * 8000), 0.0f);
        for (int k = 0; k < 60; ++k)
            for (int i = 0; i < 4000; ++i)
                x[(size_t) (k * 30 * 8000 + i)] = (float) (0.5 * std::sin (kTwoPi * 440.0 * i / rate) * std::exp (-i / 1500.0));
        PluginProcessor p;
        const bool loaded = load (p, writeWav ("long.wav", { x }, rate));
        x = {};
        check (loaded && p.getSample() != nullptr && p.getSample()->frames() == 1800 * 8000, "Phase 2: an 1800 s file at 8 kHz loads");
        prepare (p, 2, 48000.0, 256);
        play (p, 256, {});
        juce::String text;
        for (auto* prm : p.getParameters())
            if (auto* r = dynamic_cast<ReadingParameter*> (prm))
                if (r->paramID == "sample_length") text = r->getCurrentValueAsText();
        check (text == "1800.00", "Phase 2: its Sample Length reading says " + text.toStdString());

        auto window = openWindow (p, PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        auto& wave = window->getWaveform();
        const double step = wave.timeStepSeconds();
        const double seconds = 1800.0;
        const double spacing = wave.getWidth() * step / seconds;
        const double last = std::floor ((seconds - 0.001) / step) * step;
        const auto label = step >= 1.0 ? juce::String (juce::roundToInt (last)) + " s" : juce::String (last, 1) + " s";
        const double need = textWidth (ui::font (ui::Weight::regular, 11.0f, true), label) + 4.0;
        check (spacing >= need, "Phase 2: time scale labels of a 1800 s sample do not overlap at the smallest size, px spare", spacing - need, "px");
    }

    void stepperInChromatic()
    {
        // cutup-window-r1-5.
        PluginProcessor p;
        load (p, writeWav ("chrom.wav", { toneSegments (8, 0.25, 48000.0, 300.0, 170.0) }, 48000.0));
        toGrid (p, 4);
        setP (p, "key_mode", (float) PluginProcessor::keyChromatic);
        setP (p, "root_note", 60.0f);
        setP (p, "selected", 5.0f);
        auto window = openWindow (p);
        check (window->getSelectedText().endsWith ("C3"), "Phase 2: in Chromatic, the stepper names Root as slice 5's key: " + window->getSelectedText().toStdString());
    }

    double pitchOf (PluginProcessor& p, const std::vector<Ev>& ev, int from, int len)
    {
        const auto out = play (p, from + len, ev);
        return measureFreq (out.getReadPointer (0) + from, len, 48000.0);
    }

    void bendRange()
    {
        // cutup-dsp-r2-2 and cutup-dsp-r3-1.
        const auto file = writeWav ("bend.wav", { tone (96000, 300.0, 48000.0, 0.5) }, 48000.0);
        {
            PluginProcessor p;
            load (p, file);
            prepare (p, 2, 48000.0, 256);
            setP (p, "bend_range", 2.0f);
            play (p, 256, { { 0, juce::MidiMessage::pitchWheel (1, 16383) } });
            setP (p, "bend_range", 12.0f);
            const double f = pitchOf (p, { { 0, on (60) } }, 2048, 24000);
            const double want = 300.0 * std::pow (2.0, 12.0 * (16383 - 8192) / 8192.0 / 12.0);
            check (std::abs (centsBetween (f, want)) < 5.0, "Phase 2: wheel up, then Bend Range 2 to 12: cents off 12 st", centsBetween (f, want), "cents");
        }
        {
            PluginProcessor p;
            load (p, file);
            prepare (p, 2, 48000.0, 256);
            play (p, 256, { { 0, juce::MidiMessage::pitchWheel (1, 16383) } });
            setP (p, "bend_range", 0.0f);
            const double f = pitchOf (p, { { 0, on (60) } }, 2048, 24000);
            check (std::abs (centsBetween (f, 300.0)) < 5.0, "Phase 2: wheel up, then Bend Range 0: cents off the unbent pitch", centsBetween (f, 300.0), "cents");
        }
        {
            PluginProcessor p;
            load (p, file);
            prepare (p, 2, 48000.0, 256);
            const auto out = play (p, 48000, { { 0, juce::MidiMessage::controllerEvent (1, 64, 127) }, { 0, juce::MidiMessage::pitchWheel (1, 16383) },
                                               { 10, juce::MidiMessage::controllerEvent (1, 121, 0) }, { 20, on (60) }, { 12000, off (60) } });
            const double f = measureFreq (out.getReadPointer (0) + 2048, 8000, 48000.0);
            check (std::abs (centsBetween (f, 300.0)) < 5.0, "Phase 2: Reset All Controllers puts the wheel back: cents off the unbent pitch", centsBetween (f, 300.0), "cents");
            const double late = peak (out.getReadPointer (0) + 12000 + 4800, 4800);
            check (late == 0.0, "Phase 2:   and the pedal up, so the note released 100 ms ago is silent, peak", toDb (late), "dB");
        }
    }

    void markerEditDuringLoads()
    {
        // cutup-dsp-r2-3: a sample that lands in the middle of a hand edit must not get the old sample's markers.
        const auto a = writeWav ("raceA.wav", { toneSegments (6, 0.08, 48000.0, 300.0, 170.0) }, 48000.0);
        const auto b = writeWav ("raceB.wav", { toneSegments (7, 0.08, 48000.0, 400.0, 150.0) }, 48000.0);
        PluginProcessor p;
        load (p, a);
        toGrid (p, 2);
        p.testHookInMarkerEdit = [&]
        {
            p.testHookInMarkerEdit = nullptr;
            p.loadFile (b);
            p.waitForLoad (10000);
        };
        const auto t0 = p.getShownTable();
        p.moveMarker (1, (t0.start[1] + t0.start[2]) / 2);
        p.testHookInMarkerEdit = nullptr;
        const auto t = p.getShownTable();
        const auto s = p.getSample();
        check (s != nullptr && s->frames() == 26880 && t.start[t.count] == s->frames(), "Phase 2: a sample loaded during a marker edit keeps markers that fit it, end marker at",
               t.start[t.count]);
        PluginProcessor q;
        restore (q, stateOf (p));
        check (q.getSample() != nullptr && ! q.getSample()->isEmpty(), "Phase 2:   and its session saves and loads with the sample in it");
    }

    void cancelledLoad()
    {
        // cutup-dsp-r2-5: a file still loading, a second file asked for, then a session restored before the second starts.
        const auto big = writeWav ("cancelBig.wav", { tone (48000 * 40, 200.0, 48000.0, 0.3), tone (48000 * 40, 250.0, 48000.0, 0.3) }, 48000.0);
        const auto small = writeWav ("cancel.wav", { toneSegments (4, 0.25, 48000.0, 300.0, 170.0) }, 48000.0);
        PluginProcessor empty;
        const auto emptyState = stateOf (empty);
        int stuck = 0;
        for (int i = 0; i < 10; ++i)
        {
            PluginProcessor p;
            p.loadFile (big);
            juce::Thread::sleep (5);
            p.loadFile (small);
            restore (p, emptyState);
            p.waitForLoad (20000);
            if (p.getStatus().startsWith ("Loading")) ++stuck;
        }
        check (stuck == 0, "Phase 2: a load cancelled by a session restore leaves no Loading message, of 10", stuck);
    }

    void advancedRestored()
    {
        // cutup-window-r2-4.
        PluginProcessor q;
        q.advancedOpen.store (true);
        const auto withAdvanced = stateOf (q);
        PluginProcessor p;
        auto window = openWindow (p);
        restore (p, withAdvanced);
        window->refresh();
        auto* minSlice = findById<ui::ValueCell> (*window, "min_slice");
        check (minSlice->isVisible() == window->isAdvancedOpen(), "Phase 2: a state with Advanced open, restored with the window open, shows the drawer");
    }

    void valuesFit()
    {
        // cutup-window-r2-5, cutup-window-r2-6: the value text fits its cell.
        PluginProcessor p;
        load (p, writeWav ("fit.wav", { toneSegments (8, 0.25, 48000.0, 300.0, 170.0) }, 48000.0));
        const auto valueFont = ui::font (ui::Weight::medium, 16.0f, true);
        for (const auto size : { juce::Point<int> (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight), juce::Point<int> (1160, 740) })
        {
            auto window = openWindow (p, size.x, size.y);
            const auto over = [&] (const char* id, float value)
            {
                setP (p, id, value);
                window->refresh();
                auto* cell = findById<ui::ValueCell> (*window, id);
                return (double) textWidth (valueFont, cell->valueText()) - cell->getWidth();
            };
            setP (p, "slice_mode", (float) PluginProcessor::modeGrid);
            setP (p, "grid_type", (float) PluginProcessor::gridNote);
            window->refresh();
            const double tempo = over ("grid_tempo", 128.5f);
            check (tempo <= 0.0, "Phase 2: at " + std::to_string (size.x) + " px, Grid Tempo 128.50 fits its cell, px over", tempo, "px");
            double worst = -1000.0;
            for (const auto& [id, v] : std::vector<std::pair<const char*, float>> { { "s01_level", -60.0f }, { "s01_level", 12.0f }, { "s01_level", -18.0f },
                                                                                   { "s01_pitch", -12.34f }, { "penv_amount", -12.34f } })
                worst = std::max (worst, over (id, v));
            check (worst <= 0.0, "Phase 2: at " + std::to_string (size.x) + " px, slot Level and Pitch and glide Amount fit their cells, worst px over", worst, "px");
            for (auto* id : { "s01_level", "s01_pitch", "penv_amount", "grid_tempo" })
                setP (p, id, p.apvts.getParameter (id)->convertFrom0to1 (p.apvts.getParameter (id)->getDefaultValue()));
            setP (p, "slice_mode", 0.0f);
            setP (p, "grid_type", 0.0f);
        }
    }

    void menuDoubleClick()
    {
        // cutup-window-r2-8: a list waits as long as a double-click can take before it opens.
        PluginProcessor p;
        auto window = openWindow (p);
        int shortest = 1 << 30;
        for (auto* id : { "play_mode", "grid", "grid_note" })
        {
            // Grid and Grid Note only show, and so only have a size to click in, in their own Slice Mode and Grid Type.
            setP (p, "slice_mode", juce::String (id) == "play_mode" ? 0.0f : (float) PluginProcessor::modeGrid);
            setP (p, "grid_type", juce::String (id) == "grid_note" ? (float) PluginProcessor::gridNote : (float) PluginProcessor::gridEqual);
            window->refresh();
            auto* field = findById<ui::MenuField> (*window, id);
            const auto c = field->getLocalBounds().getCentre().toFloat();
            field->mouseUp (mouseAt (*field, c, c, false));
            auto* timer = (juce::Timer*) field;   // the field's own timer opens the list
            shortest = std::min (shortest, timer->isTimerRunning() ? timer->getTimerInterval() : 0);
            timer->stopTimer();
        }
        check (shortest >= juce::MouseEvent::getDoubleClickTimeout(), "Phase 2: Play Mode, Grid and Grid Note wait out a double-click before opening, ms", shortest, "ms");
    }

    void typingWhileSelectionMoves()
    {
        // cutup-window-r3-1.
        PluginProcessor p;
        load (p, writeWav ("typing.wav", { toneSegments (8, 0.25, 48000.0, 300.0, 170.0) }, 48000.0));
        auto window = openWindow (p);
        auto* level = findById<ui::ValueCell> (*window, "s01_level");
        level->startTyping();
        p.selectSlice (3);
        window->refresh();
        level->applyTyped ("-6");
        check (raw (p, "s01_level") == -6.0f && raw (p, "s04_level") == 0.0f, "Phase 2: a value typed while Selected Slice moves lands on the slice it was typed for");
        if (auto* box = dynamic_cast<juce::Label*> (level->getChildComponent (0)))
            box->hideEditor (true);
    }

    void chromaticKeys()
    {
        // cutup-window-r3-3.
        PluginProcessor p;
        load (p, writeWav ("keyclick.wav", { tone (48000, 300.0, 48000.0, 0.5) }, 48000.0));
        setP (p, "key_mode", (float) PluginProcessor::keyChromatic);
        setP (p, "root_note", 60.0f);
        prepare (p, 2, 48000.0, 256);
        auto window = openWindow (p);
        auto& keys = window->getKeyMap();
        click (keys, keys.keyCentre (72));
        const auto out = play (p, 8192, {});
        window->refresh();
        const double f = measureFreq (out.getReadPointer (0) + 1024, 6000, 48000.0);
        check (std::abs (centsBetween (f, 600.0)) < 5.0, "Phase 2: in Chromatic, clicking the key an octave up plays an octave up, cents off", centsBetween (f, 600.0), "cents");
        check (keys.isLit (72) && ! keys.isLit (60), "Phase 2:   and lights the key that was clicked");
    }

    void advancedSummaryFits()
    {
        // cutup-window-r3-4.
        PluginProcessor p;
        setP (p, "min_slice", 80.0f);
        setP (p, "transpose", -12.0f);
        setP (p, "bend_range", 24.0f);
        for (const auto size : { juce::Point<int> (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight), juce::Point<int> (1160, 740) })
        {
            auto window = openWindow (p, size.x, size.y);
            window->setAdvancedOpen (false);
            window->refresh();
            const auto summary = window->getAdvancedSummary();
            const double over = textWidth (ui::font (ui::Weight::regular, 13.0f), summary) - (window->getAdvancedHeader().getWidth() - 90);
            check (summary.isNotEmpty() && over <= 0.0, "Phase 2: at " + std::to_string (size.x) + " px, the closed Advanced summary of three changes fits, px over", over, "px");
        }
    }

    // ---- presets ----

    void presetSaved()
    {
        PluginProcessor p;
        p.setCurrentProgram (5);
        const auto st = stateOf (p);
        PluginProcessor q;
        restore (q, st);
        check (q.getCurrentProgram() == 5, "Phase 2: a session saved after choosing Reverse Swell names it again, preset", q.getCurrentProgram());
        check (! q.apvts.state.hasProperty ("program"), "Phase 2:   and the preset stays out of the parameter tree");

        PluginProcessor fresh;
        const auto xml = juce::AudioProcessor::getXmlFromBinary (stateOf (fresh).getData(), (int) stateOf (fresh).getSize());
        check (xml != nullptr && ! xml->hasAttribute ("program"), "Phase 2: a new instance where no preset was chosen saves none (guard)");
    }

    // ---- the installed 1.2.0's own sessions ----

    void oldSessions()
    {
        const auto dir = baselineDir();
        juce::StringArray lines;
        lines.addLines (dir.getChildFile ("v1.2.0-renders.txt").loadFileAsString());
        int found = 0, sound = 0, bytes = 0, program = 0, cuts = 0, params = 0, names = 0;
        for (const auto& line : lines)
        {
            const auto parts = juce::StringArray::fromTokens (line, " ", "");
            if (parts.size() < 2) continue;
            juce::MemoryBlock st;
            if (! dir.getChildFile ("v1.2.0-" + parts[0] + ".state").loadFileAsData (st)) continue;
            ++found;

            PluginProcessor p;
            prepare (p, 2, 48000.0, kScoreBlock);
            restore (p, st);
            p.waitForLoad (10000);
            if (p.getCurrentProgram() != -1) ++program;
            if (stateOf (p) != st) ++bytes;

            const auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
            const auto* e = xml != nullptr ? xml->getChildByName ("SMOOTH_SLICE") : nullptr;
            if (e == nullptr || p.getSample() == nullptr || p.getSample()->name != e->getStringAttribute ("name")) ++names;
            juce::String shown;
            const auto t = p.getShownTable();
            for (int i = 0; i <= t.count; ++i) shown << (i > 0 ? " " : "") << t.start[i];
            if (e == nullptr || shown != e->getStringAttribute ("slices")) ++cuts;
            if (xml != nullptr)
                for (auto* prmXml : xml->getChildWithTagNameIterator ("PARAM"))
                    if (auto* prm = p.apvts.getParameter (prmXml->getStringAttribute ("id")))
                        if (std::abs (prm->convertFrom0to1 (prm->getValue()) - (float) prmXml->getDoubleAttribute ("value")) > 1.0e-4f) ++params;

            if (juce::String::toHexString ((juce::int64) playScore (p)) != parts[1]) ++sound;
        }
        check (found == 17, "Phase 2: the installed 1.2.0's sessions are there (default, 14 presets, Manual, Grid)", found);
        check (sound == 0, "Phase 2: each plays the score bit for bit as the installed 1.2.0 played it, sessions that differ", sound);
        check (bytes == 0, "Phase 2: each saves back the same bytes, sessions that differ", bytes);
        check (names == 0 && cuts == 0, "Phase 2: each keeps its sample and its cuts, sessions that differ", names + cuts);
        check (params == 0, "Phase 2: each keeps every control and slot as saved, values that differ", params);
        check (program == 0, "Phase 2: each reads as no preset chosen, sessions that do not", program);
    }

    // ---- improvement: the sample file moved or deleted ----

    void missingFile()
    {
        const auto src = writeWav ("moving.wav", { toneSegments (6, 0.25, 48000.0, 300.0, 170.0) }, 48000.0);
        const auto copy = tempDir().getChildFile ("Lead vocal take 3.wav");
        src.copyFileTo (copy);
        PluginProcessor p;
        load (p, copy);
        toGrid (p, 3);
        p.moveMarker (2, p.getShownTable().start[2] + 2000);
        const auto table = p.getShownTable();
        const auto st = stateOf (p);
        copy.deleteFile();

        PluginProcessor q;
        restore (q, st);
        auto window = openWindow (q);
        const bool all = q.getSample() != nullptr && q.getSample()->name == "Lead vocal take 3.wav" && q.getShownTable() == table && q.isManualEdited();
        check (all, "Phase 2: with its file deleted, a session keeps its sample, name and hand-moved cuts (guard: the audio is in the session)");
        check (window->getStatusText().startsWith ("Lead vocal take 3.wav"), "Phase 2:   and the window names it: " + window->getStatusText().toStdString());
        check (stateOf (q) == st, "Phase 2:   and the next save keeps it all, byte for byte");

        // A session whose saved sample was damaged says which sample it was.
        auto damaged = st;
        static_cast<char*> (damaged.getData())[damaged.getSize() - 100] ^= 0x40;
        PluginProcessor r;
        restore (r, damaged);
        auto window2 = openWindow (r);
        check (window2->getStatusText().contains ("Lead vocal take 3.wav"), "Phase 2: a damaged saved sample is named in the window: " + window2->getStatusText().toStdString());
    }

    // The audio CPU with a sample loaded, which AuBench cannot do: 16 looping stereo voices, as testCpu, at three pitches.
    void cpuWithSample()
    {
        const double rate = 48000.0;
        const auto x = toneSegments (16, 1.0, rate, 300.0, 137.0, 0.05);
        const auto file = writeWav ("cpu2.wav", { x, x }, rate);
        for (const float up : { 0.0f, 7.0f, 40.0f })
        {
            PluginProcessor p;
            load (p, file);
            toGrid (p, 6);
            setP (p, "polyphony", 16.0f);
            setP (p, "play_mode", (float) PluginProcessor::playLoop);
            setP (p, "transpose", std::min (24.0f, up));
            for (int k = 0; k < 16; ++k) setP (p, PluginProcessor::slotId (k, "pitch").toRawUTF8(), up - std::min (24.0f, up));
            prepare (p, 2, rate, 512);
            std::vector<Ev> ev;
            for (int k = 0; k < 16; ++k) ev.push_back ({ 0, on (60 + k) });
            const auto start = juce::Time::getMillisecondCounterHiRes();
            (void) play (p, 480000, ev, 512);
            info ("Phase 2: 16 stereo voices looping " + std::to_string ((int) up) + " st up: share of one core", (juce::Time::getMillisecondCounterHiRes() - start) / 100.0, "%");
        }
    }

    void run()
    {
        aliasing();
        zoomDrags();
        savedSlicesMatchSavedSettings();
        manualFromTheHost();
        typedValues();
        hostReset();
        leadInKeepsStrongest();
        keyboardSettles();
        longSamples();
        stepperInChromatic();
        bendRange();
        markerEditDuringLoads();
        cancelledLoad();
        advancedRestored();
        valuesFit();
        menuDoubleClick();
        typingWhileSelectionMoves();
        chromaticKeys();
        advancedSummaryFits();
        presetSaved();
        oldSessions();
        missingFile();
        cpuWithSample();
    }
}
