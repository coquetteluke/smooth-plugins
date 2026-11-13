#pragma once
// Phase 2 (2026-11-13): a check for each bug the hunt kept, and for the two patterns the other plug-ins took up (a host Reset, and
// the preset chosen saved with the session). Each was run on 1.7.1, with test hooks that change nothing, before its fix, and failed
// there for the reason the hunt gave. Included by PluginTest.cpp after LockTests.h. PluginTest --phase2 runs only these.
// Limits here were written before any of these ran. Never loosen one to make a run pass.
#include <sys/stat.h>

namespace phase2limits
{
    constexpr double backAfterPauseSeconds = 15.0;   // the same song again after a pause: shown within this, as a fresh song is
    constexpr double refreshAtMostSeconds = locklimits::refreshAtMostSeconds;   // a held key: written again at least this often
    constexpr double bounceWriteSeconds = 0.3;       // in an offline bounce a new reading reaches key.json within this of wall time
    constexpr double hostResetSignalSeconds = 0.5;   // after a host Reset, this much silence is heard as silence
}

namespace phase2
{
using locktests::reading;
using locktests::setLock;
using locktests::waitFor;
using smoothkey::KeyFile;
using smoothkey::KeyState;
using smoothkey::Reading;
using windowtests::findById;
using windowtests::mouseAt;

inline void pushAll (KeyAnalyser& a, const std::vector<float>& audio)
{
    for (size_t pos = 0; pos < audio.size(); pos += 4096)
        a.push (audio.data() + pos, (int) std::min<size_t> (4096, audio.size() - pos));
}

inline void pushSilence (KeyAnalyser& a, double seconds, double sr)
{
    std::vector<float> zeros ((size_t) (seconds * sr), 0.0f);
    pushAll (a, zeros);
}

inline int shownOf (const Reading& r) { return r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0); }

// Seconds of audio until the reading shows want, or -1.
inline double secondsUntil (KeyAnalyser& a, const std::vector<float>& audio, double sr, int want)
{
    for (size_t pos = 0; pos < audio.size(); pos += 512)
    {
        a.push (audio.data() + pos, (int) std::min<size_t> (512, audio.size() - pos));
        if (shownOf (a.reading()) == want) return (double) (pos + 512) / sr;
    }
    return -1.0;
}

inline juce::String fileText (const juce::String& path) { return juce::File (path).loadFileAsString(); }

inline std::unique_ptr<KeyEditor> window (PluginProcessor& p)
{
    std::unique_ptr<KeyEditor> e (dynamic_cast<KeyEditor*> (p.createEditor()));
    if (e != nullptr) e->setVisible (true);
    return e;
}

inline bool show (PluginProcessor& p, const Reading* r, const std::function<bool (const PluginProcessor::Display&)>& ready)
{
    p.setReadingForTests (r);
    return waitFor ([&] { return ready (p.getDisplay()); }, 2000);
}

inline juce::Rectangle<int> inPanel (juce::Component& c, juce::Component& panel)
{
    return panel.getLocalArea (c.getParentComponent(), c.getBounds());
}

// The part of the hint's own text that something visible, drawn above the hint line, covers. In design points.
inline int hintCovered (juce::Component& panel)
{
    auto* hint = findById<keyui::Readout> (panel, "hint");
    if (hint == nullptr) return 9999;
    const float wordsWidth = keyui::textWidth (hint->getText(), keyui::book, 15.0f);
    const auto words = hint->getBounds().withWidth (juce::jmin (hint->getWidth(), (int) std::ceil (wordsWidth)));
    int covered = 0;
    bool above = false;
    for (auto* c : panel.getChildren())
    {
        if (c == hint) { above = true; continue; }
        if (above && c->isVisible() && c->getAlpha() > 0.0f)
            covered = juce::jmax (covered, c->getBounds().getIntersection (words).getWidth());
    }
    return covered;
}

// The pixels of the bottom rows of a view that are not plain paper.
inline int inkInBottomRows (juce::Component& c, int rows)
{
    const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
    int count = 0;
    for (int y = image.getHeight() - rows; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto px = image.getPixelAt (x, y);
            const auto paper = keyui::tone::paper;
            if (std::abs ((int) px.getRed() - (int) paper.getRed()) + std::abs ((int) px.getGreen() - (int) paper.getGreen())
                    + std::abs ((int) px.getBlue() - (int) paper.getBlue()) > 60)
                ++count;
        }
    return count;
}

inline juce::File baselineFolder()
{
    return juce::File (juce::String (__FILE__)).getParentDirectory().getChildFile ("baseline").getChildFile ("v1.7.1");
}
}

// ---- the analyser: a pause, a DC offset, fading, sharp tuning ------------------------------------------------------

static void testPhase2Analyser()
{
    using namespace phase2;
    const double sr = 48000.0;
    const auto ps = progressions();
    const auto ts = timbres();

    // key-dsp-r1-1. C major, then G major, a pause, then C major again. After a pause the reading must be free to move.
    for (int stopped = 0; stopped < 2; ++stopped)
    {
        KeyAnalyser a;
        a.configure (sr);
        a.setResponse (Response::stable);
        pushAll (a, chordClip (0, ps[0], ts[0], clipSeed (20261113, 0, 0, 0), sr, 60.0));
        pushAll (a, chordClip (7, ps[0], ts[0], clipSeed (20261113, 7, 0, 0), sr, 60.0));
        const bool onG = shownOf (a.reading()) == 7;
        if (stopped == 0) pushSilence (a, 10.0, sr);
        else a.idle (10.0);   // a host that stops calling the plug-in
        const double back = secondsUntil (a, chordClip (0, ps[0], ts[0], clipSeed (20261113, 0, 1, 0), sr, 30.0), sr, 0);
        check (onG && back >= 0.0 && back <= phase2limits::backAfterPauseSeconds,
               std::string ("C major, G major, ") + (stopped ? "the host stopped for 10 s" : "10 s of silence") + ", then C major: shown again after (-1 never in 30 s)", back, "s");
        info ("and the newest time stamp ahead of the analyser clock then", a.stampAhead(), "s");
    }

    // key-dsp-r2-2. Every timer compared with the analyser clock: none may be left on the old timeline by a pause.
    {
        KeyAnalyser a;
        a.configure (sr);
        a.setResponse (Response::stable);
        pushAll (a, chordClip (0, ps[0], ts[0], clipSeed (20261113, 0, 0, 0), sr, 60.0));
        pushAll (a, chordClip (7, ps[0], ts[0], clipSeed (20261113, 7, 0, 0), sr, 60.0));
        pushSilence (a, 10.0, sr);
        pushAll (a, chordClip (7, ps[0], ts[0], clipSeed (20261113, 7, 2, 0), sr, 3.0));
        check (a.stampAhead() <= 1.0e-9, "chords, a 10 s pause, chords: no time stamp lies ahead of the clock, by", a.stampAhead(), "s");
    }
    {
        KeyAnalyser a;
        a.configure (sr);
        a.setResponse (Response::stable);
        a.setSource (Source::voice);
        pushAll (a, sungClip (2, false, clipSeed (20261113, 2, 5, 0), sr, 90.0));
        const bool shown = a.reading().key >= 0;
        pushSilence (a, 10.0, sr);
        pushAll (a, sungClip (2, false, clipSeed (20261113, 2, 6, 0), sr, 3.0));
        check (shown && a.stampAhead() <= 1.0e-9, "a lone voice, a 10 s pause, the voice again: no time stamp lies ahead of the clock, by", a.stampAhead(), "s");
    }

    // key-dsp-r3-2. A DC offset with no notes is not music: the confidence falls as in silence.
    {
        KeyAnalyser a;
        a.configure (sr);
        a.setResponse (Response::stable);
        pushAll (a, chordClip (4, ps[0], ts[0], clipSeed (20261113, 4, 0, 0), sr, 12.0));
        const float before = a.reading().confidence;
        pushAll (a, std::vector<float> ((size_t) (10.0 * sr), 0.01f));
        check (before > 0.3f && a.reading().confidence <= limits::confidenceAfterTenSecondsSilence,
               "chords, then 10 s of a steady 0.01 offset: confidence falls as in silence, to", a.reading().confidence);
    }

    // key-dsp-r3-3. In silence the runner up fades with the key it runs behind. A minor loop heard from its second chord, in Fast,
    // 4 s in, where the runner up is real (0.09). First written with clean chords in one key, then two keys at once, then a lone
    // voice: their runner up was 0.000 to 0.011, so they could not show anything. Corrected to this.
    {
        KeyAnalyser a;
        a.configure (sr);
        a.setResponse (Response::fast);
        const auto audio = chordClip (9, ps[6], ts[2], clipSeed (20261113, 9, 6, 2), sr, 30.0, 1);
        for (size_t pos = 0; pos < audio.size(); pos += 4096)   // up to the moment there is a runner up, about 4 s in
        {
            a.push (audio.data() + pos, (int) std::min<size_t> (4096, audio.size() - pos));
            if (a.reading().key >= 0 && a.reading().altConfidence > 0.05f) break;
        }
        const auto before = a.reading();
        pushSilence (a, 10.0, sr);
        const auto after = a.reading();
        check (before.altKey >= 0 && before.altConfidence > 0.05f && after.altConfidence <= after.confidence + 1.0e-6f,
               "after 10 s of silence the runner up is no surer than the key on show: runner up at", after.altConfidence);
        info ("runner up before the silence", before.altConfidence);
        info ("key on show after it", after.confidence);
    }

    // key-dsp-r2-1. Music tuned 45 cents sharp, with bright noise above 5 kHz. The top of the note range must be read, not guessed.
    {
        const auto outsideShare = [&] (double cents)
        {
            std::vector<float> audio ((size_t) (20.0 * sr), 0.0f);
            std::vector<Note> notes;
            for (double t = 0.0; t < 20.0; t += 2.0)
                for (int midi : { 54, 58, 61, 66, 70, 73 })   // F# major, held
                    notes.push_back ({ t, 2.0, midi, 0.15 });
            renderNotes (audio, notes, ts[0], cents, sr, 7);
            // White noise, high-passed hard above 5.2 kHz, as cymbals put there.
            std::mt19937 rng (11);
            std::normal_distribution<float> gauss (0.0f, 1.0f);
            juce::IIRFilter hp[6];
            for (auto& f : hp) f.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, 5200.0, 0.707));
            for (auto& x : audio)
            {
                float n = gauss (rng) * 0.05f;
                for (auto& f : hp) n = f.processSingleSampleRaw (n);
                x += n;
            }
            KeyAnalyser a;
            a.configure (sr);
            a.setResponse (Response::stable);
            double overshoot = 0.0;
            for (size_t pos = 0; pos < audio.size(); pos += 4096)
            {
                a.push (audio.data() + pos, (int) std::min<size_t> (4096, audio.size() - pos));
                overshoot = std::max (overshoot, a.topBinOvershoot());
            }
            const auto& c = a.reading().chroma;
            double all = 0.0;
            for (float v : c) all += v;
            return std::pair<double, double> { (c[9] + c[4] + c[0]) / std::max (all, 1.0e-9), overshoot };
        };
        const auto flat = outsideShare (-45.0), sharp = outsideShare (45.0), inTune = outsideShare (0.0);
        // First written as the share of A, E and C in the reading, which read 0.005 sharp, 0.005 flat and 0.007 in tune on 1.7.1:
        // the false value reached no key here. What is checked is the value itself: an interpolation never passes the bins it is read from.
        // Not fixed in 1.7.2 and kept as INFO there, as fixing it changed readings of the detuned chord clips that the check "chords
        // in Full Mix: every reading over 96 clips bit identical to 1.5.1" pinned. Fixed in 1.7.3 with Luke's yes on 2026-11-13, and
        // a check since: 30.6 on 1.7.1 and 1.7.2, 1 now. Rounding aside, a value read between two bins is never above the larger.
        check (sharp.second <= 1.0 + 1.0e-9, "F# major 45 cents sharp with noise above 5 kHz: the top log bin against the largest bin it is read from (1.7.2: 30.6; 1 when read, not drawn on)", sharp.second);
        info ("the same in tune", inTune.second);
        info ("share of A, E and C in the reading, 45 cents sharp", sharp.first);
        info ("the same 45 cents flat", flat.first);
        info ("the same in tune", inTune.first);
    }
}

// ---- key.json: a fixed key stays fresh, closing never makes an old key new, a failure note clears, a bounce writes at once ----

static void testPhase2File()
{
    using namespace phase2;
    const double when = 1794011292.5;

    // key-dsp-r1-2 and key-file-link-r1-2. A key set by hand never changes, like a held one, so it is written again in time too.
    // So is a key frozen by Listening off or Bypass.
    {
        KeyState manual { 4, true, 1.0, false };
        manual.source = 1;
        KeyState paused { 9, false, 0.82, false };
        for (auto* s : { &manual, &paused })
        {
            const auto path = tempJson ("phase2-refresh.json");
            KeyFile file;
            file.setPath (path.toRawUTF8(), 0.5);
            double last = 0.0, worstGap = 0.0;
            long writes = 0;
            for (double t = 0.0; t <= 1200.0; t += 1.0)
            {
                file.update (*s, t, when + t);
                if (file.writeCount() > writes) { if (writes > 0) worstGap = std::max (worstGap, t - last); last = t; writes = file.writeCount(); }
            }
            worstGap = std::max (worstGap, 1200.0 - last);
            check (writes >= 4 && worstGap <= phase2limits::refreshAtMostSeconds,
                   std::string (s == &manual ? "a key set by hand" : "a key frozen with Listening off") + ", unchanged for 20 minutes, is written again in time: longest gap", worstGap, "s");
        }
    }

    // key-file-link-r1-1 (writer) and key-dsp-r3-1. A Key that closes never writes over a newer line from another Key.
    {
        const auto path = tempJson ("phase2-two.json");
        juce::File (path).deleteFile();
        static const Reading eMajor = reading (4, false, 0.8f), gMajor = reading (7, false, 0.8f);
        auto a = std::make_unique<PluginProcessor> (path, 0.05);
        PluginProcessor b (path, 0.05);
        prepare (*a, 2, 48000.0, 512);
        prepare (b, 2, 48000.0, 512);
        a->setReadingForTests (&eMajor);
        const bool aWrote = waitFor ([&] { return fileText (path).contains ("\"key\":\"E\""); }, 2000);
        b.setReadingForTests (&gMajor);
        const bool bWrote = waitFor ([&] { return fileText (path).contains ("\"key\":\"G\""); }, 2000);
        a.reset();
        const auto v = juce::JSON::parse (fileText (path));
        check (aWrote && bWrote && v["key"].toString() == "G", "two Keys, the first one closes: key.json keeps the other's newer key, " + v["key"].toString().toStdString());
        b.setReadingForTests (nullptr);
    }

    // And closing never makes the last line look newer than it is.
    {
        const auto path = tempJson ("phase2-close.json");
        juce::File (path).deleteFile();
        double written = 0.0, closed = 0.0;
        {
            PluginProcessor p (path, 0.05);
            prepare (p, 2, 48000.0, 512);
            setParam (p, "mode", 1.0f);
            setParam (p, "manualKey", 4.0f);
            waitFor ([&] { return fileText (path).contains ("\"key\":\"E\""); }, 2000);
            written = (double) juce::JSON::parse (fileText (path))["time"];
            sleepMs (1500);
        }
        const auto v = juce::JSON::parse (fileText (path));
        closed = (double) v["time"];
        check (written > 0.0 && std::abs (closed - written) < 0.05 && ! (bool) v["listening"],
               "closing keeps the time of the last real write, and says listening false: moved the time by", closed - written, "s");
    }

    // key-window-r2-3. A write fails, then the state goes back to the line already on disk: the note does not stay up.
    {
        const auto folder = testFolder().getChildFile ("phase2-blocked");
        folder.deleteRecursively();
        folder.createDirectory();
        const auto path = folder.getChildFile ("key.json").getFullPathName();
        KeyFile file;
        file.setPath (path.toRawUTF8(), 0.5);
        KeyState fSharp { 6, true, 1.0, false }, g { 7, true, 1.0, false };
        fSharp.source = g.source = 1;
        file.update (fSharp, 0.0, when);
        chmod (folder.getFullPathName().toRawUTF8(), 0500);
        file.update (g, 1.0, when + 1.0);
        const bool failed = file.hasFailed();
        chmod (folder.getFullPathName().toRawUTF8(), 0755);
        for (double t = 2.0; t <= 20.0; t += 0.5) file.update (fSharp, t, when + t);
        check (failed && ! file.hasFailed() && juce::File (path).loadFileAsString().contains ("\"key\":\"F#\""),
               "a write fails, then the key goes back to the line on disk: the failure is cleared");
    }

    // key-file-link-r2-1, Key's half. In a bounce faster than real time, a new reading is written at once, not after half a second.
    for (int offline = 0; offline < 2; ++offline)
    {
        const auto path = tempJson ("phase2-bounce.json");
        juce::File (path).deleteFile();
        static const Reading c = reading (0, false, 0.8f), d = reading (2, false, 0.8f);
        PluginProcessor p (path, 0.5);
        p.setNonRealtime (offline == 1);
        prepare (p, 2, 48000.0, 512);
        p.setReadingForTests (&c);
        waitFor ([&] { return fileText (path).contains ("\"key\":\"C\""); }, 2000);
        const double from = juce::Time::getMillisecondCounterHiRes();
        p.setReadingForTests (&d);
        waitFor ([&] { return fileText (path).contains ("\"key\":\"D\""); }, 2000);
        const double took = (juce::Time::getMillisecondCounterHiRes() - from) * 0.001;
        if (offline == 1)
            check (took <= phase2limits::bounceWriteSeconds, "offline bounce: a new reading reaches key.json within", took, "s");
        else
            info ("playing in real time: a new reading half a second after the last write reaches key.json within", took, "s");
        p.setReadingForTests (nullptr);
    }
}

// ---- the readings as the host sees them ----------------------------------------------------------------------------

static void testPhase2Readings()
{
    using namespace phase2;
    PluginProcessor p (tempJson ("phase2-readings.json"));
    prepare (p, 2, 48000.0, 512);

    // key-dsp-r1-3. The Confidence reading is a whole percent.
    static const Reading odd = reading (4, false, 0.784615f);
    p.setReadingForTests (&odd);
    waitFor ([&] { return p.getDisplay().key == 4; }, 2000);
    sleepMs (60);
    const auto text = locktests::readingText (p, "Confidence");
    check (text == "78", "the Confidence reading is a whole percent: " + text.toStdString());
    p.setReadingForTests (nullptr);

    // HostStress paramtext. Each reading's own text reads back as the value it shows.
    int wrong = 0;
    juce::String example;
    for (auto* prm : p.getParameters())
    {
        if (dynamic_cast<ReadingParameter*> (prm) == nullptr) continue;
        for (int i = 0; i <= 12; ++i)
        {
            const float v = (float) i / 12.0f;
            const auto shown = prm->getText (v, 64);
            const auto back = prm->getText (prm->getValueForText (shown), 64);
            if (back != shown) { ++wrong; example = prm->getName (64) + ": " + shown + " reads back as " + back; }
        }
    }
    check (wrong == 0, "every reading's text reads back as the value it shows, wrong: " + std::to_string (wrong) + " " + example.toStdString());
}

// ---- the window ----------------------------------------------------------------------------------------------------

static void testPhase2Window()
{
    using namespace phase2;
    using namespace keyui;
    static const Reading eMajor = reading (4, false, 0.8f, 9, false, 0.3f, 0.6f);
    static const Reading aMajor = reading (9, false, 0.8f, 4, false, 0.3f, 0.6f);
    static const Reading eOrA = reading (4, false, 0.7f, 9, false, 0.45f, 0.03f, true);

    // key-window-r1-1. Every hint the window gives can be read in full: nothing is drawn over it.
    {
        PluginProcessor p (tempJson ("phase2-hints.json"));
        prepare (p, 2, 48000.0, 512);
        auto ed = window (p);
        auto& panel = ed->getPanel();
        int worst = 0, cut = 0, hints = 0;
        std::string worstHint;
        for (int state = 0; state < 2; ++state)
        {
            if (state == 0) show (p, &eMajor, [] (const PluginProcessor::Display& d) { return d.key == 4; });
            else { p.setReadingForTests (nullptr); setParam (p, "mode", 1.0f); waitFor ([&] { return p.getDisplay().manual; }, 2000); }
            sleepMs (700);   // key.json has been written
            ed->refresh();
            std::function<void (juce::Component&)> visit = [&] (juce::Component& c)
            {
                if (auto* hinted = dynamic_cast<Hinted*> (&c))
                    if (hinted->hint.isNotEmpty() && c.isVisible())
                    {
                        ++hints;
                        ed->previewHover (&c);
                        auto* line = findById<Readout> (panel, "hint");
                        const int covered = hintCovered (panel);
                        if (line == nullptr || line->getText() != hinted->hint) ++cut;
                        if (covered > worst) { worst = covered; worstHint = hinted->hint.toStdString(); }
                    }
                for (auto* child : c.getChildren()) visit (*child);
            };
            visit (panel);
        }
        setParam (p, "mode", 0.0f);
        info ("hints looked at, in Listen with a key and in Manual", hints);
        check (worst == 0, "no hint is drawn over by anything on the footer, covered at most (" + worstHint + ")", worst, "pt");
        check (cut == 0, "every hint is shown in full, not swapped for the resting one, swapped", cut);
    }

    // key-window-r2-1. Dragging and scrolling Manual Key is one gesture.
    {
        PluginProcessor p (tempJson ("phase2-drag.json"));
        prepare (p, 2, 48000.0, 512);
        auto ed = window (p);
        ed->setAdvancedOpen (true, false);
        auto* field = findById<KeyField> (ed->getPanel(), "manualKey");
        {
            windowtests::Watch w (p, "manualKey");
            const auto now = juce::Time::getCurrentTime();
            const auto at = [&] (float dy)
            {
                return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), { 20.0f, 18.0f + dy }, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                         1.0f, 0.0f, 0.0f, 0.0f, 0.0f, field, field, now, { 20.0f, 18.0f }, now, 1, dy != 0.0f);
            };
            field->mouseDown (at (0.0f));
            for (float dy : { -12.0f, -22.0f, -32.0f }) field->mouseDrag (at (dy));
            field->mouseUp (at (-32.0f));
            check (w.count.begins == 1 && w.count.ends == 1 && (int) windowtests::plain (p, "manualKey") == 3,
                   "dragging Manual Key three steps is one gesture: begins " + std::to_string (w.count.begins) + ", ends " + std::to_string (w.count.ends));
        }
        {
            // The wheel has no let go, so its gesture ends after a pause, or when the field is pressed, or when the window closes.
            // There is no message loop here to run that pause, so the window is closed.
            windowtests::Watch w (p, "manualKey");
            juce::MouseWheelDetails wheel {};
            wheel.deltaY = 0.125f;
            for (int i = 0; i < 3; ++i) field->mouseWheelMove (windowtests::mouseAt (*field, { 20.0f, 18.0f }), wheel);
            const int beginsWhileTurning = w.count.begins;
            ed.reset();
            check (beginsWhileTurning == 1 && w.count.begins == 1 && w.count.ends == 1 && (int) windowtests::plain (p, "manualKey") == 6,
                   "three turns of the wheel on Manual Key are one gesture, closed by the window closing: begins " + std::to_string (w.count.begins) + ", ends " + std::to_string (w.count.ends));
        }
    }

    PluginProcessor p (tempJson ("phase2-window.json"), 0.05);
    prepare (p, 2, 48000.0, 512);
    auto ed = window (p);
    auto& panel = ed->getPanel();

    // key-window-r1-2. Lock holds the key the window showed when it was clicked, not one that arrived since.
    {
        show (p, &eMajor, [] (const PluginProcessor::Display& d) { return d.key == 4; });
        ed->refresh();
        show (p, &aMajor, [] (const PluginProcessor::Display& d) { return d.key == 9; });
        auto* lock = findById<Pill> (panel, "lock");
        lock->setToggleState (true, juce::sendNotificationSync);
        waitFor ([&] { return p.getDisplay().locked; }, 2000);
        check (p.getDisplay().locked && p.getDisplay().key == 4, "Lock clicked while the window shows E major holds E major, held key", p.getDisplay().key);
        setLock (p, false);
        waitFor ([&] { return ! p.getDisplay().locked; }, 2000);
    }

    // key-window-r2-2. Clicking Lock with a key on show never says it is waiting for one.
    {
        show (p, &eMajor, [] (const PluginProcessor::Display& d) { return d.key == 4 && ! d.locked; });
        ed->refresh();
        int waitingSeen = 0;
        for (int i = 0; i < 3; ++i)
        {
            setLock (p, true);
            ed->refresh();
            if (ed->getStatusText() == "Lock waiting for a sure key") ++waitingSeen;
            waitFor ([&] { return p.getDisplay().locked; }, 2000);
            setLock (p, false);
            waitFor ([&] { return ! p.getDisplay().locked; }, 2000);
            ed->refresh();
        }
        check (waitingSeen == 0, "Lock clicked with E major on show: times it said it was waiting for a key, of 3", waitingSeen);
    }

    // key-window-r1-3 and key-window-r3-3. Tempo is black when the beat is heard now, and grey when it is not, whatever the key.
    {
        auto* tempo = findById<Readout> (panel, "tempoValue");
        static Reading lockedQuiet = reading (4, false, 0.8f);
        lockedQuiet.tempoBpm = 120.0f;
        lockedQuiet.signal = false;
        show (p, &lockedQuiet, [] (const PluginProcessor::Display& d) { return d.key == 4; });
        setLock (p, true);
        waitFor ([&] { return p.getDisplay().locked; }, 2000);
        ed->refresh();
        const bool greyWhenSilent = tempo->getColour() == tone::faint;
        setLock (p, false);
        waitFor ([&] { return ! p.getDisplay().locked; }, 2000);

        static Reading beatNoKey;
        beatNoKey.key = -1;
        beatNoKey.signal = true;
        beatNoKey.tempoBpm = 120.0f;
        show (p, &beatNoKey, [] (const PluginProcessor::Display& d) { return d.key < 0 && d.signal; });
        ed->refresh();
        const bool blackWhenHeard = tempo->getColour() == tone::ink;
        check (greyWhenSilent && blackWhenHeard, std::string ("tempo colour: locked with nothing heard ") + (greyWhenSilent ? "grey" : "black")
                                                       + ", a beat heard with no key yet " + (blackWhenHeard ? "black" : "grey"));
    }

    // key-window-r2-4. The tuning shown and its verdict agree: a reading shown as 1 Hz or more from 440 is red, and one shown less is not.
    {
        auto* tuning = findById<Readout> (panel, "tuningValue");
        int disagree = 0;
        std::string example;
        for (float hz : { 439.04f, 438.99f, 440.96f, 441.04f, 439.06f, 439.94f })
        {
            static Reading r;
            r = reading (4, false, 0.8f);
            r.tuningCents = 1200.0f * std::log2 (hz / 440.0f);
            show (p, &r, [hz] (const PluginProcessor::Display& d) { return std::abs (d.referenceHz - hz) < 0.005f; });
            ed->refresh();
            const float shownHz = tuning->getText().fromFirstOccurrenceOf ("A ", false, false).getFloatValue();
            const bool red = tuning->getColour() == tone::red;
            if (red != (std::abs (shownHz - 440.0f) >= 1.0f - 1.0e-4f)) { ++disagree; example = tuning->getText().toStdString() + (red ? " red" : " black"); }
            p.setReadingForTests (nullptr);
            sleepMs (60);
        }
        check (disagree == 0, "the tuning shown and its red agree, disagreements " + std::to_string (disagree) + " " + example);
    }

    // key-window-r2-5. The hint line follows the part under the mouse when that part's hint changes.
    {
        show (p, &eMajor, [] (const PluginProcessor::Display& d) { return d.key == 4 && ! d.unsure; });
        ed->refresh();
        ed->previewHover ("second");
        show (p, &eOrA, [] (const PluginProcessor::Display& d) { return d.unsure; });
        ed->refresh();
        auto* second = findById<Hinted> (panel, "second");
        auto* line = findById<Readout> (panel, "hint");
        check (second != nullptr && line != nullptr && line->getText() == second->hint, "the mouse rests on the runner up as it becomes a close call: the hint says " + line->getText().toStdString());
        ed->previewHover (nullptr);
    }

    // key-window-r3-1. The tails of j, y and g under the big reading are drawn whole.
    {
        auto* hero = findById<HeroView> (panel, "hero");
        int cutRows = 0;
        for (int open = 0; open < 2; ++open)
        {
            ed->setAdvancedOpen (open == 1, false);
            show (p, &eMajor, [] (const PluginProcessor::Display& d) { return d.key == 4 && ! d.locked; });
            ed->refresh();
            cutRows += inkInBottomRows (*hero, 2);
            static Reading none;
            none.key = -1;
            none.signal = true;
            show (p, &none, [] (const PluginProcessor::Display& d) { return d.key < 0 && d.signal; });
            ed->refresh();
            cutRows += inkInBottomRows (*hero, 2);
        }
        ed->setAdvancedOpen (false, false);
        check (cutRows == 0, "'major' and 'no key yet' stop short of the bottom of the reading, Advanced shut and open: ink in its last rows", cutRows, "px");
    }

    // key-window-r2-6. The big reading agrees with the status when nothing is being heard on purpose.
    {
        PluginProcessor q (tempJson ("phase2-paused.json"));
        prepare (q, 2, 48000.0, 512);
        auto qe = window (q);
        auto* hero = findById<HeroView> (qe->getPanel(), "hero");
        setParam (q, "listening", 0.0f);
        sleepMs (60);
        qe->refresh();
        const auto paused = hero->get().scale;
        setParam (q, "listening", 1.0f);
        setParam (q, "bypass", 1.0f);
        sleepMs (60);
        qe->refresh();
        const auto bypassed = hero->get().scale;
        check (paused == "paused" && bypassed == "bypassed", "with no key yet, paused and bypassed say so in the big reading: " + paused.toStdString() + ", " + bypassed.toStdString());
    }

    // key-window-r3-2. A double-click on Manual or on Lock ends where it started, and key.json never carries the step between.
    {
        const auto path = tempJson ("phase2-double.json");
        juce::File (path).deleteFile();
        PluginProcessor q (path, 0.05);
        prepare (q, 2, 48000.0, 512);
        auto qe = window (q);
        auto& qp = qe->getPanel();
        static const Reading eUnsure = reading (4, false, 0.4f);
        show (q, &eUnsure, [] (const PluginProcessor::Display& d) { return d.key == 4; });
        waitFor ([&] { return fileText (path).contains ("\"key\":\"E\""); }, 2000);
        sleepMs (200);
        qe->refresh();

        std::atomic<bool> watching { true };
        std::atomic<int> manualLines { 0 }, lockedLines { 0 };
        std::thread reader ([&]
        {
            while (watching.load())
            {
                const auto t = fileText (path);
                if (t.contains ("\"source\":\"manual\"")) ++manualLines;
                if (t.contains ("\"locked\":true")) ++lockedLines;
                sleepMs (5);
            }
        });

        auto* mode = findById<Segments> (qp, "mode");
        const juce::Point<float> manualSpot { mode->getWidth() * 0.75f, 10.0f };
        mode->mouseDown (mouseAt (*mode, manualSpot, 1));
        sleepMs (120);
        mode->mouseDown (mouseAt (*mode, manualSpot, 2));
        mode->mouseDoubleClick (mouseAt (*mode, manualSpot, 2));
        sleepMs (900);

        auto* lock = findById<Pill> (qp, "lock");
        lock->setToggleState (true, juce::sendNotificationSync);
        sleepMs (120);
        lock->setToggleState (false, juce::sendNotificationSync);
        lock->mouseDoubleClick (mouseAt (*lock, { 5.0f, 5.0f }, 2));
        sleepMs (900);
        watching = false;
        reader.join();
        check (manualLines.load() == 0 && lockedLines.load() == 0 && ! q.getDisplay().manual && ! q.getDisplay().locked,
               "double-clicking Manual and then Lock: reads of key.json that saw a manual or locked line " + std::to_string (manualLines.load()) + ", " + std::to_string (lockedLines.load()));

        // A single click still reaches the file in time.
        const double from = juce::Time::getMillisecondCounterHiRes();
        mode->mouseDown (mouseAt (*mode, manualSpot, 1));
        waitFor ([&] { return fileText (path).contains ("\"source\":\"manual\""); }, 3000);
        const double took = (juce::Time::getMillisecondCounterHiRes() - from) * 0.001;
        check (took <= limits::manualFileSeconds, "one click on Manual reaches key.json within", took, "s");
        setParam (q, "mode", 0.0f);
        q.setReadingForTests (nullptr);
        sleepMs (60);
    }
    p.setReadingForTests (nullptr);
    sleepMs (60);
}

// ---- sessions, presets and a host Reset ----------------------------------------------------------------------------

static void testPhase2Session()
{
    using namespace phase2;

    // key-dsp-r3-4. A session saved with Lock on and waiting, recalled while a guess is on show, waits: it does not hold the guess.
    {
        const auto path = tempJson ("phase2-recall.json");
        juce::File (path).deleteFile();
        static const Reading guess = reading (9, true, 0.3f);
        PluginProcessor p (path, 0.05);
        prepare (p, 2, 48000.0, 512);
        show (p, &guess, [] (const PluginProcessor::Display& d) { return d.key == 9; });
        juce::MemoryBlock waiting;
        baselineFolder().getChildFile ("v1.7.1-lock-waiting.state").loadFileAsData (waiting);
        p.setStateInformation (waiting.getData(), (int) waiting.getSize());
        sleepMs (300);
        const auto d = p.getDisplay();
        check (waiting.getSize() > 0 && d.lockOn && ! d.locked && ! fileText (path).contains ("\"locked\":true"),
               std::string ("a waiting Lock recalled over a 30% guess waits for a sure key: ") + (d.locked ? "held the guess" : "waiting"));
        p.setReadingForTests (nullptr);
        sleepMs (60);
    }

    // The preset last chosen is saved with the session and named again. A new instance and an old session name none.
    {
        PluginProcessor a (tempJson ("phase2-preset.json")), b (tempJson ("phase2-preset.json")), c (tempJson ("phase2-preset.json"));
        const int fresh = a.getCurrentProgram();
        a.setCurrentProgram (2);
        juce::MemoryBlock saved;
        a.getStateInformation (saved);
        b.setStateInformation (saved.getData(), (int) saved.getSize());
        juce::MemoryBlock old;
        baselineFolder().getChildFile ("v1.7.1-preset-vocal-track.state").loadFileAsData (old);
        c.setStateInformation (old.getData(), (int) old.getSize());
        check (fresh == -1 && b.getCurrentProgram() == 2 && c.getCurrentProgram() == -1 && windowtests::plain (b, "source") == 1.0f,
               "the preset chosen comes back with the session (" + std::to_string (b.getCurrentProgram()) + "), a new instance names none ("
                   + std::to_string (fresh) + ") and so does a 1.7.1 session (" + std::to_string (c.getCurrentProgram()) + ")");
    }

    // Sessions saved by the installed 1.7.1 (tests/baseline/capture.mm) load as 1.7.1 loaded them: the same controls, Lock, held key
    // and manual key, and saved again byte for byte as 1.7.1 saved them after loading.
    {
        int same = 0, total = 0;
        std::string differs;
        for (auto& f : baselineFolder().findChildFiles (juce::File::findFiles, false, "*.state"))
        {
            if (f.getFileName().contains (".reloaded.")) continue;
            ++total;
            juce::MemoryBlock data, theirs;
            f.loadFileAsData (data);
            f.getSiblingFile (f.getFileNameWithoutExtension() + ".reloaded.state").loadFileAsData (theirs);
            PluginProcessor p (tempJson ("phase2-old.json"));
            p.setStateInformation (data.getData(), (int) data.getSize());
            prepare (p, 2, 48000.0, 512);
            sleepMs (300);
            juce::MemoryBlock ours;
            p.getStateInformation (ours);
            const auto oursXml = juce::AudioProcessor::getXmlFromBinary (ours.getData(), (int) ours.getSize());
            const auto theirsXml = juce::AudioProcessor::getXmlFromBinary (theirs.getData(), (int) theirs.getSize());
            const bool ok = oursXml != nullptr && theirsXml != nullptr && oursXml->toString() == theirsXml->toString();
            // And what is held: a session with a held key holds it at once, as a locked key.
            const int lockedKey = theirsXml != nullptr ? theirsXml->getIntAttribute ("lockedKey", -1) : -1;
            const auto d = p.getDisplay();
            const bool held = lockedKey < 0 || (d.locked && d.key == lockedKey && d.minor == (theirsXml->getIntAttribute ("lockedScale", 0) == 1));
            if (ok && held) ++same;
            else differs += " " + f.getFileName().toStdString();
        }
        check (total == 7 && same == total, "sessions saved by 1.7.1 load and save again exactly as 1.7.1 did, with their held key held: " + std::to_string (same) + " of " + std::to_string (total) + differs);
    }

    // A host Reset (a stop, a locate, the start of a bounce) drops the audio not analysed yet, so what follows is heard on its own.
    // It keeps the reading, Lock and key.json: the key of a song does not change because the host jumped in it.
    {
        const double sr = 48000.0;
        const auto path = tempJson ("phase2-reset.json");
        juce::File (path).deleteFile();
        PluginProcessor p (path, 0.05);
        prepare (p, 2, sr, 512);
        setParam (p, "mode", 1.0f);
        setParam (p, "manualKey", 5.0f);
        waitFor ([&] { return fileText (path).contains ("\"key\":\"F\""); }, 2000);
        setParam (p, "mode", 0.0f);
        std::vector<float> tone ((size_t) sr);
        sine (tone.data(), (int) tone.size(), 440.0, sr, 0.5);
        feed (p, tone, sr, 1.0);
        waitFor ([&] { return p.getDisplay().signal; }, 2000);
        const bool heardTone = p.getDisplay().signal;
        const auto before = fileText (path);
        const long allocsBefore = testkit::allocs.load();
        p.reset();
        const long allocsInReset = testkit::allocs.load() - allocsBefore;
        feed (p, std::vector<float> ((size_t) (phase2limits::hostResetSignalSeconds * sr), 0.0f), sr, 1.0);
        sleepMs (150);
        const bool stillTone = p.getDisplay().signal;
        check (heardTone && ! stillTone, std::string ("after a host Reset, half a second of silence is heard as silence: ") + (stillTone ? "still hears the tone from before" : "silence"));
        check (allocsInReset == 0, "a host Reset allocates nothing, allocations", (double) allocsInReset);
        check (fileText (path) == before, "a host Reset leaves key.json as it was");
    }
}

static bool runPhase2Tests (int argc, char** argv)
{
    const bool only = argc > 1 && juce::String (argv[1]) == "--phase2";
    if (! only)
    {
        testPhase2Analyser();
        testPhase2File();
        testPhase2Readings();
        testPhase2Window();
        testPhase2Session();
        return false;
    }
    // PluginTest --phase2 [analyser|file|readings|window|session] runs only these, or one group of them.
    const juce::String group = argc > 2 ? juce::String (argv[2]) : juce::String();
    if (group.isEmpty() || group == "analyser") testPhase2Analyser();
    if (group.isEmpty() || group == "file") testPhase2File();
    if (group.isEmpty() || group == "readings") testPhase2Readings();
    if (group.isEmpty() || group == "window") testPhase2Window();
    if (group.isEmpty() || group == "session") testPhase2Session();
    return true;
}
