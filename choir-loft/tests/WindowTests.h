#pragma once
// The window, tested without a screen. Included by PluginTest.cpp, inside its namespace.
// The sound pinned bit for bit, by `PluginTest fingerprints`. These held the 1.1.0 and 1.2.0 numbers until 1.4.0, which changed how voices
// start on purpose (phrase starts, see tests/OnsetTests.h): 1.3.1's output was still identical to 1.1.0 at presets 0, 3, 4, 7 and 10 and to 1.2.0
// at every preset. 1.4.0's numbers held through 1.4.1. 1.4.2 changed starts on purpose again (a start is checked for breath and written back to
// where it began), and its numbers held through 1.4.4.
// They now hold 1.5.0's with Human at 0. 1.5.0 fixed a click: at a start a voice's own copy of the consonant before the note jumped to the
// note's new place behind the lead while it was still fading out. That changes the 5 ms after each start wherever Consonant Scatter is above
// 0, so every preset but Tight moved. Tight has no scatter and still gives 1.4.2's two numbers, bit for bit. The other change to the sound
// in 1.5.0, Alto/Tenor following notes under 98 Hz, does not show here: this material never goes that low. With those two changes left out,
// the 1.5.0 source gives 1.4.4's output bit for bit on Luke's 11 takes at the default, Full Stack and Tight (realvox, see the README), so
// nothing else in 1.5.0, the Human control at 0 included, touches the sound.
// They now hold 1.5.1's with Human at 0. 1.5.1 changed starts on purpose once more: the period of a start is chosen among the deep dips and
// their halves, thirds and quarters (see startPeriod in src/PitchTracker.h), which calls a breathy high start 20 to 50 ms sooner and on the
// right note, and calls a clean start on its period a few ms sooner where the old rule sat on a passing harmonic first. Every preset moved,
// Tight too. Any later change that is not meant shows here again.

namespace windowtests
{
struct Fingerprint { int preset; std::uint64_t sum; };
// Presets 0, 3, 4, 7 and 10 with four voices, as 1.1.0 measured them.
constexpr Fingerprint before11[] = { { 0, 4098435958583399820ull }, { 3, 15994760532772079402ull }, { 4, 15136841548073866403ull },
                                     { 7, 11747136029797576096ull }, { 10, 16349240374929031256ull } };

// Each preset at its own voice count. -1 is the settings as first opened.
constexpr Fingerprint before12[] = {
    { -1, 6964773355638076092ull }, { 0, 13826634681334812772ull }, { 1, 16858024752313915086ull }, { 2, 14455849956877040184ull },
    { 3, 15994760532772079402ull }, { 4, 15136841548073866403ull }, { 5, 3377724399809152291ull }, { 6, 3601937124367659ull },
    { 7, 11747136029797576096ull }, { 8, 8406555889939023338ull }, { 9, 14590588442701139453ull }, { 10, 10716541807614450493ull } };

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

// The output light is a reading, not a control, so it has no parameter id to be found by
inline ui::OutputLight* outputLightIn (juce::Component& panel)
{
    for (auto* c : panel.getChildren())
        if (auto* light = dynamic_cast<ui::OutputLight*> (c))
            return light;
    return nullptr;
}

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
    const juce::StringArray readings { "detectedPitch", "leadNote", "voice1Note", "voice2Note", "voice3Note", "voice4Note", "voice5Note", "voice6Note", "voice7Note", "voice8Note",
                                       "voicesSounding", "activeKey" };

    // The sound, bit for bit, against 1.5.0 with Human at 0 (Tight: against 1.4.2 still), and with the window open and drawing all the way through.
    for (const auto& f : before12)
        check (outputFingerprintOf (f.preset, false, false) == f.sum,
               "null test: bit for bit the same as 1.5.1 at Human 0, each preset at its voice count, " + (f.preset < 0 ? std::string ("first opened") : "preset " + std::to_string (f.preset)));

    for (const auto& f : before11)
    {
        check (outputFingerprint (f.preset) == f.sum, "null test: output identical to 1.5.1 at Human 0, four voices, preset " + std::to_string (f.preset));

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
                if (! readings.contains (withId->paramID) && withId->paramID != "moreVoices" && ids.count (withId->paramID) == 0)
                {
                    ++missing;
                    std::printf ("  no control for %s\n", withId->paramID.toRawUTF8());
                }
        check (missing == 0, "every parameter has a control in the window", (double) missing, "missing");

        int notParameters = 0;
        setParam (*p, "moreVoices", 4.0f);
        setParam (*p, "voices", 4.0f);
        e->tick (1.0 / 30.0);
        for (const auto& id : ids)
            if (id != "presets" && id != "advanced" && id != "hearAll" && p->apvts.getParameter (id) == nullptr)
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

    // 1.5.2. The Ceiling switch and the output light: in the window at every voice count with Advanced open or closed, nothing cut off,
    // nothing overlapped, and the light lit when the output is within 1 dB of full scale whether the ceiling is on or off.
    {
        int misplaced = 0, cut = 0;
        for (const bool open : { false, true })
            for (int count = 1; count <= PluginProcessor::maxVoices; ++count)
            {
                auto p = make();
                p->advancedOpen.store (open);
                setParam (*p, "voices", (float) std::min (count, 4));
                setParam (*p, "moreVoices", count > 4 ? (float) (count - 4) : 0.0f);
                auto e = openWindow (*p);
                e->tick (1.0 / 30.0);
                auto& panel = e->getPanel();
                auto* toggle = findById<ui::PillToggle> (panel, "ceiling");
                auto* light = outputLightIn (panel);
                if (toggle == nullptr || light == nullptr || ! toggle->isVisible() || ! light->isVisible()) { ++misplaced; continue; }
                if (! panel.getLocalBounds().contains (toggle->getBounds()) || ! panel.getLocalBounds().contains (light->getBounds())) ++misplaced;
                for (auto* c : panel.getChildren())
                    if (c->isVisible() && c != toggle && c != light && (c->getBounds().intersects (toggle->getBounds()) || c->getBounds().intersects (light->getBounds())))
                        ++misplaced;
                if (toggle->getBounds().intersects (light->getBounds())) ++misplaced;
                if (juce::GlyphArrangement::getStringWidth (ui::font (ui::Weight::medium, 13.0f), toggle->getButtonText()) > (float) toggle->getWidth() - 16.0f) ++cut;
                if (ui::OutputLight::widestText() > (float) light->getWidth()) ++cut;
            }
        check (misplaced == 0, "Ceiling and the output light are in the window and clear of everything else, 1 to 8 voices, Advanced open and closed", (double) misplaced, "misplaced");
        check (cut == 0, "and neither is cut off: the switch's word and the longest thing the light can say fit", (double) cut, "cut off");
        info ("the light's longest words against its width", 100.0 * ui::OutputLight::widestText() / 288.0, "%");

        ui::OutputLight light;
        light.feed (std::pow (10.0f, -3.0f / 20.0f), 0.033);
        check (! light.isHot() && light.text() == "Out -3.0 dB", "the light at -3 dBFS is not lit and reads " + light.text().toStdString());
        light.feed (std::pow (10.0f, -0.9f / 20.0f), 0.033);
        check (light.isHot() && light.text() == "Out -0.9 dB, near full scale", "at -0.9 dBFS it is lit and reads " + light.text().toStdString());
        light.feed (std::pow (10.0f, 2.5f / 20.0f), 0.033);
        check (light.isHot() && light.text() == "Out +2.5 dB, over full scale", "over full scale it reads " + light.text().toStdString());
        light.feed (0.0f, 1.0);
        check (light.isHot(), "it holds its reading for a moment after the peak");
        for (int i = 0; i < 300; ++i) light.feed (0.0f, 0.033);
        check (! light.isHot() && light.text().isEmpty(), "and goes out when nothing comes out");
        for (const juce::String& words : { ui::OutputLight::textFor (-3.0f), ui::OutputLight::textFor (-0.5f), ui::OutputLight::textFor (4.0f) })
            check (! hasEmDash (words), "no em dash in " + words.toStdString());

        // In the window itself: a loud take lights it with the ceiling off, and with it on (the ceiling holds -0.3 dBFS, which is within 1 dB)
        for (const bool ceilingOn : { false, true })
        {
            auto p = make();
            prepare (*p, 2, 48000.0, 256);
            setParam (*p, "ceiling", ceilingOn ? 1.0f : 0.0f);
            auto e = openWindow (*p);
            auto x = voice::synth (voice::steady (220.0, 1.0, 48000.0), voice::vowelA, 48000.0, -40.0, -12.0);
            const double pk = peak (x.data(), (int) x.size());
            for (auto& v : x) v = (float) (v * 0.89 / pk);
            render (*p, x, 256);
            e->tick (1.0 / 30.0);
            auto* light = outputLightIn (e->getPanel());
            check (light != nullptr && light->isHot(), std::string ("a lead at -1 dBFS lights the output light, Ceiling ") + (ceilingOn ? "on" : "off"), light != nullptr ? light->peakDb() : 0.0, "dBFS");
            if (ceilingOn)
                check (light != nullptr && light->peakDb() <= -0.3f + 0.001f, "and with Ceiling on the light reads no higher than the ceiling", light != nullptr ? light->peakDb() : 0.0, "dBFS");
        }
    }

    // The lead has a card of its own, first in the row of singers, at every voice count
    {
        auto p = make();
        auto e = openWindow (*p);
        auto& panel = e->getPanel();
        int misplaced = 0, misaligned = 0, hidden = 0, doubled = 0;

        for (int voices = 1; voices <= 8; ++voices)
        {
            setParam (*p, "voices", (float) juce::jmin (voices, 4));
            setParam (*p, "moreVoices", (float) juce::jmax (0, voices - 4));
            e->tick (1.0 / 30.0);

            auto* on = findById<ui::SingerSwitch> (panel, "leadOn");
            auto* level = findById<ui::BarSlider> (panel, "leadLevel");
            auto* pan = findById<ui::BarSlider> (panel, "leadPan");
            auto* v1Head = findById<ui::SingerSwitch> (panel, "voice1On");
            auto* v1Level = findById<ui::BarSlider> (panel, "voice1Level");
            auto* v1Pan = findById<ui::BarSlider> (panel, "voice1Pan");
            const auto in = [&] (juce::Component* c) { return panel.getLocalArea (c->getParentComponent(), c->getBounds()); };

            for (auto* c : { (juce::Component*) on, (juce::Component*) level, (juce::Component*) pan })
            {
                if (! c->isVisible() || c->getAlpha() < 0.99f || c->getWidth() < 80)   // isShowing needs a window on screen, which a test has not got
                    ++hidden;
                if (in (c).getRight() > in (v1Head).getX() || in (c).getX() < 0)
                    ++misplaced;
            }

            if (in (on).getY() != in (v1Head).getY() || in (level).getY() != in (v1Level).getY() || in (pan).getY() != in (v1Pan).getY()
                || in (level).getHeight() != in (v1Level).getHeight())
                ++misaligned;

            // One control for each of the lead's parameters, not a second copy somewhere else in the window.
            for (const char* id : { "leadOn", "leadLevel", "leadPan" })
            {
                int count = 0;
                std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
                {
                    if (c.getComponentID() == id)
                        ++count;
                    for (auto* child : c.getChildren())
                        walk (*child);
                };
                walk (*e);
                if (count != 1)
                    ++doubled;
            }
        }

        check (hidden == 0, "the lead's switch, Level and Pan show at every voice count, Advanced closed", (double) hidden, "hidden");
        check (misplaced == 0, "the lead's card sits left of voice 1 at every voice count", (double) misplaced, "misplaced");
        check (misaligned == 0, "the lead's name, Level and Pan line up with a voice's", (double) misaligned, "misaligned");
        check (doubled == 0, "each lead control is in the window once", (double) doubled, "doubled");

        auto* on = findById<ui::SingerSwitch> (panel, "leadOn");
        auto* leadOn = p->apvts.getParameter ("leadOn");
        on->mouseDown (mouseAt (*on, { 40.0f, 10.0f }));
        e->tick (1.0 / 30.0);
        check (leadOn->getValue() < 0.5f && on->getRight() == "Off", "a click on the lead's name turns it off and the card says Off, " + on->getRight().toStdString());
        check (findById<ui::BarSlider> (panel, "leadLevel")->getAlpha() < 0.9f && findById<ui::BarSlider> (panel, "leadPan")->getAlpha() < 0.9f, "and its Level and Pan dim");
        on->mouseDown (mouseAt (*on, { 40.0f, 10.0f }));
        e->tick (1.0 / 30.0);
        check (leadOn->getValue() >= 0.5f && on->getRight() != "Off", "a second click brings it back");
    }

    // With the lead's card in the row the voice cards are narrower. Every interval's words still fit beside the arrows.
    {
        auto p = make();
        auto e = openWindow (*p);
        auto& panel = e->getPanel();
        double worst = -1.0e9, squeeze = 0.0;
        std::string where;

        for (int voices = 1; voices <= 8; ++voices)
            for (const int mode : { (int) modeScale, (int) modeFixed, (int) modeMidi, (int) modeChannel })
            {
                setParam (*p, "voices", (float) juce::jmin (voices, 4));
                setParam (*p, "moreVoices", (float) juce::jmax (0, voices - 4));
                setParam (*p, "mode", (float) mode);

                for (int semitones = -24; semitones <= 24; ++semitones)
                {
                    for (int v = 0; v < voices; ++v)
                        setParam (*p, "voice" + juce::String (v + 1) + "Interval", (float) semitones);
                    e->tick (1.0 / 30.0);

                    for (int v = 0; v < voices; ++v)
                    {
                        auto* field = findById<ui::IntervalField> (panel, "voice" + juce::String (v + 1) + "Interval");
                        const double over = field->wordsOverflow();
                        squeeze = std::max (squeeze, (double) field->wordsSqueeze());
                        if (over > worst)
                        {
                            worst = over;
                            where = std::to_string (voices) + " voices, mode " + std::to_string (mode) + ", " + std::to_string (semitones) + " st";
                        }
                    }
                }
            }

        check (worst <= 0.01, "every interval's words fit in its card at every voice count and Mode, worst at " + where, worst, "px over");
        check (squeeze <= 0.03, "and the letters are narrowed by 3% at most to do it", 100.0 * squeeze, "%");
    }

    // 1.5.0: Human sits in the bottom row, first of the three that say how the voices sing, at every voice count, with Advanced open or shut.
    // And no bar anywhere in the window cuts its name or its value short, at every voice count, with long values in every bar.
    for (const bool open : { false, true })
    {
        auto p = make();
        p->advancedOpen.store (open);
        auto e = openWindow (*p);
        auto& panel = e->getPanel();
        int hidden = 0, misplaced = 0, bars = 0;
        double worst = -1.0e9;
        std::string where;

        setParam (*p, "glide", 500.0f);
        setParam (*p, "vibratoRate", 5.55f);
        setParam (*p, "reference", 441.5f);
        setParam (*p, "harmonyLevel", -12.5f);
        setParam (*p, "leadLevel", -12.5f);
        setParam (*p, "leadPan", -100.0f);
        for (int v = 0; v < 8; ++v)
        {
            setParam (*p, vid (v, "Level"), -12.5f);
            setParam (*p, vid (v, "Pan"), -100.0f);
            setParam (*p, vid (v, "Drift"), 100.0f);
            setParam (*p, vid (v, "Tone"), -100.0f);
            setParam (*p, vid (v, "Throat"), 140.0f);
        }

        for (int voices = 1; voices <= 8; ++voices)
            for (const float amount : { 0.0f, 35.0f, 100.0f })
            {
                setParam (*p, "voices", (float) juce::jmin (voices, 4));
                setParam (*p, "moreVoices", (float) juce::jmax (0, voices - 4));
                setParam (*p, "human", amount);
                setParam (*p, "humanize", amount);
                setParam (*p, "vibrato", amount);
                setParam (*p, "mix", amount);
                e->tick (1.0 / 30.0);

                auto* human = findById<ui::BarSlider> (panel, "human");
                auto* humanize = findById<ui::BarSlider> (panel, "humanize");
                auto* vibrato = findById<ui::BarSlider> (panel, "vibrato");
                auto* harmony = findById<ui::BarSlider> (panel, "harmonyLevel");
                if (human == nullptr || ! human->isVisible() || human->getAlpha() < 0.99f || human->getWidth() < 120)
                    ++hidden;
                else if (human->getY() != humanize->getY() || human->getHeight() != humanize->getHeight() || human->getX() < 0
                         || human->getRight() > humanize->getX() || humanize->getRight() > vibrato->getX() || vibrato->getRight() > harmony->getX())
                    ++misplaced;

                std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
                {
                    if (! c.isVisible())
                        return;
                    if (auto* bar = dynamic_cast<ui::BarSlider*> (&c))
                    {
                        ++bars;
                        const double over = bar->textOverflow();
                        if (over > worst)
                        {
                            worst = over;
                            where = bar->getComponentID().toStdString() + " (" + bar->labelText().toStdString() + " " + bar->valueText().toStdString() + ") at " + std::to_string (voices) + " voices";
                        }
                    }
                    for (auto* child : c.getChildren())
                        walk (*child);
                };
                walk (panel);
            }

        const std::string state = open ? "Advanced open" : "Advanced closed";
        check (hidden == 0, "Human shows at every voice count, " + state, (double) hidden, "hidden");
        check (misplaced == 0, "Human, Humanize and Vibrato sit side by side left of Harmony, " + state, (double) misplaced, "misplaced");
        check (bars > 0 && worst <= 0.0, "every bar shows its whole name and value at every voice count, " + state + ", tightest is " + where, worst, "px over");
    }

    // Human by hand
    {
        auto p = make();
        auto e = openWindow (*p);
        auto* bar = findById<ui::BarSlider> (*e, "human");
        auto* prm = p->apvts.getParameter ("human");
        check (bar != nullptr && bar->valueText() == "0%", "Human reads 0% when the window first opens");
        GestureCount watch;
        prm->addListener (&watch);
        bar->applyTyped ("65");
        check (std::abs (prm->convertFrom0to1 (prm->getValue()) - 65.0f) < 0.01f && bar->valueText() == "65%", "typing 65 sets Human, and it reads " + bar->valueText().toStdString());
        check (watch.begins == 1 && watch.ends == 1, "as one gesture", (double) watch.begins, "gestures");
        check (std::abs (bar->getDoubleClickReturnValue()) < 0.01, "double-click on Human returns to 0, the robot sound");
        setParam (*p, "human", 20.0f);
        check (std::abs (bar->getValue() - 20.0) < 0.01, "automation moves the Human control");
        prm->removeListener (&watch);
    }

    // A Key or Scale picked by hand wins over Follow Key
    {
        auto p = make();
        auto e = openWindow (*p);
        auto* follow = p->apvts.getParameter ("followKey");
        auto* keyP = p->apvts.getParameter ("key");
        auto* scaleP = p->apvts.getParameter ("scale");

        setParam (*p, "followKey", 1.0f);
        setParam (*p, "key", 9.0f);
        check (follow->getValue() >= 0.5f, "automation of Key leaves Follow Key on");

        GestureCount watch;
        follow->addListener (&watch);
        findById<ui::ChoiceField> (*e, "key")->pick (4);
        check (juce::roundToInt (keyP->convertFrom0to1 (keyP->getValue())) == 4, "picking a Key in the window sets Key");
        check (follow->getValue() < 0.5f, "picking a Key in the window turns Follow Key off");
        check (watch.begins == 1 && watch.ends == 1, "and Follow Key goes off as one gesture", (double) watch.begins, "gestures");

        setParam (*p, "followKey", 1.0f);
        findById<ui::ChoiceField> (*e, "scale")->pick (scaleMajor);
        check (juce::roundToInt (scaleP->convertFrom0to1 (scaleP->getValue())) == scaleMajor, "picking a Scale in the window sets Scale");
        check (follow->getValue() < 0.5f, "picking a Scale in the window turns Follow Key off");

        findById<ui::ChoiceField> (*e, "key")->pick (7);
        check (watch.begins == 2 && watch.ends == 2, "a pick with Follow Key already off leaves it alone", (double) watch.begins, "gestures");

        setParam (*p, "followKey", 1.0f);
        findById<ui::ChoiceField> (*e, "chordRoot")->pick (2);
        check (follow->getValue() >= 0.5f, "picking a Chord leaves Follow Key on");
        follow->removeListener (&watch);
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

        // A click with no movement switches the singer, and only that. A drag moves it, and only that.
        {
            auto* on = p->apvts.getParameter ("voice1On");
            GestureCount onWatch, panWatch2, intervalWatch2;
            on->addListener (&onWatch);
            panP->addListener (&panWatch2);
            intervalP->addListener (&intervalWatch2);
            const auto at = stage.singerPosition (0);

            stage.mouseDown (mouseAt (stage, at));
            stage.mouseUp (mouseAt (stage, at));
            check (on->getValue() < 0.5f, "a click on a singer turns it off");
            check (onWatch.begins == 1 && onWatch.ends == 1, "as one gesture on its own parameter", (double) onWatch.begins, "gestures");
            check (panWatch2.changes == 0 && intervalWatch2.changes == 0 && panWatch2.begins == 0, "and it moves nothing");
            e->tick (1.0 / 30.0);
            check (! stage.isHeard (0), "the stage shows that singer as off");

            stage.mouseDown (mouseAt (stage, at));
            stage.mouseDrag (mouseAt (stage, at + juce::Point<float> (2.0f, 1.0f)));
            stage.mouseUp (mouseAt (stage, at + juce::Point<float> (2.0f, 1.0f)));
            check (on->getValue() >= 0.5f, "a click with a tiny wobble still counts as a click, and turns it back on");
            check (panWatch2.changes == 0 && intervalWatch2.changes == 0, "and still moves nothing");

            onWatch.begins = onWatch.ends = onWatch.changes = 0;
            stage.mouseDown (mouseAt (stage, at));
            stage.mouseDrag (mouseAt (stage, at + juce::Point<float> (40.0f, 0.0f)));
            stage.mouseUp (mouseAt (stage, at + juce::Point<float> (40.0f, 0.0f)));
            check (onWatch.changes == 0 && on->getValue() >= 0.5f, "a drag moves the singer and leaves it on");
            check (panWatch2.changes > 0 && panWatch2.begins == 1 && panWatch2.ends == 1, "and it is one gesture on pan");

            // Off singers can be dragged too, and stay off.
            for (int i = 0; i < 40; ++i)
                e->tick (1.0 / 30.0);
            const auto moved = stage.singerPosition (0);
            stage.mouseDown (mouseAt (stage, moved));
            stage.mouseUp (mouseAt (stage, moved));
            check (on->getValue() < 0.5f, "off again");
            const float panBefore = panP->getValue();
            stage.mouseDown (mouseAt (stage, moved));
            stage.mouseDrag (mouseAt (stage, moved + juce::Point<float> (-30.0f, 0.0f)));
            stage.mouseUp (mouseAt (stage, moved + juce::Point<float> (-30.0f, 0.0f)));
            check (on->getValue() < 0.5f && panP->getValue() != panBefore, "dragging an off singer moves it and it stays off");

            on->removeListener (&onWatch);
            panP->removeListener (&panWatch2);
            intervalP->removeListener (&intervalWatch2);
        }

        // Its card does the same, and All brings everyone back, the lead too.
        {
            auto* head = findById<ui::SingerSwitch> (*e, "voice2On");
            auto* on2 = p->apvts.getParameter ("voice2On");
            head->mouseDown (mouseAt (*head, { 10.0f, 10.0f }));
            check (on2->getValue() < 0.5f, "a click on a voice's name turns it off");
            head->mouseDown (mouseAt (*head, { 10.0f, 10.0f }));
            check (on2->getValue() >= 0.5f, "and on");

            // The lead, on the stage and by its switch
            for (int i = 0; i < 40; ++i)
                e->tick (1.0 / 30.0);
            auto* leadOn = p->apvts.getParameter ("leadOn");
            const auto leadAt = stage.singerPosition (StageView::leadIndex);
            stage.mouseDown (mouseAt (stage, leadAt));
            stage.mouseUp (mouseAt (stage, leadAt));
            check (leadOn->getValue() < 0.5f, "a click on the lead turns it off, leaving only harmonies");
            on2->setValueNotifyingHost (0.0f);
            auto* v8 = p->apvts.getParameter ("voice8On");
            v8->setValueNotifyingHost (0.0f);
            auto* all = findById<juce::Component> (*e, "hearAll");
            check (all != nullptr, "there is an All button");
            all->mouseDown (mouseAt (*all, { 5.0f, 5.0f }));
            bool everyone = leadOn->getValue() >= 0.5f;
            for (int v = 0; v < 8; ++v)
                everyone = everyone && p->apvts.getParameter (vid (v, "On"))->getValue() >= 0.5f;
            check (everyone, "All brings everyone back");
            e->tick (1.0 / 30.0);
            findById<ui::SingerSwitch> (*e, "leadOn")->mouseDown (mouseAt (*e, { 5.0f, 5.0f }));
            check (leadOn->getValue() < 0.5f, "the lead's own switch works too");
            leadOn->setValueNotifyingHost (1.0f);
        }

        // Voices 1 to 8 as one control, over two parameters
        {
            auto* picker = findById<ui::VoicePicker> (*e, "voices");
            picker->choose (8);
            check (p->voiceCount() == 8 && juce::roundToInt (p->apvts.getRawParameterValue ("voices")->load()) == 4 && juce::roundToInt (p->apvts.getRawParameterValue ("moreVoices")->load()) == 4, "choosing 8 sets Voices 4 and More Voices 4");
            e->tick (1.0 / 30.0);
            int shownCards = 0;
            for (int v = 0; v < 8; ++v)
                if (auto* c = findById<ui::SingerSwitch> (*e, "voice" + juce::String (v + 1) + "On"))
                    if (c->isShowing() || c->getParentComponent()->isVisible())
                        ++shownCards;
            check (shownCards == 8, "8 voices shows 8 cards", shownCards);
            picker->choose (3);
            check (p->voiceCount() == 3 && juce::roundToInt (p->apvts.getRawParameterValue ("moreVoices")->load()) == 0, "choosing 3 takes More Voices back to 0");
            e->tick (1.0 / 30.0);
            shownCards = 0;
            for (int v = 0; v < 8; ++v)
                if (findById<ui::SingerSwitch> (*e, "voice" + juce::String (v + 1) + "On")->getParentComponent()->isVisible())
                    ++shownCards;
            check (shownCards == 4, "up to 4 voices keeps 4 cards", shownCards);
            findById<ui::SingerSwitch> (*e, "voice4On")->mouseDown (mouseAt (*e, { 5.0f, 5.0f }));
            check (p->voiceCount() == 4 && p->apvts.getParameter ("voice4On")->getValue() >= 0.5f, "a click on a spare card adds it, and does not switch it");
        }

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
        check (findById<ui::SingerSwitch> (*e, "leadOn")->getRight() == "A3", "and its card shows that note, " + findById<ui::SingerSwitch> (*e, "leadOn")->getRight().toStdString());
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
