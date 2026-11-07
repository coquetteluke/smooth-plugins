#pragma once
// The window, tested without a screen. Included by PluginTest.cpp, inside its namespace.
// Numbers from the 1.1.0 source, before the window or its level readings existed, by `PluginTest fingerprints` at commit 2af225a.

namespace windowtests
{
struct Fingerprint { int preset; std::uint64_t sum; };
constexpr Fingerprint before11[] = { { 0, 12819405965456412613ull }, { 3, 10956679219727345505ull }, { 4, 7588531360145783692ull },
                                     { 7, 10480141954853775759ull }, { 10, 17408058606749435550ull } };

inline void collectIds (juce::Component& c, std::set<juce::String>& ids)
{
    if (c.getComponentID().isNotEmpty())
        ids.insert (c.getComponentID());
    for (auto* child : c.getChildren())
        collectIds (*child, ids);
}

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

struct GestureCount final : juce::AudioProcessorParameter::Listener
{
    int begins = 0, ends = 0, changes = 0;
    void parameterValueChanged (int, float) override { ++changes; }
    void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
};

inline std::unique_ptr<HarmonyEditor> openWindow (PluginProcessor& p)
{
    std::unique_ptr<HarmonyEditor> e (dynamic_cast<HarmonyEditor*> (p.createEditor()));
    if (e != nullptr)
        e->setVisible (true);
    return e;
}

inline juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> at, int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, at, now, clicks, false);
}
}

void testWindow()
{
    using namespace windowtests;
    const juce::StringArray readings { "detectedPitch", "leadNote", "voice1Note", "voice2Note", "voice3Note", "voice4Note", "voicesSounding", "activeKey" };

    // The sound, bit for bit, against the build from before the window, with the window open and drawing all the way through.
    for (const auto& f : before11)
    {
        check (outputFingerprint (f.preset) == f.sum, "null test: output identical to 1.1.0, preset " + std::to_string (f.preset));

        check (outputFingerprintWithWindow (f.preset) == f.sum, "null test: identical with the window open and drawing, preset " + std::to_string (f.preset));
    }

    // Every control is on screen, and it is a real parameter
    {
        auto p = make();
        p->advancedOpen.store (true);
        auto e = openWindow (*p);
        check (e != nullptr, "the processor opens the Harmony window");
        std::set<juce::String> ids;
        collectIds (*e, ids);
        int missing = 0;
        for (auto* prm : p->getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
                if (! readings.contains (withId->paramID) && ids.count (withId->paramID) == 0)
                {
                    ++missing;
                    std::printf ("  no control for %s\n", withId->paramID.toRawUTF8());
                }
        check (missing == 0, "every parameter has a control in the window", (double) missing, "missing");

        int notParameters = 0;
        for (const auto& id : ids)
            if (id != "presets" && id != "advanced" && p->apvts.getParameter (id) == nullptr)
                ++notParameters;
        check (notParameters == 0, "no control exists only in the window", (double) notParameters, "window only");
    }

    // Every Mode: controls that swap places are all visible with Advanced open, and half of them with it closed. Nothing overlaps.
    for (const bool open : { false, true })
    {
        auto p = make();
        p->advancedOpen.store (open);
        auto e = openWindow (*p);
        auto& panel = e->getPanel();
        int overlaps = 0, outside = 0;

        for (int mode = 0; mode < numModes; ++mode)
        {
            setParam (*p, "mode", (float) mode);
            e->tick (1.0 / 30.0);
            juce::Array<juce::Component*> shown;
            for (auto* c : panel.getChildren())
                if (c->isVisible())
                    shown.add (c);
            for (int a = 0; a < shown.size(); ++a)
            {
                if (! panel.getLocalBounds().contains (shown[a]->getBounds()))
                    ++outside;
                for (int b = a + 1; b < shown.size(); ++b)
                    if (shown[a]->getBounds().intersects (shown[b]->getBounds()))
                    {
                        ++overlaps;
                        std::printf ("  overlap: %s and %s\n", shown[a]->getComponentID().toRawUTF8(), shown[b]->getComponentID().toRawUTF8());
                    }
            }
            if (open)
                for (const char* id : { "key", "scale", "followKey", "chordRoot", "chordType", "chordHold" })
                    if (findById<juce::Component> (panel, id) == nullptr || ! findById<juce::Component> (panel, id)->isVisible())
                        ++outside;
        }
        check (overlaps == 0, std::string ("nothing overlaps in any Mode, Advanced ") + (open ? "open" : "closed"), (double) overlaps, "overlaps");
        check (outside == 0, std::string ("everything fits and is reachable, Advanced ") + (open ? "open" : "closed"), (double) outside, "misplaced");
    }

    // Automation moves the window, and the window moves the parameter with a gesture
    {
        auto p = make();
        auto e = openWindow (*p);

        setParam (*p, "voice1Pan", 42.0f);
        auto* pan = findById<ui::BarSlider> (*e, "voice1Pan");
        check (pan != nullptr && std::abs (pan->getValue() - 42.0) < 0.01, "automation moves the Pan control");
        check (pan->valueText() == "42 R", "Pan shows its own value string, " + pan->valueText().toStdString());

        setParam (*p, "voice1Interval", 7.0f);
        check (std::abs (findById<ui::IntervalField> (*e, "voice1Interval")->getValue() - 7.0) < 0.01, "automation moves the Interval control");

        GestureCount watch;
        auto* level = p->apvts.getParameter ("voice2Level");
        level->addListener (&watch);
        auto* levelBar = findById<ui::BarSlider> (*e, "voice2Level");
        levelBar->applyTyped ("-12");
        check (std::abs (level->convertFrom0to1 (level->getValue()) + 12.0f) < 0.01f, "typing a value sets the parameter");
        levelBar->applyTyped ("Off");
        check (level->convertFrom0to1 (level->getValue()) <= -60.0f, "typing Off turns the voice off");
        check (levelBar->valueText() == "Off", "and it reads Off");
        levelBar->applyTyped ("nonsense");
        check (level->convertFrom0to1 (level->getValue()) <= -60.0f, "typing nonsense changes nothing");
        check (watch.begins == 2 && watch.ends == 2, "typing begins and ends a gesture each time", (double) watch.begins, "gestures");
        check (std::abs (levelBar->getDoubleClickReturnValue() + 6.0) < 0.01, "double-click on Level returns to its default");
        level->removeListener (&watch);

        auto* typedPan = findById<ui::BarSlider> (*e, "voice3Pan");
        typedPan->applyTyped ("35 L");
        check (std::abs (typedPan->getValue() + 35.0) < 0.01, "typing 35 L pans left");
        typedPan->applyTyped ("c");
        check (std::abs (typedPan->getValue()) < 0.01, "typing c centres it");

        // The interval's arrows skip names in the key, and step by one otherwise.
        setParam (*p, "mode", (float) modeScale);
        e->tick (1.0 / 30.0);
        setParam (*p, "voice1Interval", 4.0f);
        auto* interval = findById<ui::IntervalField> (*e, "voice1Interval");
        interval->step (1);
        check (juce::roundToInt (interval->getValue()) == 5, "in key, the up arrow from a 3rd goes to a 4th");
        interval->step (-1);
        check (juce::roundToInt (interval->getValue()) == 4, "and back down lands on a 3rd");
        interval->step (-1);
        check (juce::roundToInt (interval->getValue()) == 2, "and down again to a 2nd, not to another 3rd");
        setParam (*p, "mode", (float) modeFixed);
        e->tick (1.0 / 30.0);
        interval->step (1);
        check (juce::roundToInt (interval->getValue()) == 3, "in Fixed, the arrow moves one semitone");
        setParam (*p, "mode", (float) modeScale);
        e->tick (1.0 / 30.0);

        // Dragging a singer on the stage moves pan and interval through real gestures, and a double-click puts them back.
        auto& stage = e->getStage();
        setParam (*p, "voice1Interval", 4.0f);
        setParam (*p, "voice1Pan", -35.0f);
        for (int i = 0; i < 40; ++i)
            e->tick (1.0 / 30.0);
        GestureCount panWatch, intervalWatch;
        auto* panP = p->apvts.getParameter ("voice1Pan");
        auto* intervalP = p->apvts.getParameter ("voice1Interval");
        panP->addListener (&panWatch);
        intervalP->addListener (&intervalWatch);
        const auto from = stage.singerPosition (0);
        const auto to = from + juce::Point<float> (60.0f, -3.0f * stage.semitonePixels());
        stage.mouseDown (mouseAt (stage, from));
        stage.mouseDrag (mouseAt (stage, to));
        stage.mouseUp (mouseAt (stage, to));
        check (panWatch.begins == 1 && panWatch.ends == 1 && intervalWatch.begins == 1 && intervalWatch.ends == 1, "dragging a singer is one gesture on pan and one on interval");
        check (std::abs (intervalP->convertFrom0to1 (intervalP->getValue()) - 7.0f) < 0.01f, "dragging up three rows adds three semitones");
        check (panP->convertFrom0to1 (panP->getValue()) > -35.0f + 20.0f, "dragging right pans right", panP->convertFrom0to1 (panP->getValue()), "pan");
        panP->removeListener (&panWatch);
        intervalP->removeListener (&intervalWatch);

        // In a MIDI mode a drag only pans.
        setParam (*p, "mode", (float) modeMidi);
        for (int i = 0; i < 40; ++i)
            e->tick (1.0 / 30.0);
        const float intervalBefore = intervalP->getValue();
        const auto midiFrom = stage.singerPosition (0);
        stage.mouseDown (mouseAt (stage, midiFrom));
        stage.mouseDrag (mouseAt (stage, midiFrom + juce::Point<float> (0.0f, -40.0f)));
        stage.mouseUp (mouseAt (stage, midiFrom));
        check (intervalP->getValue() == intervalBefore, "with MIDI deciding the notes, a drag leaves the interval alone");
        setParam (*p, "mode", (float) modeScale);
        for (int i = 0; i < 40; ++i)
            e->tick (1.0 / 30.0);
        stage.mouseDoubleClick (mouseAt (stage, stage.singerPosition (0), 2));
        check (std::abs (panP->getValue() - panP->getDefaultValue()) < 0.001f && std::abs (intervalP->getValue() - intervalP->getDefaultValue()) < 0.001f,
               "double-clicking a singer resets its pan and interval");

        check (e->getWidth() == PluginProcessor::defaultWindowWidth, "the window opens at its default size", (double) e->getWidth(), "wide");
    }

    // Reading the audio writes no automation
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        auto e = openWindow (*p);
        GestureCount watch;
        for (auto* prm : p->getParameters())
            prm->addListener (&watch);
        const auto x = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        for (size_t at = 0; at + 256 <= x.size(); at += 256)
        {
            for (int ch = 0; ch < 2; ++ch)
                std::copy_n (x.data() + at, 256, buf.getWritePointer (ch));
            p->processBlock (buf, midi);
            e->tick (1.0 / 180.0);
        }
        for (auto* prm : p->getParameters())
            prm->removeListener (&watch);
        check (watch.changes == 0 && watch.begins == 0, "a window watching audio writes no automation", (double) watch.changes, "writes");
        check (e->getStage().isLive(), "the singers follow a voice that is singing");
        const auto note = e->getStage().noteFor (StageView::leadIndex);
        check (note == "A3", "the lead reads the note it is on, " + note.toStdString());
        // A3 in C major, a 3rd up from defaults is C4, and a 5th up is E4.
        check (e->getStage().noteFor (0) == "C4", "voice 1 reads the note it sings, " + e->getStage().noteFor (0).toStdString());
        check (e->getStage().noteFor (1) == "E4", "voice 2 reads the note it sings, " + e->getStage().noteFor (1).toStdString());
    }

    // Open and shut, repeatedly, with no audio and then with audio running on its own thread
    {
        auto p = make();
        for (int i = 0; i < 60; ++i)
        {
            auto e = openWindow (*p);
            e->tick (1.0 / 30.0);
        }
        check (true, "the window opens and shuts 60 times with no audio");
    }
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        std::atomic<bool> running { true };
        std::atomic<long long> blocks { 0 };
        const auto x = voice::synth (voice::steady (220.0, 2.0, SR), voice::vowelA, SR);
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer midi;
            size_t at = 0;
            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        buf.setSample (ch, i, x[(at + (size_t) i) % x.size()]);
                at += 256;
                p->processBlock (buf, midi);
                ++blocks;
            }
        });

        juce::Image canvas (juce::Image::ARGB, 2400, 1560, true);
        for (int i = 0; i < 60; ++i)
        {
            auto e = openWindow (*p);
            for (int f = 0; f < 3; ++f)
                e->tick (1.0 / 30.0);
            juce::Graphics g (canvas);
            g.addTransform (juce::AffineTransform::scale (i % 2 == 0 ? 2.0f : 1.0f));
            e->paintEntireComponent (g, true);
        }
        running = false;
        audio.join();
        check (blocks.load() > 0, "the window opens, draws and shuts 60 times with audio running", (double) blocks.load(), "blocks");
    }

    // The display survives anything the parameters can be
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        auto e = openWindow (*p);
        const auto x = voice::synth (voice::phrase ({ 220.0, 262.0, 330.0, 196.0 }, 0.3, SR), voice::vowelA, SR);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        juce::Image canvas (juce::Image::ARGB, 1200, 780, true);
        int shots = 0;
        for (int preset = 0; preset < p->getNumPrograms(); ++preset)
            for (const int key : { 0, 11 })
                for (const int scale : { 0, 1, 5 })
                {
                    p->setCurrentProgram (preset);
                    setParam (*p, "key", (float) key);
                    setParam (*p, "scale", (float) scale);
                    setParam (*p, "voice1Interval", preset % 2 == 0 ? 24.0f : -24.0f);
                    for (size_t at = 0; at + 256 <= x.size() && at < 256 * 40; at += 256)
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            std::copy_n (x.data() + at, 256, buf.getWritePointer (ch));
                        p->processBlock (buf, midi);
                    }
                    e->tick (1.0 / 30.0);
                    juce::Graphics g (canvas);
                    e->paintEntireComponent (g, true);
                    ++shots;
                }
        check (shots > 0, "every preset, key extreme and scale draws", (double) shots, "pictures");
    }

    // What the window adds to a session: its size, whether Advanced is open, and nothing else
    {
        auto p = make();
        p->windowWidth.store (1500);
        p->advancedOpen.store (true);
        juce::MemoryBlock saved;
        p->getStateInformation (saved);
        auto q = make();
        q->setStateInformation (saved.getData(), (int) saved.getSize());
        check (q->windowWidth.load() == 1500 && q->advancedOpen.load(), "window size and Advanced come back with the session");
        auto e = openWindow (*q);
        check (e->getWidth() == 1500 && e->isAdvancedOpen(), "and the window opens that way", (double) e->getWidth(), "wide");
        check (std::abs ((double) e->getWidth() / e->getHeight() - (double) HarmonyEditor::designWidth / HarmonyEditor::designHeight) < 0.005, "the window keeps its shape");
        e.reset();

        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        int extras = 0;
        for (int i = 0; i < xml->getNumAttributes(); ++i)
            if (xml->getAttributeName (i) != "windowWidth" && xml->getAttributeName (i) != "advancedOpen")
                ++extras;
        check (extras == 0, "nothing else new in saved state", (double) extras, "attributes");
        check (q->apvts.state.getProperty ("windowWidth").isVoid(), "and the window's size stays out of the parameter tree");

        xml->setAttribute ("windowWidth", 999999);
        juce::MemoryBlock damaged;
        juce::AudioProcessor::copyXmlToBinary (*xml, damaged);
        auto r = make();
        r->setStateInformation (damaged.getData(), (int) damaged.getSize());
        check (r->windowWidth.load() == PluginProcessor::maxWindowWidth, "a damaged window size goes back in range", (double) r->windowWidth.load(), "wide");

        xml->removeAttribute ("windowWidth");
        xml->removeAttribute ("advancedOpen");
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        auto o = make();
        o->setStateInformation (old.getData(), (int) old.getSize());
        check (o->windowWidth.load() == PluginProcessor::defaultWindowWidth && ! o->advancedOpen.load(), "a session from before the window opens at the default size");
    }

    // Wording: no em dash anywhere the window can show one.
    {
        auto p = make();
        auto e = openWindow (*p);
        std::function<int (juce::Component&)> scan = [&] (juce::Component& c)
        {
            int found = 0;
            if (auto* h = dynamic_cast<ui::Hinted*> (&c))
                if (h->hint.containsChar ((juce::juce_wchar) 0x2014))
                    ++found;
            for (auto* child : c.getChildren())
                found += scan (*child);
            return found;
        };
        check (scan (*e) == 0, "no em dash in any hint");
    }
}
