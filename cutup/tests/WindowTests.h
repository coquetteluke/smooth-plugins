#pragma once
// The window, tested without a screen. Included by PluginTest.cpp after its helpers.

namespace windowtests
{
    // Every output sample of a fixed performance, hashed bit for bit.
    inline std::uint64_t hashBlock (std::uint64_t h, const juce::AudioBuffer<float>& b, int n)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const auto* x = reinterpret_cast<const unsigned char*> (b.getReadPointer (ch));
            for (size_t i = 0; i < (size_t) n * sizeof (float); ++i)
            {
                h ^= x[i];
                h *= 1099511628211ull;
            }
        }
        return h;
    }

    inline std::vector<float> fingerprintAudio()
    {
        auto x = toneSegments (12, 0.20, 48000.0, 220.0, 95.0);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] *= (float) (0.35 + 0.65 * std::exp (-(double) (i % 9600) / 2400.0));
        return x;
    }

    // Plays one preset with some slots moved, through one real processor. With a window, it stays open and draws the whole way.
    inline std::uint64_t outputFingerprint (const juce::File& file, int preset, bool withWindow)
    {
        PluginProcessor p;
        load (p, file);
        p.setCurrentProgram (preset);
        setP (p, "s02_pitch", 3.0f);
        setP (p, "s03_level", -6.0f);
        setP (p, "s04_pan", -40.0f);
        setP (p, "s05_reverse", 1.0f);
        setP (p, "s06_attack", 40.0f);
        setP (p, "transpose", 0.0f);
        prepare (p, 2, 48000.0, 256);

        std::unique_ptr<SliceEditor> window (withWindow ? dynamic_cast<SliceEditor*> (p.createEditor()) : nullptr);

        constexpr int block = 256, total = 48000 * 3;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        std::uint64_t h = 14695981039346656037ull;
        int blockIndex = 0;

        for (int pos = 0; pos < total; pos += block, ++blockIndex)
        {
            buffer.clear();
            midi.clear();
            const int step = pos / 6000;
            if (pos % 6000 < block)
            {
                midi.addEvent (on (60 + (step * 5) % 12, 60 + (step * 13) % 67), pos % 6000);
                if (step > 0)
                    midi.addEvent (off (60 + ((step - 1) * 5) % 12), pos % 6000);
            }
            p.processBlock (buffer, midi);
            h = hashBlock (h, buffer, block);

            if (window != nullptr)
            {
                window->refresh();
                if (blockIndex % 16 == 0)
                    window->createComponentSnapshot (window->getLocalBounds());
            }
        }
        return h;
    }

    // From `PluginTest fingerprints` at commit a6207be: the 1.0.0 processor and its plain window.
    struct Fingerprint { int preset; std::uint64_t sum; };
    constexpr Fingerprint before11[] = {
        { 0, 9270065411128525949ull }, { 1, 16092510606060473473ull }, { 2, 12321090339321958136ull }, { 3, 14699743656941204444ull },
        { 4, 13420086507191853742ull }, { 5, 718716507959506807ull }, { 6, 6929569582261721549ull }, { 7, 2365734372308282001ull },
        { 8, 4313120883877692087ull }, { 9, 6175778323170859343ull }, { 10, 6248031368819116338ull }, { 11, 9131364516839188301ull },
        { 12, 8471114080684322497ull }, { 13, 6351017173171457224ull } };

    template <typename T> T* findById (juce::Component& c, const juce::String& id)
    {
        if (c.getComponentID() == id)
            if (auto* typed = dynamic_cast<T*> (&c))
                return typed;
        for (auto* child : c.getChildren())
            if (auto* found = findById<T> (*child, id))
                return found;
        return nullptr;
    }

    // Really on screen: every parent up to the window shows, it has a size, and it fits inside the window.
    inline bool showsInWindow (juce::Component& c, juce::Component& window)
    {
        if (c.getWidth() <= 0 || c.getHeight() <= 0)
            return false;
        for (auto* at = &c; at != &window; at = at->getParentComponent())
            if (at == nullptr || ! at->isVisible())
                return false;
        return window.getLocalBounds().contains (window.getLocalArea (&c, c.getLocalBounds()));
    }

    struct GestureCount final : juce::AudioProcessorParameter::Listener
    {
        int begins = 0, ends = 0, changes = 0;
        void parameterValueChanged (int, float) override { ++changes; }
        void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
    };

    inline juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> at, juce::Point<float> downAt, bool dragged, int clicks = 1,
                                     juce::ModifierKeys mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier))
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, downAt, now, clicks, dragged);
    }

    inline void click (juce::Component& c, juce::Point<float> at)
    {
        c.mouseDown (mouseAt (c, at, at, false));
        c.mouseUp (mouseAt (c, at, at, false));
    }

    inline void drag (juce::Component& c, juce::Point<float> from, juce::Point<float> to)
    {
        c.mouseDown (mouseAt (c, from, from, false));
        c.mouseDrag (mouseAt (c, to, from, true));
        c.mouseUp (mouseAt (c, to, from, true));
    }

    // Drags a cell the way a mouse does, away from whichever end it is near.
    inline void dragCell (ui::ValueCell& cell)
    {
        const auto centre = cell.getLocalBounds().getCentre().toFloat();
        const float way = cell.valueToProportionOfLength (cell.getValue()) > 0.5 ? -1.0f : 1.0f;
        drag (cell, centre, centre + juce::Point<float> (way * 90.0f, 0.0f));
    }

    inline void collectAll (juce::Component& c, std::vector<juce::Component*>& out)
    {
        out.push_back (&c);
        for (auto* child : c.getChildren())
            collectAll (*child, out);
    }

    struct Prover
    {
        PluginProcessor& p;
        SliceEditor& window;
        std::set<juce::String> proved;
        int failuresHere = 0;

        void fail (const juce::String& what)
        {
            ++failuresHere;
            if (failuresHere < 20)
                check (false, what.toStdString());
        }

        juce::RangedAudioParameter* param (const juce::String& id) { return p.apvts.getParameter (id); }

        // One way into every control: click what a person would click, then look for it on screen, then move it.
        void revealMode (int mode, int gridType = -1)
        {
            auto* modes = findById<ui::Segments> (window, "slice_mode");
            click (*modes, modes->centreOf (mode));
            window.refresh();
            if (gridType >= 0)
            {
                auto* types = findById<ui::Segments> (window, "grid_type");
                click (*types, types->centreOf (gridType));
                window.refresh();
            }
        }

        void openAdvanced()
        {
            if (! window.isAdvancedOpen())
                click (*findById<juce::Component> (window, "advanced"), { 20.0f, 8.0f });
            window.refresh();
        }

        void proveCell (const juce::String& id)
        {
            auto* cell = findById<ui::ValueCell> (window, id);
            if (cell == nullptr || ! showsInWindow (*cell, window))
                return fail (id + ": no cell on screen");

            auto* prm = param (id);
            GestureCount count;
            prm->addListener (&count);
            const float before = prm->getValue();
            dragCell (*cell);
            const float moved = prm->getValue();
            prm->removeListener (&count);

            if (std::abs (moved - before) < 1.0e-4f)
                return fail (id + ": dragging its cell did not move it");
            if (count.begins != 1 || count.ends != 1)
                return fail (id + ": gesture begins " + juce::String (count.begins) + " ends " + juce::String (count.ends));

            cell->mouseDoubleClick (mouseAt (*cell, { 5.0f, 5.0f }, { 5.0f, 5.0f }, false, 2));
            if (std::abs (prm->getValue() - prm->getDefaultValue()) > 1.0e-4f)
                return fail (id + ": double-click did not reset it");
            proved.insert (id);
        }

        void provePill (const juce::String& id)
        {
            auto* pill = findById<ui::Pill> (window, id);
            if (pill == nullptr || ! showsInWindow (*pill, window))
                return fail (id + ": no button on screen");
            auto* prm = param (id);
            GestureCount count;
            prm->addListener (&count);
            const float before = prm->getValue();
            click (*pill, { 3.0f, 3.0f });
            const float moved = prm->getValue();
            pill->mouseDown (mouseAt (*pill, { 3.0f, 3.0f }, { 3.0f, 3.0f }, false));
            prm->removeListener (&count);
            if (std::abs (moved - before) < 0.5f || std::abs (prm->getValue() - before) > 1.0e-4f || count.begins != 2 || count.ends != 2)
                return fail (id + ": its button does not toggle it, with gestures");
            proved.insert (id);
        }

        void proveSegments (const juce::String& id)
        {
            auto* seg = findById<ui::Segments> (window, id);
            if (seg == nullptr || ! showsInWindow (*seg, window))
                return fail (id + ": no switch on screen");
            auto* prm = param (id);
            const int first = seg->getSelected();
            for (int i = 0; i < seg->getNumChoices(); ++i)
            {
                click (*seg, seg->centreOf (i));
                if (juce::roundToInt (prm->convertFrom0to1 (prm->getValue())) != i)
                    return fail (id + ": option " + juce::String (i) + " did not take");
            }
            click (*seg, seg->centreOf (first));
            window.refresh();
            proved.insert (id);
        }

        // A list opens a real menu, which needs a screen. Its menu calls choose with the row picked, so that is what runs here.
        void proveMenu (const juce::String& id)
        {
            auto* menu = findById<ui::MenuField> (window, id);
            if (menu == nullptr || ! showsInWindow (*menu, window))
                return fail (id + ": no list on screen");
            auto* prm = param (id);
            const int first = menu->getSelected();
            for (int i = 0; i < menu->getNumChoices(); ++i)
            {
                menu->choose (i);
                if (juce::roundToInt (prm->convertFrom0to1 (prm->getValue())) != i)
                    return fail (id + ": row " + juce::String (i) + " did not take");
            }
            menu->choose (first);
            proved.insert (id);
        }

        void proveSlots()
        {
            auto& stepper = window.getStepperComponent();
            const auto next = window.stepperNextArrow();
            const char* cells[] = { "pitch", "level", "pan", "attack", "release" };

            for (int slot = 0; slot < PluginProcessor::kSlots; ++slot)
            {
                if (slot > 0)
                    click (stepper, next);
                window.refresh();

                if (p.getSelectedIndex() != slot)
                    return fail ("the arrow did not reach slot " + juce::String (slot + 1));

                for (auto* what : cells)
                {
                    const auto id = PluginProcessor::slotId (slot, what);
                    const auto movedBefore = snapshotSlots();
                    proveCell (id);
                    // The drag must have reached this parameter and no other slot's. proveCell resets it again afterwards.
                    if (snapshotSlots() != movedBefore)
                        fail (id + ": left another slot's parameter changed");
                }
                provePill (PluginProcessor::slotId (slot, "reverse"));

                // Every other slot's controls are gone: one set of controls, following the selection.
                if (slot > 0 && findById<juce::Component> (window, PluginProcessor::slotId (slot - 1, "pitch")) != nullptr)
                    fail ("slot " + juce::String (slot) + "'s Pitch is still in the window after moving on");
            }
            proved.insert ("selected");

            // And back down, by the other arrow.
            const auto previous = window.stepperPreviousArrow();
            for (int slot = PluginProcessor::kSlots - 1; slot > 0; --slot)
                click (stepper, previous);
            window.refresh();
            if (p.getSelectedIndex() != 0)
                fail ("the back arrow did not reach slot 1");
        }

        std::vector<float> snapshotSlots()
        {
            std::vector<float> all;
            for (int s = 0; s < PluginProcessor::kSlots; ++s)
                for (auto* what : { "pitch", "reverse", "attack", "release", "level", "pan" })
                    all.push_back (param (PluginProcessor::slotId (s, what))->getValue());
            return all;
        }

        void proveEverything()
        {
            revealMode (PluginProcessor::modeTransient);
            proveCell ("sensitivity");
            proveCell ("slice_start");

            revealMode (PluginProcessor::modeGrid, PluginProcessor::gridEqual);
            proveMenu ("grid");
            proveCell ("slice_start");
            proveSegments ("grid_type");

            revealMode (PluginProcessor::modeGrid, PluginProcessor::gridNote);
            proveMenu ("grid_note");
            proveCell ("grid_tempo");

            revealMode (PluginProcessor::modeGrid, PluginProcessor::gridEqual);
            revealMode (PluginProcessor::modeTransient);
            proveSegments ("slice_mode");

            openAdvanced();
            for (auto* id : { "min_slice", "transpose", "bend_range" })
                proveCell (id);

            for (auto* id : { "output", "root_note", "polyphony", "vel_sens", "loop_fade", "decay", "sustain", "penv_amount", "penv_time" })
                proveCell (id);
            proveMenu ("play_mode");
            proveSegments ("key_mode");
            provePill ("bypass");

            proveSlots();

            // The three readings show what a host sees.
            for (auto* prm : p.getParameters())
                if (auto* reading = dynamic_cast<ReadingParameter*> (prm))
                {
                    auto* cell = findById<ui::ReadingCell> (window, reading->paramID);
                    window.refresh();
                    if (cell == nullptr || ! showsInWindow (*cell, window))
                        fail (reading->paramID + ": no reading on screen");
                    else if (! cell->getValueText().startsWith (reading->getCurrentValueAsText()))
                        fail (reading->paramID + ": shows " + cell->getValueText() + ", host sees " + reading->getCurrentValueAsText());
                    else
                        proved.insert (reading->paramID);
                }
        }
    };

    inline int countOverlaps (SliceEditor& window)
    {
        // Controls never sit on each other. Reverse sits in a gap in the stepper's row on purpose.
        int overlaps = 0;
        std::vector<juce::Component*> shown;
        for (auto* c : window.getChildren())
            if (c->isVisible() && c->getComponentID().isNotEmpty() && c->getBounds().getWidth() > 0)
                shown.push_back (c);
        for (size_t a = 0; a < shown.size(); ++a)
            for (size_t b = a + 1; b < shown.size(); ++b)
            {
                const auto& ia = shown[a]->getComponentID();
                const auto& ib = shown[b]->getComponentID();
                if ((ia == "selected" && ib.endsWith ("_reverse")) || (ib == "selected" && ia.endsWith ("_reverse")))
                    continue;
                if (shown[a]->getBounds().intersects (shown[b]->getBounds()))
                {
                    ++overlaps;
                    check (false, "controls overlap: " + ia.toStdString() + " and " + ib.toStdString());
                }
            }
        return overlaps;
    }
}

static void testWindow()
{
    using namespace windowtests;
    const double rate = 48000.0;
    const auto file = writeWav ("window.wav", { fingerprintAudio() }, rate);

    // The sound, bit for bit, against the 1.0.0 build, with and without the new window drawing all the way through.
    {
        int wrong = 0, wrongWithWindow = 0;
        for (const auto& f : before11)
        {
            if (outputFingerprint (file, f.preset, false) != f.sum) ++wrong;
            if (outputFingerprint (file, f.preset, true) != f.sum) ++wrongWithWindow;
        }
        check (wrong == 0, "null test: all 14 presets bit identical to 1.0.0, presets that differ", wrong);
        check (wrongWithWindow == 0, "null test: identical with this window open and drawing, presets that differ", wrongWithWindow);
    }

    // Every parameter can be reached from the window, by mouse, with no sample and with one.
    for (const bool withSample : { false, true })
    {
        PluginProcessor p;
        if (withSample)
            load (p, file);
        std::unique_ptr<SliceEditor> window (dynamic_cast<SliceEditor*> (p.createEditor()));
        window->setVisible (true);
        window->setSize (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        Prover prover { p, *window, {}, 0 };
        prover.proveEverything();

        int missing = 0;
        for (auto* prm : p.getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
                if (prover.proved.count (withId->paramID) == 0)
                {
                    ++missing;
                    check (false, "not reachable from the window: " + withId->paramID.toStdString());
                }

        const std::string tag = withSample ? " with a sample" : " with no sample";
        check (p.getParameters().size() == 218, "the plug-in still has 218 parameters", p.getParameters().size());
        check (prover.failuresHere == 0, "no control failed to reach its parameter" + tag, prover.failuresHere);
        check (missing == 0 && (int) prover.proved.size() == 218, "every parameter driven through the window at the smallest size" + tag + ", count", (double) prover.proved.size());
    }

    PluginProcessor p;
    load (p, file);
    toGrid (p, 4);   // 8 slices
    prepare (p, 2, rate, 256);
    std::unique_ptr<SliceEditor> window (dynamic_cast<SliceEditor*> (p.createEditor()));
    window->setVisible (true);
    window->refresh();

    // Name and words.
    check (SliceEditor::getWindowTitle() == "Cutup", "the window is titled Cutup");
    {
        std::vector<juce::Component*> all;
        collectAll (*window, all);
        int bad = 0, tooLong = 0;
        const auto hintFont = ui::font (ui::Weight::regular, 12.5f);
        window->setSize (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
        window->setAdvancedOpen (true);
        for (auto* c : all)
            if (auto* hinted = dynamic_cast<ui::Hinted*> (c))
            {
                if (hinted->hint.containsIgnoreCase ("smooth") || hinted->hint.contains (juce::String::fromUTF8 ("\xe2\x80\x94")) || hinted->hint.isEmpty())
                {
                    ++bad;
                    check (false, "hint has a bad word or is empty: " + c->getComponentID().toStdString());
                }
                if (juce::GlyphArrangement::getStringWidth (hintFont, hinted->hint) > (float) window->getHintRoom())
                {
                    ++tooLong;
                    check (false, "hint is cut off at the smallest size: " + hinted->hint.toStdString());
                }
            }
        check (bad == 0, "every hint is there, no old name, no em dash, bad", bad);
        check (tooLong == 0, "every hint fits at the smallest size, too long", tooLong);
    }

    // Nothing overlaps in any layout, at the smallest size and the default.
    for (const auto size : { juce::Point<int> (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight),
                             juce::Point<int> (PluginProcessor::defaultWindowWidth, PluginProcessor::defaultWindowHeight) })
    {
        window->setSize (size.x, size.y);
        int overlaps = 0;
        for (const bool open : { false, true })
        {
            window->setAdvancedOpen (open);
            for (const int mode : { 0, 1, 2 })
                for (const int kind : { 0, 1 })
                {
                    setP (p, "slice_mode", (float) mode);
                    setP (p, "grid_type", (float) kind);
                    window->refresh();
                    overlaps += countOverlaps (*window);
                }
        }
        check (overlaps == 0, "no controls overlap at " + std::to_string (size.x) + " by " + std::to_string (size.y) + ", overlaps", overlaps);
        check (window->getWaveform().getHeight() >= 120, "the waveform keeps its room with Advanced open at this size", window->getWaveform().getHeight(), "px");
    }
    window->setAdvancedOpen (false);
    toGrid (p, 4);
    window->refresh();

    // Automation moves the window, including controls that follow the selection.
    {
        setP (p, "selected", 5.0f);
        window->refresh();
        auto* cell = findById<ui::ValueCell> (*window, "s05_pitch");
        check (cell != nullptr, "selecting slice 5 from outside points Pitch at slot 5");
        setP (p, "s05_pitch", 3.0f);
        check (cell != nullptr && std::abs (cell->getValue() - 3.0) < 0.01 && cell->valueText() == "+3 st", "automating Slice 05 Pitch moves the cell: " + (cell != nullptr ? cell->valueText().toStdString() : ""));
        setP (p, "s05_pitch", 0.0f);

        // A value can be typed, in the words the window shows.
        auto* attack = findById<ui::ValueCell> (*window, "s05_attack");
        attack->applyTyped ("1.5 s");
        check (std::abs (p.apvts.getRawParameterValue ("s05_attack")->load() - 1500.0f) < 1.0f, "typing 1.5 s sets Attack to 1500 ms");
        attack->applyTyped ("nonsense");
        check (std::abs (p.apvts.getRawParameterValue ("s05_attack")->load() - 1500.0f) < 1.0f, "typing a word changes nothing");
        attack->applyTyped ("20 ms");
        check (std::abs (p.apvts.getRawParameterValue ("s05_attack")->load() - 20.0f) < 0.1f, "typing 20 ms sets Attack to 20 ms");
        auto* pan = findById<ui::ValueCell> (*window, "s05_pan");
        pan->applyTyped ("40 L");
        check (std::abs (p.apvts.getRawParameterValue ("s05_pan")->load() + 40.0f) < 0.1f, "typing 40 L sets Pan to -40");
        pan->applyTyped ("c");
        check (std::abs (p.apvts.getRawParameterValue ("s05_pan")->load()) < 0.1f, "typing c centers Pan");
        auto* rootCell = findById<ui::ValueCell> (*window, "root_note");
        rootCell->applyTyped ("D3");
        check ((int) p.apvts.getRawParameterValue ("root_note")->load() == 62, "typing D3 sets Root to its note", p.apvts.getRawParameterValue ("root_note")->load());
        setP (p, "root_note", 60.0f);
        setP (p, "s05_attack", 1.0f);
    }

    // The selection moving mid drag does not strand a gesture.
    {
        window->refresh();
        auto* cell = findById<ui::ValueCell> (*window, "s05_level");
        auto* prm = p.apvts.getParameter ("s05_level");
        GestureCount count;
        prm->addListener (&count);
        const auto at = cell->getLocalBounds().getCentre().toFloat();
        cell->mouseDown (mouseAt (*cell, at, at, false));
        setP (p, "selected", 9.0f);
        window->refresh();
        check (cell->getComponentID() == "s05_level", "a cell being dragged stays on its slot until let go");
        cell->mouseDrag (mouseAt (*cell, at + juce::Point<float> (-30.0f, 0.0f), at, true));
        cell->mouseUp (mouseAt (*cell, at + juce::Point<float> (-30.0f, 0.0f), at, true));
        prm->removeListener (&count);
        check (count.begins == 1 && count.ends == 1, "that drag begins and ends once on its own parameter");
        window->refresh();
        check (cell->getComponentID() == "s09_level", "then it follows the selection: " + cell->getComponentID().toStdString());
        setP (p, "s05_level", 0.0f);
        setP (p, "selected", 3.0f);
        window->refresh();
    }

    // The graphs' dots drive the same parameters, and Attack and Release follow the slice.
    {
        auto* amp = findById<AmpGraph> (*window, "amp_graph");
        auto* pitch = findById<PitchGraph> (*window, "pitch_graph");
        const auto rawOf = [&] (const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        amp->setBounds (0, 0, 300, 90);   // so the distances are known

        drag (*amp, amp->handlePosition (0), amp->handlePosition (0) + juce::Point<float> (40.0f, 0.0f));
        check (rawOf ("s03_attack") > 5.0f, "the first Amp dot moves the selected slice's Attack", rawOf ("s03_attack"), "ms");
        check (std::abs (rawOf ("s01_attack") - 1.0f) < 0.001f, "and no other slice's", rawOf ("s01_attack"), "ms");

        setP (p, "sustain", 70.0f);
        const float decayBefore = rawOf ("decay");
        drag (*amp, amp->handlePosition (1), amp->handlePosition (1) + juce::Point<float> (25.0f, 12.0f));
        check (rawOf ("sustain") < 69.0f && rawOf ("decay") > decayBefore, "the second Amp dot moves Decay and Sustain together");

        const float releaseBefore = rawOf ("s03_release");
        drag (*amp, amp->handlePosition (2), amp->handlePosition (2) + juce::Point<float> (-30.0f, 0.0f));
        check (rawOf ("s03_release") > releaseBefore, "the third Amp dot moves the selected slice's Release", rawOf ("s03_release"), "ms");

        setP (p, "selected", 8.0f);
        window->refresh();
        drag (*amp, amp->handlePosition (0), amp->handlePosition (0) + juce::Point<float> (30.0f, 0.0f));
        check (rawOf ("s08_attack") > 3.0f, "after selecting slice 8, the dot moves slice 8's Attack", rawOf ("s08_attack"), "ms");

        pitch->setBounds (0, 0, 200, 90);
        drag (*pitch, pitch->handlePosition (0), pitch->handlePosition (0) + juce::Point<float> (0.0f, -20.0f));
        check (rawOf ("penv_amount") > 5.0f, "the first Pitch dot sets how far", rawOf ("penv_amount"), "st");
        const float timeBefore = rawOf ("penv_time");
        drag (*pitch, pitch->handlePosition (1), pitch->handlePosition (1) + juce::Point<float> (40.0f, 0.0f));
        check (rawOf ("penv_time") > timeBefore, "the second Pitch dot sets how long", rawOf ("penv_time"), "ms");

        for (auto* id : { "sustain", "decay", "penv_amount", "penv_time", "s03_attack", "s03_release", "s08_attack" })
        {
            auto* prm = p.apvts.getParameter (id);
            prm->setValueNotifyingHost (prm->getDefaultValue());
        }
        window->resized();
    }

    // The hero: Start tab, Root tab, keys and slices all work.
    {
        auto& wave = window->getWaveform();
        auto& keys = window->getKeyMap();
        setP (p, "selected", 1.0f);
        setP (p, "slice_start", 0.0f);
        window->refresh();

        auto* startParam = p.apvts.getParameter ("slice_start");
        GestureCount count;
        startParam->addListener (&count);
        const auto tab = wave.startHandlePosition();
        drag (wave, tab, juce::Point<float> ((float) wave.getWidth() * 0.25f, tab.y));
        startParam->removeListener (&count);
        const double expected = (double) p.getSample()->frames() / rate * 250.0;
        check (std::abs (p.apvts.getRawParameterValue ("slice_start")->load() - expected) < expected * 0.05 + 2.0, "dragging the Start tab a quarter across sets Slice Start, ms", p.apvts.getRawParameterValue ("slice_start")->load());
        check (count.begins == 1 && count.ends == 1, "with one gesture");
        window->refresh();
        check (p.getShownTable().start[0] >= (int) (expected * 0.001 * rate) - 2, "and the slices start there");
        setP (p, "slice_start", 0.0f);
        window->refresh();

        drag (keys, keys.rootTabCentre(), keys.keyCentre (64));
        check ((int) p.apvts.getRawParameterValue ("root_note")->load() == 64, "dragging the Root tab onto a key sets Root Note", p.apvts.getRawParameterValue ("root_note")->load());
        setP (p, "root_note", 60.0f);
        window->refresh();

        prepare (p, 2, rate, 256);
        click (keys, keys.keyCentre (63));
        check (p.getSelectedIndex() == 3, "clicking the key of slice 4 selects it", p.getSelectedIndex() + 1);
        const auto heard = play (p, 2048, {});
        check (rms (heard.getReadPointer (0), 2048) > 1.0e-3 && p.getActiveVoices() >= 1, "and plays it", rms (heard.getReadPointer (0), 2048));
        window->refresh();
        check (keys.isLit (63), "and that key lights while it sounds");
        const auto silent = play (p, 48000, {});
        juce::ignoreUnused (silent);
        window->refresh();
        check (! keys.isLit (63), "and goes dark again");

        // A key with no slice on it does nothing.
        click (keys, keys.keyCentre (75));
        check (p.getSelectedIndex() == 3, "a key with no slice leaves the selection alone");

        // Zoom shows only the selected slice, and marker mapping follows it.
        const auto t = p.getShownTable();
        window->setZoomed (true);
        window->refresh();
        check (wave.getViewFrom() <= t.start[3] && wave.getViewTo() >= t.start[4] && wave.getViewTo() - wave.getViewFrom() < (t.start[4] - t.start[3]) * 2, "Zoom slice fills the view with the selected slice");
        check (std::abs (wave.frameAtX (wave.xForFrame (t.start[3] + 100)) - (t.start[3] + 100)) < 8, "and positions map back to the right frame");
        const auto state = [&] { juce::MemoryBlock m; p.getStateInformation (m); return m; };
        const auto zoomedState = state();
        window->setZoomed (false);
        window->refresh();
        check (zoomedState == state(), "Zoom adds nothing to the saved state");
    }

    // Drawing costs: a voice playing never rebuilds the cached layers.
    {
        auto& wave = window->getWaveform();
        window->refresh();
        (void) window->createComponentSnapshot (window->getLocalBounds());
        const int before = wave.getRebuildCount();
        prepare (p, 2, rate, 256);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buffer (2, 256);
        for (int b = 0; b < 200; ++b)
        {
            midi.clear();
            if (b % 25 == 0)
                midi.addEvent (on (60 + (b / 25) % 8), 0);
            buffer.clear();
            p.processBlock (buffer, midi);
            window->refresh();
            if (b % 4 == 0)
                (void) window->createComponentSnapshot (window->getLocalBounds());
        }
        check (wave.getRebuildCount() == before, "200 blocks of notes rebuild no cached layer, rebuilds", wave.getRebuildCount() - before);
        setP (p, "s02_level", -6.0f);
        window->refresh();
        (void) window->createComponentSnapshot (window->getLocalBounds());
        check (wave.getRebuildCount() == before + 1, "moving a level rebuilds it once", wave.getRebuildCount() - before);
        setP (p, "s02_level", 0.0f);
    }

    // Size and Advanced come back, damaged values stay in range, and nothing else was added.
    {
        window->setSize (1300, 900);
        window->setAdvancedOpen (true);
        juce::MemoryBlock saved1;
        p.getStateInformation (saved1);
        PluginProcessor q;
        q.setStateInformation (saved1.getData(), (int) saved1.getSize());
        q.waitForLoad (10000);
        check (q.windowWidth.load() == 1300 && q.windowHeight.load() == 900 && q.advancedOpen.load(), "window size and Advanced come back from the saved state");

        std::unique_ptr<SliceEditor> second (dynamic_cast<SliceEditor*> (q.createEditor()));
        check (second->getWidth() == 1300 && second->getHeight() == 900 && second->isAdvancedOpen(), "and a new window opens that way");
        second.reset();

        auto xml = juce::AudioProcessor::getXmlFromBinary (saved1.getData(), (int) juce::jmin ((size_t) 4 * 1024 * 1024, saved1.getSize()));
        check (xml != nullptr && xml->getNumAttributes() == 3, "the state's root holds only those three, attributes", xml != nullptr ? xml->getNumAttributes() : -1);
        if (xml != nullptr)
        {
            auto older = *xml;
            older.removeAttribute ("windowWidth");
            older.removeAttribute ("windowHeight");
            older.removeAttribute ("advancedOpen");
            juce::MemoryBlock olderState;
            juce::AudioProcessor::copyXmlToBinary (older, olderState);
            PluginProcessor o;
            o.setStateInformation (olderState.getData(), (int) olderState.getSize());
            check (o.windowWidth.load() == PluginProcessor::defaultWindowWidth && ! o.advancedOpen.load(), "a 1.0.0 session with none of them opens at the default size");

            xml->setAttribute ("windowWidth", 99999999);
            xml->setAttribute ("windowHeight", -5);
            juce::MemoryBlock damaged;
            juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
            PluginProcessor r;
            r.setStateInformation (damaged.getData(), (int) damaged.getSize());
            check (r.windowWidth.load() == PluginProcessor::maxWindowWidth && r.windowHeight.load() == PluginProcessor::minWindowHeight, "absurd saved sizes are clamped");
            xml->setAttribute ("windowWidth", "not a number");
            juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
            PluginProcessor r2;
            r2.setStateInformation (damaged.getData(), (int) damaged.getSize());
            check (r2.windowWidth.load() >= PluginProcessor::minWindowWidth && r2.windowWidth.load() <= PluginProcessor::maxWindowWidth, "a saved size that is not a number stays in range");
        }
        window->setSize (PluginProcessor::defaultWindowWidth, PluginProcessor::defaultWindowHeight);
        window->setAdvancedOpen (false);
    }

    // Loading through the window keeps the audio thread clean: no allocation in any block, and the note keeps sounding until the swap.
    {
        std::vector<float> longAudio;
        for (int i = 0; i < 40; ++i)
            for (auto v : fingerprintAudio())
                longAudio.push_back (v);
        const auto bigFile = writeWav ("window-big.wav", { longAudio }, rate);

        PluginProcessor q;
        load (q, file);
        toGrid (q, 4);
        setP (q, "play_mode", (float) PluginProcessor::playLoop);
        prepare (q, 2, rate, 256);
        std::unique_ptr<SliceEditor> w (dynamic_cast<SliceEditor*> (q.createEditor()));
        w->refresh();
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        midi.addEvent (on (61), 0);
        q.processBlock (buffer, midi);
        const auto* oldSample = q.getSample().get();

        w->filesDropped ({ bigFile.getFullPathName() }, 10, 10);

        long allocsInBlocks = 0;
        int blocks = 0, silentBlocks = 0;
        bool sawLoading = false;
        const auto started = juce::Time::getMillisecondCounterHiRes();
        while (q.getSample().get() == oldSample && juce::Time::getMillisecondCounterHiRes() - started < 30000.0)
        {
            midi.clear();
            buffer.clear();
            const long before = threadAllocs;   // this thread only: the background thread is allocating, as it should
            q.processBlock (buffer, midi);
            allocsInBlocks += threadAllocs - before;
            ++blocks;
            if (rms (buffer.getReadPointer (0), 256) < 1.0e-5)
                ++silentBlocks;
            w->refresh();
            sawLoading = sawLoading || w->getStatusText().startsWith ("Loading");
        }
        check (q.getSample().get() != oldSample, "the dropped file arrives");
        check (blocks > 10, "while it loaded, blocks ran", blocks);
        check (allocsInBlocks == 0, "no block allocated while the file loaded, allocations", (double) allocsInBlocks);
        check (silentBlocks == 0, "the held note kept sounding the whole time, silent blocks", silentBlocks);
        check (sawLoading, "and the window said so while it loaded");
        q.waitForLoad (30000);
        w->refresh();
        check (w->getStatusText().contains ("window-big.wav"), "then it names the new file: " + w->getStatusText().toStdString());
        w.reset();
    }

    // A file it cannot use says so in the window, and the sample stays.
    {
        const auto notAudio = tempDir().getChildFile ("broken.wav");
        notAudio.replaceWithText ("this is not audio");
        PluginProcessor q;
        load (q, file);
        std::unique_ptr<SliceEditor> w (dynamic_cast<SliceEditor*> (q.createEditor()));
        const auto* kept = q.getSample().get();
        w->filesDropped ({ notAudio.getFullPathName() }, 1, 1);
        q.waitForLoad (10000);
        w->refresh();
        check (w->getStatusText().contains ("Could not read") && q.getSample().get() == kept, "a broken file says so, and the sample stays: " + w->getStatusText().toStdString());
    }

    // Opening and closing, again and again, with a real audio thread running and with none, while a file loads.
    {
        PluginProcessor q;
        load (q, file);
        toGrid (q, 4);
        prepare (q, 2, rate, 256);
        std::atomic<bool> running { true };
        std::atomic<int> blocksRun { 0 };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            int b = 0;
            while (running.load())
            {
                midi.clear();
                if (++b % 20 == 0)
                    midi.addEvent (on (60 + b % 8), 0);
                buffer.clear();
                q.processBlock (buffer, midi);
                blocksRun.fetch_add (1);
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        });
        for (int i = 0; i < 60; ++i)
        {
            std::unique_ptr<SliceEditor> w (dynamic_cast<SliceEditor*> (q.createEditor()));
            w->setVisible (true);
            w->refresh();
            if (i % 3 == 0)
                (void) w->createComponentSnapshot (w->getLocalBounds());
            if (i == 20)
                q.loadFile (file);
            w->setZoomed (i % 2 == 0);
            w->refresh();
        }
        running = false;
        audio.join();
        q.waitForLoad (10000);
        check (blocksRun.load() > 0, "60 windows opened and closed while audio ran, blocks", blocksRun.load());

        for (int i = 0; i < 30; ++i)
        {
            std::unique_ptr<SliceEditor> w (dynamic_cast<SliceEditor*> (q.createEditor()));
            w->refresh();
        }
        check (true, "30 more with no audio");
    }
}
