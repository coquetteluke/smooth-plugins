#pragma once
// The window, tested without a screen. Included by PluginTest.cpp.
#include "editor/DuoEditor.h"
#include <set>

namespace windowtests
{
    // From `PluginTest fingerprints` on the 0.2.0 source at commit 8f9ef4a, with src/ untouched, before the window existed. Same order as nulltest::cases.
    constexpr std::uint64_t before[] = {
        6811122550820723901ull,  // as first opened
        1691295039992846014ull,  // Natural Pair
        1971145277203352354ull,  // Tight Double
        5074139018286875273ull,  // Quiet Thickener
        15686075276666239666ull, // Wide Stack
        16104445148038349082ull, // Loose Gang
        15670168807560303474ull, // Doubles Only Wide
        11120569788700014983ull, // Two Singers
        10793454012161618210ull, // Slapback Double
        4820892812069055583ull,  // mono in, stereo out
        13064547663857801966ull, // mono in, mono out
        5159340724599114457ull,  // 44.1 kHz, small blocks
        12794902507484840028ull, // 96 kHz, big blocks
        11571380111130561769ull, // every control moving
    };

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

    inline std::unique_ptr<DuoEditor> openWindow (PluginProcessor& p)
    {
        std::unique_ptr<DuoEditor> e (dynamic_cast<DuoEditor*> (p.createEditor()));
        if (e != nullptr)
            e->setVisible (true);
        return e;
    }

    inline juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> from, juce::Point<float> to, int clicks = 1, bool dragged = false)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), to, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, from, now, clicks, dragged);
    }

    /** A drag from one point to another, in the component's own coordinates, through its real mouse handlers. */
    inline void drag (juce::Component& c, juce::Point<float> from, juce::Point<float> to)
    {
        c.mouseDown (mouseEvent (c, from, from));
        for (int i = 1; i <= 4; ++i)
            c.mouseDrag (mouseEvent (c, from, from + (to - from) * (float) i / 4.0f, 1, true));
        c.mouseUp (mouseEvent (c, from, to, 1, true));
    }

    inline float value (PluginProcessor& p, const char* id)  { return p.apvts.getRawParameterValue (id)->load(); }

    /** Plays silence or voice for a while, ticking the display as the screen would. */
    inline void playFor (PluginProcessor& p, DuoEditor& e, const std::vector<float>& voice, double seconds, const std::function<void()>& eachFrame = {})
    {
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer midi;
        const int frames = (int) (seconds * 30.0);
        size_t at = 0;

        for (int f = 0; f < frames; ++f)
        {
            for (int b = 0; b < 6; ++b, at += 256)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        buffer.setSample (ch, i, voice.empty() ? 0.0f : voice[(at + (size_t) i) % voice.size()]);
                p.processBlock (buffer, midi);
            }
            e.tick (1.0 / 30.0);
            if (eachFrame)
                eachFrame();
        }
    }
}

static void testWindow()
{
    using namespace windowtests;
    using ui::StageView;

    const auto voice = nulltest::loadCorpus ("sung_goodnews");
    check (voice.size() > 100000, "the sung recording loads");

    // The sound, bit for bit, against the source from before the window, first with no window, then with it open and drawing all the way through.
    int index = 0;
    for (const auto& c : nulltest::cases)
    {
        {
            PluginProcessor p;
            check (nulltest::hashOf (p, c, [] (int) {}) == before[index], std::string ("null test: bit for bit the same as 0.2.0, no window, ") + c.name);
        }
        {
            PluginProcessor p;
            std::unique_ptr<DuoEditor> window;
            const auto sum = nulltest::hashOf (p, c, [&] (int block)
            {
                if (window == nullptr)
                    window = openWindow (p);
                if (block % 6 == 0)
                {
                    window->tick (1.0 / 30.0);
                    if (block % 96 == 0)
                        (void) window->createComponentSnapshot (window->getLocalBounds(), true, 2.0f);
                }
            });
            check (sum == before[index], std::string ("null test: bit for bit the same with the window open and drawing, ") + c.name);
        }
        ++index;
    }

    // Every control is on screen, and it is a real parameter
    {
        PluginProcessor p;
        p.advancedOpen.store (true);
        auto e = openWindow (p);
        check (e != nullptr, "the processor opens the Duo window");
        std::set<juce::String> ids;
        collectIds (*e, ids);
        for (auto* prm : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (prm))
                check (ids.count (ranged->getParameterID()) == 1, "on screen: " + ranged->getName (32).toStdString());
        for (const auto& id : ids)
            if (id != "presets" && id != "advanced" && id != "stage")
                check (p.apvts.getParameter (id) != nullptr, "control is a real parameter: " + id.toStdString());
        check (p.getName() == "Duo", "the build name is unchanged until the rename");
    }

    // Each control and the window move each other
    {
        PluginProcessor p;
        p.advancedOpen.store (true);
        auto e = openWindow (p);
        for (const auto* id : { "width", "tightness", "lead", "double1", "double2", "double3", "timingamount", "pitchamount", "vibrato", "timbre", "leadpan", "doublespan", "extradelay" })
        {
            auto* control = findById<ui::ParamControl> (e->getPanel(), id);
            check (control != nullptr, std::string ("found control ") + id);
            if (control == nullptr) continue;
            auto* prm = p.apvts.getParameter (id);
            prm->setValueNotifyingHost (0.25f);
            check (std::abs (control->getProportion() - prm->getValue()) < 0.01f, std::string ("automation moves the control: ") + id);
            GestureCount gestures;
            prm->addListener (&gestures);
            const auto middle = control->getLocalBounds().getCentre().toFloat();
            drag (*control, middle, middle + juce::Point<float> (31.0f, -27.0f));
            check (gestures.begins == 1 && gestures.ends == 1, std::string ("one begin and one end gesture on a drag: ") + id);
            check (gestures.changes > 0, std::string ("a drag moves the parameter: ") + id);
            const float def = prm->convertFrom0to1 (prm->getDefaultValue());
            control->mouseDoubleClick (mouseEvent (*control, middle, middle, 2));
            check (std::abs (value (p, id) - def) < 0.11f, std::string ("double-click resets: ") + id);
            prm->removeListener (&gestures);
        }

        // Typing, in what the window itself shows.
        auto* level = findById<ui::ParamControl> (e->getPanel(), "double1");
        level->applyTyped ("-12.5");
        check (std::abs (value (p, "double1") + 12.5f) < 0.06f, "typing a level", value (p, "double1"), "dB");
        level->applyTyped ("off");
        check (value (p, "double1") <= DuoEngine::offDb, "typing off");
        auto* pan = findById<ui::ParamControl> (e->getPanel(), "leadpan");
        pan->applyTyped ("35 L");
        check (std::abs (value (p, "leadpan") + 35.0f) < 0.06f && pan->valueText() == "35 L", "typing 35 L, and it reads back the same");
        pan->applyTyped ("rubbish");
        check (std::abs (value (p, "leadpan") + 35.0f) < 0.06f, "typing nonsense changes nothing");
        auto* width = findById<ui::ParamControl> (e->getPanel(), "width");
        width->applyTyped ("500");
        check (value (p, "width") == 100.0f, "typing past the end stops at the end");

        // Doubles and Bypass
        auto* count = findById<ui::CountPicker> (e->getPanel(), "doubles");
        count->choose (3);
        check (juce::roundToInt (value (p, "doubles")) == 3, "the Doubles buttons set Doubles");
        auto* bypass = findById<juce::ToggleButton> (e->getPanel(), "bypass");
        bypass->setToggleState (true, juce::sendNotificationSync);
        check (value (p, "bypass") >= 0.5f, "the Bypass button sets Bypass");
        p.apvts.getParameter ("range")->setValueNotifyingHost (1.0f);
        check (findById<ui::ChoiceField> (e->getPanel(), "range")->getSelected() == 3, "automation moves Vocal Range");
    }

    // The stage shows what the settings say
    {
        PluginProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 256);
        p.prepareToPlay (48000.0, 256);
        auto e = openWindow (p);
        auto& stage = e->getStage();
        const auto set = [&p] (const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };

        const auto settle = [&] { for (int i = 0; i < 60; ++i) e->tick (1.0 / 30.0); };

        // Width
        set ("doubles", 2.0f);
        for (const float w : { 0.0f, 25.0f, 70.0f, 100.0f })
        {
            set ("width", w);
            settle();
            check (std::abs (stage.singerAt (0).x - stage.xForPan (-w * 0.01f)) < 0.6f && std::abs (stage.singerAt (1).x - stage.xForPan (w * 0.01f)) < 0.6f,
                   "the pair stand at Width " + std::to_string ((int) w) + ", mirrored");
        }

        // The Width handles sit under the singers.
        {
            auto* control = findById<ui::ParamControl> (e->getPanel(), "width");
            const float atStage = stage.getX() + stage.xForPan (1.0f);
            set ("width", 100.0f);
            const auto rightEnd = juce::Point<float> (atStage - (float) control->getX(), (float) ui::WidthControl::trackY);
            control->mouseDown (mouseEvent (*control, rightEnd, rightEnd));
            control->mouseUp (mouseEvent (*control, rightEnd, rightEnd));
            check (std::abs (value (p, "width") - 100.0f) < 0.5f, "a click under hard right on the stage is Width 100", value (p, "width"), "%");
        }

        // Tightness: the lane grows, and the marks move back
        set ("width", 70.0f);
        float lastLane = 0.0f, lastMark = 1.0e6f;
        for (const float t : { 0.0f, 30.0f, 60.0f, 100.0f })
        {
            set ("tightness", t);
            settle();
            check (stage.zoneHalfHeight (0) > lastLane, "the lane grows with Tightness at " + std::to_string ((int) t), stage.zoneHalfHeight (0), "px");
            check (stage.markY (0) < lastMark, "the mark moves back with Tightness at " + std::to_string ((int) t));
            lastLane = stage.zoneHalfHeight (0);
            lastMark = stage.markY (0);
        }
        set ("tightness", 30.0f);

        // Extra Delay moves them back, on the stage, and it all still fits
        set ("extradelay", 100.0f);
        set ("tightness", 100.0f);
        settle();
        check (stage.markY (1) > 0.0f, "the furthest setting is still on the stage", stage.markY (1), "px from the top");
        set ("extradelay", 0.0f);
        set ("tightness", 30.0f);

        // Lead Pan and Doubles Pan
        set ("leadpan", -40.0f);
        settle();
        check (std::abs (stage.singerAt (StageView::leadIndex).x - stage.xForPan (-0.4f)) < 0.6f, "the lead stands at Lead Pan");
        set ("leadpan", 0.0f);
        set ("doublespan", 20.0f);
        set ("width", 50.0f);
        settle();
        check (std::abs (stage.singerAt (1).x - stage.xForPan (0.7f)) < 0.6f, "Doubles Pan moves the pair together");
        set ("doublespan", 0.0f);

        // Doubles count
        set ("doubles", 1.0f);
        settle();
        check (stage.isOnStage (0) && ! stage.isOnStage (1) && ! stage.isOnStage (2), "one double on stage");
        set ("doubles", 3.0f);
        settle();
        check (stage.isOnStage (2), "three doubles on stage");
        set ("doubles", 2.0f);
    }

    // The stage follows the real delay, and the lanes hold what the sound does
    for (const int preset : { 0, 3, 4, 7 })
    {
        PluginProcessor p;
        p.setCurrentProgram (preset);
        p.setPlayConfigDetails (2, 2, 48000.0, 256);
        p.prepareToPlay (48000.0, 256);
        auto e = openWindow (p);
        auto& stage = e->getStage();
        playFor (p, *e, voice, 3.0);
        double worstTrack = 0.0, outOfLane = 0.0, frames = 0.0;
        const int count = juce::roundToInt (value (p, "doubles"));
        playFor (p, *e, voice, 40.0, [&]
        {
            for (int v = 0; v < count; ++v)
            {
                const auto at = stage.singerAt (v);
                worstTrack = juce::jmax (worstTrack, (double) std::abs (at.y - stage.yForMs ((double) p.getDelayMs (v), at.x)));
                outOfLane += std::abs (at.y - stage.markY (v)) > stage.zoneHalfHeight (v) ? 1.0 : 0.0;
                frames += 1.0;
            }
        });
        const auto name = p.getProgramName (preset).toStdString();
        check (stage.isLive(), "live while the voice plays, " + name);
        check (worstTrack < 12.0, "the singer stays on its real delay, worst, " + name, worstTrack, "px");
        check (outOfLane / frames < 0.02, "share of time a singer is outside its lane, " + name, outOfLane / frames * 100.0, "%");
        const auto tracked = e->getReadoutText (0);
        check (tracked.endsWith (" ms"), "the readout shows the delay: " + tracked.toStdString());
    }

    // Silence: everyone walks back and the display stops
    {
        PluginProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 256);
        p.prepareToPlay (48000.0, 256);
        auto e = openWindow (p);
        auto& stage = e->getStage();
        playFor (p, *e, voice, 2.0);
        check (stage.isMoving(), "moving while the voice plays");
        playFor (p, *e, {}, 3.0);
        check (! stage.isLive() && ! stage.isMoving(), "nothing moving after silence, with audio still running");
        check (std::abs (stage.singerAt (0).y - stage.markY (0)) < 0.5f, "a double is back on its mark in silence");
    }

    // Dragging singers on the stage
    {
        PluginProcessor p;
        auto e = openWindow (p);
        auto& stage = e->getStage();
        const auto settle = [&] { for (int i = 0; i < 60; ++i) e->tick (1.0 / 30.0); };
        settle();
        GestureCount widthG, looseG, panG;
        p.apvts.getParameter ("width")->addListener (&widthG);
        p.apvts.getParameter ("tightness")->addListener (&looseG);
        p.apvts.getParameter ("leadpan")->addListener (&panG);
        const float width0 = value (p, "width"), loose0 = value (p, "tightness");

        auto from = stage.singerAt (1);
        drag (stage, from, from + juce::Point<float> (40.0f, 6.0f));
        check (value (p, "width") > width0 + 5.0f, "dragging a double outward widens", value (p, "width"), "%");
        check (value (p, "tightness") == loose0, "a sideways drag leaves Tightness alone");
        check (widthG.begins == 1 && widthG.ends == 1 && looseG.begins == 0, "that drag is one Width gesture and nothing else");
        settle();

        const float width1 = value (p, "width");
        from = stage.singerAt (0);
        drag (stage, from, from + juce::Point<float> (5.0f, -60.0f));
        check (value (p, "tightness") > loose0 + 5.0f, "dragging a double back loosens", value (p, "tightness"), "%");
        check (value (p, "width") == width1, "a drag back leaves Width alone");
        check (looseG.begins == 1 && looseG.ends == 1, "that drag is one Tightness gesture");
        settle();

        from = stage.singerAt (StageView::leadIndex);
        drag (stage, from, from + juce::Point<float> (-60.0f, 40.0f));
        check (value (p, "leadpan") < -5.0f, "dragging the lead moves Lead Pan", value (p, "leadpan"), "%");
        check (panG.begins == 1 && panG.ends == 1, "that drag is one Lead Pan gesture");
        settle();

        from = stage.singerAt (StageView::leadIndex);
        stage.mouseDoubleClick (mouseEvent (stage, from, from, 2));
        check (std::abs (value (p, "leadpan")) < 0.05f, "double-click on the lead centres it", value (p, "leadpan"), "%");
        from = stage.singerAt (1);
        stage.mouseDoubleClick (mouseEvent (stage, from, from, 2));
        check (std::abs (value (p, "width") - p.apvts.getParameter ("width")->convertFrom0to1 (p.apvts.getParameter ("width")->getDefaultValue())) < 0.1f
                   && std::abs (value (p, "tightness") - p.apvts.getParameter ("tightness")->convertFrom0to1 (p.apvts.getParameter ("tightness")->getDefaultValue())) < 0.1f,
               "double-click on a double resets Width and Tightness");

        // A click on empty floor moves nothing.
        const float w = value (p, "width");
        const juce::Point<float> empty (10.0f, 120.0f);
        drag (stage, empty, empty + juce::Point<float> (50.0f, 30.0f));
        check (value (p, "width") == w, "dragging empty floor does nothing");

        p.apvts.getParameter ("width")->removeListener (&widthG);
        p.apvts.getParameter ("tightness")->removeListener (&looseG);
        p.apvts.getParameter ("leadpan")->removeListener (&panG);

        // Hints
        const auto hovered = stage.singerAt (1);
        stage.mouseMove (mouseEvent (stage, hovered, hovered));
        check (stage.hint.contains ("Double 2") && stage.hint.contains ("Width"), "the hint names the singer under the mouse");
    }

    // Mono: no left or right to stand in
    {
        PluginProcessor p;
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (juce::AudioChannelSet::mono());
        layout.outputBuses.add (juce::AudioChannelSet::mono());
        check (p.setBusesLayout (layout), "mono layout accepted");
        p.prepareToPlay (48000.0, 256);
        auto e = openWindow (p);
        for (int i = 0; i < 60; ++i)
            e->tick (1.0 / 30.0);
        check (std::abs (e->getStage().singerAt (0).x - e->getStage().xForPan (0.0f)) < 0.6f, "on a mono track everyone stands in the middle");
    }

    // Saved state: window size and open section, nothing else new
    {
        PluginProcessor p;
        p.windowWidth.store (1400);
        {
            auto e = openWindow (p);
            check (e->getWidth() == 1400, "the window opens at the saved size");
            e->setAdvancedOpen (true, false);
            check (p.advancedOpen.load(), "opening Advanced is remembered");
            check (e->getAdvancedAmount() == 1.0f, "Advanced is open");
            e->setSize (1000, 610);
            check (p.windowWidth.load() == 1000, "resizing is remembered");
        }
        juce::MemoryBlock state;
        p.getStateInformation (state);
        PluginProcessor q;
        q.setStateInformation (state.getData(), (int) state.getSize());
        check (q.windowWidth.load() == 1000 && q.advancedOpen.load(), "window size and Advanced come back with the session");
        check (! q.apvts.state.hasProperty ("windowWidth") && ! q.apvts.state.hasProperty ("advancedOpen"), "they stay out of the sound's value tree");

        // Older sessions and damaged sizes.
        auto xml = p.apvts.copyState().createXml();
        PluginProcessor oldSession;
        juce::MemoryBlock oldState;
        juce::AudioProcessor::copyXmlToBinary (*xml, oldState);
        oldSession.setStateInformation (oldState.getData(), (int) oldState.getSize());
        check (oldSession.windowWidth.load() == PluginProcessor::defaultWindowWidth && ! oldSession.advancedOpen.load(), "a 0.2.0 session opens with the default window");
        xml->setAttribute ("windowWidth", 999999);
        juce::MemoryBlock bad;
        juce::AudioProcessor::copyXmlToBinary (*xml, bad);
        PluginProcessor damaged;
        damaged.setStateInformation (bad.getData(), (int) bad.getSize());
        check (damaged.windowWidth.load() == PluginProcessor::maxWindowWidth, "a damaged size goes back in range");
    }

    // Opening and closing, with audio running on another thread and without. Every picture gets drawn, at both pixel densities.
    {
        PluginProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 256);
        p.prepareToPlay (48000.0, 256);
        std::atomic<bool> running { true };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            size_t at = 0;
            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        buffer.setSample (ch, i, voice[(at + (size_t) i) % voice.size()]);
                at += 256;
                p.processBlock (buffer, midi);
            }
        });
        for (int i = 0; i < 30; ++i)
        {
            auto e = openWindow (p);
            for (int f = 0; f < 5; ++f)
                e->tick (1.0 / 30.0);
            (void) e->createComponentSnapshot (e->getLocalBounds(), true, i % 2 == 0 ? 1.0f : 2.0f);
        }
        running = false;
        audio.join();
        check (true, "30 opens and closes with audio running");

        PluginProcessor quiet;
        for (int i = 0; i < 30; ++i)
        {
            auto e = openWindow (quiet);
            (void) e->createComponentSnapshot (e->getLocalBounds(), true, 1.0f);
        }
        check (true, "30 opens and closes with no audio");

        // The window outliving nothing: a processor reads fine after its window closes.
        juce::AudioBuffer<float> b (2, 256);
        juce::MidiBuffer midi;
        b.clear();
        quiet.prepareToPlay (48000.0, 256);
        quiet.processBlock (b, midi);
        check (b.getMagnitude (0, 256) == 0.0f, "audio still runs after the window closes");
    }

    // The window's reading of the input leaves the audio path allocation free and the input alone.
    {
        PluginProcessor p;
        prepare (p, 2, 48000.0, 256);
        juce::AudioBuffer<float> b (2, 256);
        juce::MidiBuffer midi;
        for (int i = 0; i < 256; ++i)
        {
            b.setSample (0, i, 0.25f);
            b.setSample (1, i, -0.5f);
        }
        const long allocsBefore = testkit::allocs.load();
        p.processBlock (b, midi);
        check (testkit::allocs.load() == allocsBefore, "no allocation with the new reading");
        check (std::abs (p.takeInputPeak() - 0.5f) < 1.0e-6f, "the input peak reading");
        check (p.takeInputPeak() == 0.0f, "reading it clears it");
    }
}
