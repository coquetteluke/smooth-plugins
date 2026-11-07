#pragma once
// The window, tested without a screen. Included by PluginTest.cpp, after its helpers.
#include "ReadingFingerprint.h"
#include "editor/Controls.h"
#include "editor/KeyViews.h"

namespace windowtests
{
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

struct Watch
{
    Watch (PluginProcessor& p, const char* id) : param (*p.apvts.getParameter (id)) { param.addListener (&count); }
    ~Watch() { param.removeListener (&count); }
    bool oneGesture() const { return count.begins == 1 && count.ends == 1 && count.changes >= 1; }
    juce::RangedAudioParameter& param;
    GestureCount count;
};

inline juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> at, int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, at, now, clicks, false);
}

inline float plain (PluginProcessor& p, const char* id)
{
    auto* prm = p.apvts.getParameter (id);
    return prm->convertFrom0to1 (prm->getValue());
}

inline std::unique_ptr<KeyEditor> openWindow (PluginProcessor& p)
{
    std::unique_ptr<KeyEditor> e (dynamic_cast<KeyEditor*> (p.createEditor()));
    if (e != nullptr)
        e->setVisible (true);
    return e;
}
}

static void testWindow()
{
    using namespace windowtests;
    using namespace keyui;

    // Detection, bit for bit, against the analyser from before the window. The window's extra data must not change a single reading.
    const auto print = fingerprint::readingFingerprint();
    check (print == fingerprint::expectedBefore, "every reading bit identical to the 1.0.0 analyser");

    // The heavy letters really are heavy, not the font's default weight.
    check (textWidth ("A#", heavy, 1000.0f) > textWidth ("A#", book, 1000.0f) * 1.05f, "the font gives a true heavy weight, wider than regular by a factor of",
           textWidth ("A#", heavy, 1000.0f) / juce::jmax (1.0f, textWidth ("A#", book, 1000.0f)));

    // Typed keys.
    {
        struct Case { const char* text; int key, scale; };
        constexpr Case good[] = { { "C", 0, -1 }, { "F#", 6, -1 }, { "bb", 10, -1 }, { "Bb minor", 10, 1 }, { "b", 11, -1 }, { "bm", 11, 1 }, { "Am", 9, 1 }, { "c# major", 1, 0 },
                                  { "Cb", 11, -1 }, { " e flat ", 3, -1 }, { "g sharp minor", 8, 1 }, { "a min", 9, 1 } };
        bool all = true;
        for (auto& c : good)
        {
            int key = -1, scale = -2;
            all = all && parseKey (c.text, key, scale) && key == c.key && scale == c.scale;
        }
        check (all, "typed keys are read correctly");
        bool none = true;
        for (auto* bad : { "", "h", "hello", "c minr", "123", "#" })
        {
            int key = -1, scale = -2;
            none = none && ! parseKey (bad, key, scale);
        }
        check (none, "typing something that is not a key does nothing");
    }

    PluginProcessor p (tempJson ("window.json"));
    prepare (p, 2, 48000.0, 512);

    {
        auto ed = openWindow (p);
        check (ed != nullptr, "the window opens");
        auto& panel = ed->getPanel();

        // Every control has a place on screen.
        ed->setAdvancedOpen (true, false);
        bool allThere = true;
        for (auto* prm : p.getParameters())
        {
            auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm);
            if (withId == nullptr || dynamic_cast<ReadingParameter*> (prm) != nullptr)
                continue;
            auto* control = findById<juce::Component> (panel, withId->getParameterID());
            const bool ok = control != nullptr && control->isVisible() && panel.getLocalBounds().contains (control->getBounds());
            if (! ok) std::printf ("no control on screen for %s\n", withId->getParameterID().toRawUTF8());
            allThere = allThere && ok;
        }
        check (allThere, "every writable parameter has a visible control in the window");

        // Nothing overlaps once Advanced is open, and it folds away again.
        auto* response = findById<Segments> (panel, "response");
        auto* keyboardView = findById<KeyboardView> (panel, "keyboard");
        check (response != nullptr && keyboardView != nullptr && keyboardView->getBottom() <= response->getY(), "Advanced does not cover the keyboard");
        ed->setAdvancedOpen (false, false);
        check (! response->isVisible(), "Advanced folds away");
        check (ed->isAdvancedOpen() == false, "Advanced remembers it is shut");

        // Automation moves the window.
        auto* mode = findById<Segments> (panel, "mode");
        auto* tempo = findById<Segments> (panel, "tempoScale");
        auto* swap = findById<Pill> (panel, "swapRelative");
        auto* listening = findById<Pill> (panel, "listening");
        auto* keyField = findById<KeyField> (panel, "manualKey");
        setParam (p, "mode", 1.0f);
        setParam (p, "tempoScale", 2.0f);
        setParam (p, "swapRelative", 1.0f);
        setParam (p, "manualKey", 7.0f);
        check (mode->getValue() == 1 && tempo->getValue() == 2 && swap->getToggleState() && keyField->getValue() == 7, "automation moves the window's controls");
        setParam (p, "tempoScale", 0.0f);
        setParam (p, "swapRelative", 0.0f);
        setParam (p, "mode", 0.0f);

        // The window moves the parameters, with a begin and an end. Half is the first block on screen and the second value of the parameter.
        {
            Watch w (p, "tempoScale");
            tempo->mouseDown (mouseAt (*tempo, { tempo->getWidth() * 0.1f, 5.0f }));
            check (w.oneGesture() && (int) plain (p, "tempoScale") == 1, "clicking Half sets Tempo Scale, as one gesture");
        }
        {
            Watch w (p, "tempoScale");
            tempo->mouseDoubleClick (mouseAt (*tempo, { tempo->getWidth() * 0.9f, 5.0f }, 2));
            check (w.oneGesture() && (int) plain (p, "tempoScale") == 0, "double-click returns Tempo Scale to its default");
        }
        {
            Watch w (p, "swapRelative");
            swap->setToggleState (true, juce::sendNotificationSync);
            check (w.oneGesture() && plain (p, "swapRelative") > 0.5f, "Swap sets Relative Swap, as one gesture");
        }
        {
            Watch w (p, "swapRelative");
            swap->mouseDoubleClick (mouseAt (*swap, { 3.0f, 3.0f }, 2));
            check (w.oneGesture() && plain (p, "swapRelative") < 0.5f, "double-click returns Relative Swap to its default");
        }
        {
            Watch w (p, "listening");
            listening->setToggleState (false, juce::sendNotificationSync);
            check (w.oneGesture() && plain (p, "listening") < 0.5f, "Listening turns off, as one gesture");
            setParam (p, "listening", 1.0f);
        }
        {
            auto* reset = findById<ResetButton> (panel, "reset");
            Watch w (p, "reset");
            reset->setState (juce::Button::buttonDown);
            const bool held = plain (p, "reset") > 0.5f;
            reset->setState (juce::Button::buttonNormal);
            check (held && plain (p, "reset") < 0.5f && w.count.begins == 2 && w.count.ends == 2, "Reset is held while pressed and lets go, as real presses");
        }
        {
            Watch k (p, "manualKey"), s (p, "manualScale");
            keyField->applyTyped ("Bb minor");
            check (k.oneGesture() && s.oneGesture() && (int) plain (p, "manualKey") == 10 && (int) plain (p, "manualScale") == 1, "typing Bb minor sets Manual Key and Manual Scale, as a gesture each");
        }
        {
            keyField->applyTyped ("nonsense");
            check ((int) plain (p, "manualKey") == 10, "typing nonsense leaves the key alone");
            keyField->mouseDoubleClick (mouseAt (*keyField, { 3.0f, 3.0f }, 2));
            check ((int) plain (p, "manualKey") == 0, "double-click returns Manual Key to its default");
            setParam (p, "manualScale", 0.0f);
        }

        // The circle: every key is under its own label, and it sets nothing unless in Manual.
        auto* circle = findById<CircleView> (panel, "circle");
        bool mapped = true;
        for (int k = 0; k < 24; ++k)
            mapped = mapped && circle->keyAt (circle->pointForKey (k)) == k;
        check (mapped, "each of the 24 keys on the circle is where its label says");
        check (circle->keyAt (circle->centre()) == -1, "the middle of the circle is not a key");

        const auto spotFor = circle->pointForKey (12 + 4);
        circle->mouseDown (mouseAt (*circle, spotFor));
        check ((int) plain (p, "manualKey") == 0 && (int) plain (p, "manualScale") == 0, "clicking the circle in Listen changes nothing");

        setParam (p, "mode", 1.0f);
        sleepMs (60);
        ed->refresh();
        {
            Watch k (p, "manualKey"), s (p, "manualScale");
            circle->mouseDown (mouseAt (*circle, spotFor));
            check (k.oneGesture() && s.oneGesture() && (int) plain (p, "manualKey") == 4 && (int) plain (p, "manualScale") == 1, "clicking E minor on the circle in Manual sets it");
        }
        {
            bool notes = true;
            auto* kb = findById<KeyboardView> (panel, "keyboard");
            for (int note = 0; note < 12; ++note)
            {
                // Click each note in the top of black keys and the bottom of white ones.
                const float whiteWidth = (float) kb->getWidth() / 7.0f;
                constexpr float xs[12] = { 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.5f, 4.0f, 4.5f, 5.0f, 5.5f, 6.0f, 6.5f };
                const bool blackKey = note == 1 || note == 3 || note == 6 || note == 8 || note == 10;
                const juce::Point<float> at (xs[note] * whiteWidth, blackKey ? 10.0f : (float) kb->getHeight() - 5.0f);
                notes = notes && kb->noteAt (at) == note;
            }
            check (notes, "each of the 12 keyboard notes is under its label");
        }
        setParam (p, "mode", 0.0f);
        sleepMs (60);
        ed->refresh();

        // The letters stay the same size whatever the key, and the window stays readable at its smallest.
        ed->setSize (PluginProcessor::minWindowWidth, juce::roundToInt ((double) PluginProcessor::minWindowWidth * KeyEditor::designHeight / KeyEditor::designWidth));
        const float smallest = (float) ed->getKeyLetterBounds().getHeight() * (float) ed->getWidth() / (float) KeyEditor::designWidth;
        info ("height of the key letters at the smallest window", smallest, "pt");
        check (smallest >= 150.0f, "key letters at least 150 points tall at the smallest window", smallest, "pt");
    }

    // Size and open section are saved. Bad values cannot do harm, and nothing else is added to the saved state.
    {
        PluginProcessor a (tempJson ("window.json")), b (tempJson ("window.json"));
        {
            auto ed = openWindow (a);
            ed->setSize (1280, juce::roundToInt (1280.0 * KeyEditor::designHeight / KeyEditor::designWidth));
            ed->setAdvancedOpen (true, false);
        }
        juce::MemoryBlock saved;
        a.getStateInformation (saved);
        b.setStateInformation (saved.getData(), (int) saved.getSize());
        check (b.windowWidth.load() == 1280 && b.advancedOpen.load(), "window size and Advanced come back with the session");
        {
            auto ed = openWindow (b);
            check (ed->getWidth() == 1280 && ed->isAdvancedOpen(), "the window reopens at its saved size with Advanced open");
        }

        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        check (xml != nullptr && xml->getNumAttributes() == 2, "only window size and open section are added to saved state, attributes", xml != nullptr ? xml->getNumAttributes() : -1);

        xml->setAttribute ("windowWidth", 999999);
        juce::MemoryBlock bad;
        juce::AudioProcessor::copyXmlToBinary (*xml, bad);
        b.setStateInformation (bad.getData(), (int) bad.getSize());
        check (b.windowWidth.load() == PluginProcessor::maxWindowWidth, "a damaged window size is clamped");

        // A session from before the window has no such attributes. It keeps working and opens at the default size.
        xml->removeAttribute ("windowWidth");
        xml->removeAttribute ("advancedOpen");
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        PluginProcessor c (tempJson ("window.json"));
        c.setStateInformation (old.getData(), (int) old.getSize());
        check (c.windowWidth.load() == PluginProcessor::defaultWindowWidth && ! c.advancedOpen.load(), "a session from before the window opens at the default size");
    }

    // Opening, drawing and closing the window many times while audio runs: audio stays bit identical and nothing blocks it.
    {
        PluginProcessor q (tempJson ("window.json"));
        prepare (q, 2, 48000.0, 512);
        std::atomic<bool> running { true };
        std::atomic<long> differing { 0 }, blocks { 0 };
        std::atomic<double> worstMs { 0.0 };

        std::thread audio ([&]
        {
            std::mt19937 rng (9);
            std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
            juce::AudioBuffer<float> buf (2, 512), copy (2, 512);
            juce::MidiBuffer midi;
            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* x = buf.getWritePointer (ch);
                    for (int i = 0; i < 512; ++i) x[i] = uni (rng);
                    copy.copyFrom (ch, 0, buf, ch, 0, 512);
                }
                const auto started = std::chrono::steady_clock::now();
                q.processBlock (buf, midi);
                const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - started).count();
                if (ms > worstMs.load()) worstMs.store (ms);
                for (int ch = 0; ch < 2; ++ch)
                    if (std::memcmp (buf.getReadPointer (ch), copy.getReadPointer (ch), sizeof (float) * 512) != 0) ++differing;
                ++blocks;
                sleepMs (2);
            }
        });

        for (int i = 0; i < 30; ++i)
        {
            auto ed = openWindow (q);
            for (int f = 0; f < 3; ++f)
            {
                ed->refresh();
                ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            }
            ed->setAdvancedOpen (i % 2 == 0, false);
        }

        running = false;
        audio.join();
        check (differing.load() == 0 && blocks.load() > 50, "audio bit identical while the window opened, drew and closed 30 times, blocks", (double) blocks.load());
        info ("slowest audio block during that", worstMs.load(), "ms");
        check (worstMs.load() < 10.67, "no audio block took longer than the block itself during that", worstMs.load(), "ms");
    }
}
