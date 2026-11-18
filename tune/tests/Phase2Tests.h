#pragma once
// The faults the Phase 2 hunt kept (Bug Fixing/smooth-plugins/fix/tune.json, key-readers.md and HostStress paramtext), each written and run
// on 1.2.1 before any fix. Included at the end of PluginTest.cpp, which holds the helpers. Run: PluginTest phase2 (or a full run).
// Limits: phase2limits in PassLimits.h, written before these were run, and the suite's own.

namespace phase2
{
using namespace testkit;
using namespace measure;

inline std::vector<float> baselineInput()
{
    juce::MemoryBlock block;
    juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getChildFile ("baseline/input.f32").loadFileAsData (block);
    const auto* in = static_cast<const float*> (block.getData());
    return std::vector<float> (in, in + block.getSize() / sizeof (float));
}

inline juce::MemoryBlock baselineState (const juce::String& name)
{
    juce::MemoryBlock block;
    juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getChildFile ("baseline/v1.2.1-" + name + ".state").loadFileAsData (block);
    return block;
}

// tune-r1-1: a host Reset clears what prepareToPlay clears, allocates nothing, and the one Reset JUCE's AU wrapper sends right after
// the host turns its own bypass on is left out, so Bypass still fades instead of cutting (the way Velvet Rope 0.8.1, Duo 0.4.3 and
// Punch 0.6.2 do it).
inline void testHostReset()
{
    const auto vowel = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 0.5, SR), voice::vowelA, SR);
    {
        auto p = make();
        prepare (*p, 2, SR, 512);
        render (*p, vowel, 512);
        p->reset();
        const auto quiet = render (*p, std::vector<float> ((size_t) (0.05 * SR), 0.0f), 512);
        const double worst = peak (quiet.getReadPointer (0), quiet.getNumSamples());
        check (worst == 0.0, "Phase 2: a vowel, a host Reset, then 50 ms of silence: peak of the old vowel left in the output", toDb (worst), "dB");
    }
    {
        const auto x = voice::synth (voice::phrase ({ 200.0, 240.0, 300.0 }, 0.4, SR), voice::vowelA, SR, -30.0);
        auto p = make(), fresh = make();
        prepare (*p, 2, SR, 512);
        prepare (*fresh, 2, SR, 512);
        render (*p, vowel, 512);
        p->reset();
        const auto a = render (*p, x, 512), b = render (*fresh, x, 512);
        long differ = 0;
        for (int i = 0; i < a.getNumSamples(); ++i) differ += a.getSample (0, i) != b.getSample (0, i) ? 1 : 0;
        check (differ == 0, "Phase 2: play, reset, play: the second part equals a new instance; samples that differ", (double) differ);
        const long before = testkit::allocs.load();
        p->reset();
        const long made = testkit::allocs.load() - before;
        check (made == 0, "Phase 2: a host Reset allocates nothing", (double) made);
    }
    {
        auto q = make();
        q->reset();
        prepare (*q, 2, SR, 512);
        const auto y = render (*q, vowel, 512);
        check (countNonFinite (y) == 0 && peak (y.getReadPointer (0), y.getNumSamples()) > 0.01, "Phase 2: a Reset before the first prepare is harmless");
    }
    {
        // Two instances; the host turns Bypass on in both at 0.25 s, and in one JUCE's AU wrapper then sends its Reset.
        auto a = make(), b = make();
        prepare (*a, 2, SR, 512);
        prepare (*b, 2, SR, 512);
        const int at = (int) std::lround (0.25 * SR / 512.0);
        const auto ya = render (*a, vowel, 512, nullptr, [&] (int block) { if (block == at) setParam (*a, "bypass", 1.0f); });
        const auto yb = render (*b, vowel, 512, nullptr, [&] (int block) { if (block == at) { setParam (*b, "bypass", 1.0f); b->reset(); } });
        double worst = 0.0;
        for (int i = 0; i < ya.getNumSamples(); ++i) worst = std::max (worst, (double) std::abs (ya.getSample (0, i) - yb.getSample (0, i)));
        check (worst == 0.0, "Phase 2: the Reset that comes with host bypass switching on changes nothing; largest difference", worst);
        // Later on, while bypassed, a Reset is a real one: silence in gives silence out at once.
        b->reset();
        const auto quiet = render (*b, std::vector<float> (2048, 0.0f), 512);
        check (peak (quiet.getReadPointer (0), quiet.getNumSamples()) == 0.0, "Phase 2: a Reset later on while bypassed is not left out");
    }
}

// tune-r1-2: Transpose typed with the plus sign the window shows it with.
inline void testTransposeTyping()
{
    auto p = make();
    prepare (*p, 2, SR, 256);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* transpose = dynamic_cast<ui::BarSlider*> (ed->findChildWithID ("transpose"));
    if (transpose == nullptr) { check (false, "Phase 2: the Transpose bar is found"); return; }
    setParam (*p, "transpose", 3.0f);
    const auto shown = transpose->valueText();
    transpose->applyTyped (shown.replace ("3", "5"));
    check (std::abs (realValue (*p, "transpose") - 5.0f) < 0.01f, "Phase 2: Transpose at " + shown.toStdString() + ", the 3 typed over with a 5: it reads", realValue (*p, "transpose"), "st");
    auto* prm = p->apvts.getParameter ("transpose");
    check (std::abs (prm->getValueForText ("+7") - prm->convertTo0to1 (7.0f)) < 1.0e-6f, "Phase 2: a host that types +7 into Transpose gets +7", prm->convertFrom0to1 (prm->getValueForText ("+7")), "st");
    check (std::abs (prm->getValueForText ("-12") - prm->convertTo0to1 (-12.0f)) < 1.0e-6f && std::abs (prm->getValueForText ("4 st") - prm->convertTo0to1 (4.0f)) < 1.0e-6f,
           "Phase 2: -12 and 4 st still read as they did");
}

// tune-r2-1, and the extreme Transpose half of it: where the readings put the output, against where it is.
inline void testFarFromTarget()
{
    // Only C on Tune, a steady E3 sung: the output sits on C3, 400 cents down.
    auto p = make();
    setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f);
    onlyNote (*p, 0);
    prepare (*p, 2, SR, 256);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* te = dynamic_cast<TuneEditor*> (ed.get());
    const auto x = voice::synth (voice::steady (noteHz (52), 1.0, SR), voice::vowelA, SR);
    const auto out = mono (render (*p, x, 256));
    const auto r = p->getReadings();
    const double sits = r.centsOffTarget + r.correctionCents;
    const double heard = median (centsOff (out.data(), (long) out.size(), SR, noteHz (48), 0.4, 0.9));
    check (std::abs (heard) <= limits::outputInstantCents, "Phase 2: only C on Tune, E3 sung: the output is on C3, off by", heard, "cents");
    check (std::abs (sits) <= phase2limits::outputSitsCents, "Phase 2:   and the readings put it (Pitch Off Target + Correction Applied) off C by", sits, "cents");
    if (te != nullptr)
    {
        te->tick (0.2);
        auto& d = te->getDisplay();
        check (std::abs (d.outMarkCents()) <= phase2limits::outputSitsCents, "Phase 2:   the window's lime mark sits off the note by", d.outMarkCents(), "cents");
        check (d.isNoteLit(), "Phase 2:   the note turns lime: " + d.shownText().toStdString());
        check (d.shownText().contains ("400 cents sharp"), "Phase 2:   and Sung says 400 cents sharp: " + d.shownText().toStdString());
    }

    // Transpose +24 and a note sung 30 cents flat: Correction Applied is 2430 cents.
    auto q = make();
    setParam (*q, "retune", 0.0f); setParam (*q, "humanize", 0.0f); setParam (*q, "transpose", 24.0f);
    prepare (*q, 2, SR, 256);
    render (*q, voice::synth (voice::steady (220.0 * std::pow (2.0, -30.0 / 1200.0), 1.0, SR), voice::vowelA, SR), 256);
    const auto rq = q->getReadings();
    const double sitsQ = rq.centsOffTarget + rq.correctionCents - 2400.0;
    check (std::abs (sitsQ) <= phase2limits::outputSitsCents, "Phase 2: Transpose +24, A3 sung 30 cents flat: the readings put the output off A5 by", sitsQ, "cents");
}

// tune-r3-1: the corrector's correction on top of Transpose +/-24 is not cut off by the shifter.
inline void testTransposeExtremes()
{
    const auto run = [] (float transpose, double sungHz, double wantHz)
    {
        auto p = make();
        setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "transpose", transpose);
        prepare (*p, 1, SR, 256);
        const auto x = voice::synth (voice::steady (sungHz, 1.5, SR), voice::vowelA, SR);
        const auto out = mono (render (*p, x, 256));
        return median (centsOff (out.data(), (long) out.size(), SR, wantHz, 0.4, 1.4));
    };
    const double up = run (24.0f, 220.0 * std::pow (2.0, -30.0 / 1200.0), 880.0);
    check (std::abs (up) <= limits::transposeCents, "Phase 2: Transpose +24, A3 sung 30 cents flat comes out on A5, offset", up, "cents");
    const double down = run (-24.0f, 440.0 * std::pow (2.0, 30.0 / 1200.0), 110.0);
    check (std::abs (down) <= limits::transposeCents, "Phase 2: Transpose -24, A4 sung 30 cents sharp comes out on A2, offset", down, "cents");
}

// tune-r1-3: the chosen preset is saved; a 1.2.1 session names none, and plays as 1.2.1 played it.
inline void testPresetSaved()
{
    const int hard = 5;
    {
        auto p = make();
        p->setCurrentProgram (hard);
        juce::MemoryBlock saved;
        p->getStateInformation (saved);
        auto q = make();
        q->setStateInformation (saved.getData(), (int) saved.getSize());
        check (q->getCurrentProgram() == hard && ! q->isCurrentProgramEdited(), "Phase 2: a session saved after choosing Hard Tune names it on reload, unedited (program)", q->getCurrentProgram());
        std::unique_ptr<juce::AudioProcessorEditor> ed (q->createEditor());
        auto* te = dynamic_cast<TuneEditor*> (ed.get());
        check (te != nullptr && te->presetText() == "Hard Tune", "Phase 2:   and the preset box says so: " + (te != nullptr ? te->presetText().toStdString() : std::string()));

        // Its values are in the state already and are not applied again: a setting moved after choosing it stays moved.
        setParam (*p, "retune", 50.0f);
        p->getStateInformation (saved);
        auto r = make();
        r->setStateInformation (saved.getData(), (int) saved.getSize());
        check (r->getCurrentProgram() == hard && std::abs (realValue (*r, "retune") - 50.0f) < 0.05f && r->isCurrentProgramEdited(),
               "Phase 2: Hard Tune with Retune moved to 50 ms reloads as Hard Tune, edited, at 50 ms", realValue (*r, "retune"), "ms");

        auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        if (xml != nullptr)
        {
            xml->setAttribute ("program", 999);
            juce::MemoryBlock bad;
            juce::AudioProcessor::copyXmlToBinary (*xml, bad);
            auto s = make();
            s->setStateInformation (bad.getData(), (int) bad.getSize());
            check (juce::isPositiveAndBelow (s->getCurrentProgram(), s->getNumPrograms()), "Phase 2: a damaged preset number comes back in range", s->getCurrentProgram());
            check (! s->apvts.state.hasProperty ("program"), "Phase 2:   and stays out of the parameter tree");
        }
    }
    {
        auto old = make();
        const auto st = baselineState ("preset5");
        old->setStateInformation (st.getData(), (int) st.getSize());
        check (old->getCurrentProgram() < 0, "Phase 2: a Hard Tune session saved by 1.2.1 names no preset (program)", old->getCurrentProgram());
        std::unique_ptr<juce::AudioProcessorEditor> ed (old->createEditor());
        auto* te = dynamic_cast<TuneEditor*> (ed.get());
        const auto shown = te != nullptr ? te->presetText() : juce::String();
        check (shown == "Choose a preset", "Phase 2:   and the preset box says so: " + shown.toStdString());
        auto* box = ed->findChildWithID ("presets");
        if (box != nullptr)
        {
            box->mouseDown (mouseAt (*box, { (float) box->getWidth() - 5.0f, 5.0f }));
            const int afterNext = old->getCurrentProgram();
            old->setStateInformation (st.getData(), (int) st.getSize());
            box->mouseDown (mouseAt (*box, { 5.0f, 5.0f }));
            const int afterPrevious = old->getCurrentProgram();
            check (afterNext == 0 && afterPrevious == old->getNumPrograms() - 1, "Phase 2:   from no preset the next arrow goes to the first preset and the previous one to the last: next",
                   afterNext);
        }

        auto fresh = make();
        const auto def = baselineState ("default");
        fresh->setStateInformation (def.getData(), (int) def.getSize());
        std::unique_ptr<juce::AudioProcessorEditor> ed2 (fresh->createEditor());
        auto* te2 = dynamic_cast<TuneEditor*> (ed2.get());
        check (te2 != nullptr && te2->presetText() == "Default", "Phase 2: 1.2.1's default session still reads Default: " + (te2 != nullptr ? te2->presetText().toStdString() : std::string()));
    }
    // The installed 1.2.1's own sessions (tests/baseline, captured before 1.2.2 was installed): each loads and plays as 1.2.1 played it.
    {
        const auto x = baselineInput();
        check (x.size() == 288000, "Phase 2: the 1.2.1 baseline input is there", (double) x.size());
        int same = 0, cases = 0;
        for (int preset = -1; preset < 8; ++preset)
        {
            const juce::String name = preset < 0 ? juce::String ("default") : "preset" + juce::String (preset);
            const auto st = baselineState (name);
            juce::MemoryBlock then;
            juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getChildFile ("baseline/v1.2.1-" + name + ".f32").loadFileAsData (then);
            auto p = make();
            p->setStateInformation (st.getData(), (int) st.getSize());
            prepare (*p, 2, SR, 512);
            const auto out = render (*p, x, 512);
            const auto* old = static_cast<const float*> (then.getData());
            double worst = 0.0;
            for (int i = 0; i < out.getNumSamples() && (size_t) i < then.getSize() / 4; ++i) worst = std::max (worst, (double) std::abs (out.getSample (0, i) - old[i]));
            ++cases;
            same += worst == 0.0 && then.getSize() == x.size() * 4 ? 1 : 0;
            if (worst != 0.0) info ("Phase 2:   1.2.1's " + name.toStdString() + " session differs, largest sample difference (x 1e9)", worst * 1.0e9);
        }
        check (same == cases && cases == 9, "Phase 2: the installed 1.2.1's own sessions (default and 8 presets) play bit for bit as 1.2.1 played them, cases", (double) same);
    }
}

// tune-r1-4: Custom notes on Bypass are left as sung, whatever Natural Vibrato is.
inline void testBypassedNotesKeepVibrato()
{
    auto p = make();
    setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "naturalVibrato", -100.0f);
    setParam (*p, "scale", (float) smoothtune::scaleCustom);
    for (int i = 0; i < 12; ++i)
        setParam (*p, ("note" + juce::String (i)).toRawUTF8(), 2.0f);
    const double kept = vibratoKept (*p);
    check (kept >= phase2limits::bypassedNoteVibratoKept, "Phase 2: every Custom note on Bypass, Natural Vibrato -100 (Robot Flat's): +/-40 cent vibrato kept", kept * 100.0, "%");

    prepare (*p, 1, SR, 256);
    const auto x = voice::synth (voice::vibrato (220.0, 2.0, SR, 40.0, 5.5), voice::vowelA, SR);
    const auto out = mono (render (*p, x, 256));
    const double db = nullDb (out, x, p->getLatencySamples(), (long) (0.5 * SR), (long) (1.9 * SR));
    check (db <= rodelimits::uncorrectedNullDb, "Phase 2:   and the output is the input at the reported delay, difference", db, "dB");
}

// tune-r1-5: a state loaded while the window is open brings Advanced's drawer and the window height with it.
inline void testAdvancedAfterLoad()
{
    auto p = make();
    prepare (*p, 2, SR, 256);
    juce::MemoryBlock withOpen, withClosed;
    p->advancedOpen.store (true);
    p->getStateInformation (withOpen);
    p->advancedOpen.store (false);
    p->getStateInformation (withClosed);

    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* te = dynamic_cast<TuneEditor*> (ed.get());
    auto* drawer = ed->findChildWithID ("transpose");
    if (te == nullptr || drawer == nullptr) { check (false, "Phase 2: the window and its drawer are found"); return; }
    const int closedHeight = ed->getHeight();
    p->setStateInformation (withOpen.getData(), (int) withOpen.getSize());
    te->tick (1.0 / 30.0);
    check (drawer->isVisible() == te->isAdvancedOpen() && te->isAdvancedOpen() && ed->getHeight() == closedHeight + TuneEditor::drawerHeight,
           "Phase 2: a state saved with Advanced open, loaded with the window open and Advanced closed: the drawer shows, and the window grows by", (double) (ed->getHeight() - closedHeight), "pt");
    p->setStateInformation (withClosed.getData(), (int) withClosed.getSize());
    te->tick (1.0 / 30.0);
    check (drawer->isVisible() == te->isAdvancedOpen() && ! te->isAdvancedOpen() && ed->getHeight() == closedHeight,
           "Phase 2:   and back: the drawer hides, height off the first by", (double) (ed->getHeight() - closedHeight), "pt");
    te->setAdvancedOpen (true);
    check (drawer->isVisible() && ed->getHeight() == closedHeight + TuneEditor::drawerHeight, "Phase 2:   then one click on Advanced opens it");
}

// tune-r2-2: with Follow Key on and nothing from the key file, the window shows and builds from the Key and Scale chosen, also when the
// host is not processing.
inline void testFollowKeyWindow()
{
    auto p = make ("/nonexistent/smooth-tune-phase2.json");
    setParam (*p, "followKey", 1.0f);
    setParam (*p, "key", 0.0f);
    setParam (*p, "scale", (float) smoothtune::scaleMajor);
    prepare (*p, 2, SR, 256);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* te = dynamic_cast<TuneEditor*> (ed.get());
    if (te == nullptr) { check (false, "Phase 2: the window is found"); return; }
    render (*p, std::vector<float> (256, 0.0f), 256);   // one block in C major, then the host stops
    setParam (*p, "key", 2.0f);
    te->tick (1.0 / 30.0);
    check (te->keyShown() == "D", "Phase 2: Follow Key on, no key file, D chosen with the host stopped: the Key field says " + te->keyShown().toStdString());
    te->clickNote (6);
    const auto dMajorWithoutFSharp = smoothtune::scaleMask (2, smoothtune::scaleMajor) & ~(1u << 6);
    int wrong = 0;
    for (int i = 0; i < 12; ++i)
        wrong += juce::roundToInt (realValue (*p, ("note" + juce::String (i)).toRawUTF8())) != (((dMajorWithoutFSharp >> i) & 1) ? 0 : 1) ? 1 : 0;
    check (wrong == 0, "Phase 2:   then F# clicked: the Custom notes are D major without F#; notes wrong", (double) wrong);

    // With a good key file, the window shows the key it holds before any audio has run.
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("smooth-tune-phase2-window-" + juce::String (getpid()) + ".json");
    writeKey (file, keyJson ("E", "minor", 0.9, nowUnix()));
    auto q = make (file.getFullPathName());
    setParam (*q, "followKey", 1.0f);
    q->pollKeyFileNow();
    std::unique_ptr<juce::AudioProcessorEditor> ed2 (q->createEditor());
    auto* te2 = dynamic_cast<TuneEditor*> (ed2.get());
    te2->tick (1.0 / 30.0);
    check (te2->keyShown() == "E" && te2->scaleShown() == "Minor", "Phase 2: Follow Key on, E minor in the key file, before any audio: the window says " + te2->keyShown().toStdString() + " " + te2->scaleShown().toStdString());
    file.deleteFile();
}

// tune-r2-3 and tune-r2-4: what the window says fits where it is drawn.
inline void testWindowTextFits()
{
    auto p = make();
    prepare (*p, 2, SR, 256);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* te = dynamic_cast<TuneEditor*> (ed.get());
    if (te == nullptr) { check (false, "Phase 2: the window is found"); return; }
    te->setAdvancedOpen (false);
    ed->setSize (PluginProcessor::minWindowWidth, PluginProcessor::minWindowHeight);
    const auto summaryFont = ui::font (ui::Weight::regular, 13.0f);

    struct Moves { const char* what; float transpose, reference, tracking, throat; bool formant; int moved; };
    const Moves moves[] = { { "Transpose -12 and Formant Preserve off", -12.0f, 440.0f, 50.0f, 100.0f, false, 2 },
                            { "all five moved", 3.0f, 432.0f, 75.0f, 110.0f, false, 5 } };
    for (const auto& m : moves)
    {
        setParam (*p, "transpose", m.transpose); setParam (*p, "reference", m.reference); setParam (*p, "tracking", m.tracking);
        setParam (*p, "throat", m.throat); setParam (*p, "formantPreserve", m.formant ? 1.0f : 0.0f);
        for (int i = 0; i < 10; ++i) te->tick (1.0 / 30.0);
        const auto shown = te->advancedSummaryShown();
        const float width = juce::GlyphArrangement::getStringWidth (summaryFont, shown);
        // Every moved setting is named, or counted in "+N more".
        int named = 0;
        for (const char* name : { "Transpose", "Reference", "Tracking", "Throat", "Formant" })
            named += shown.contains (name) ? 1 : 0;
        const int counted = shown.fromLastOccurrenceOf ("+", false, false).upToFirstOccurrenceOf (" more", false, false).getIntValue();
        check (width <= te->advancedSummaryRoom() && named + (shown.contains (" more") ? counted : 0) == m.moved,
               std::string ("Phase 2: smallest window, Advanced closed, ") + m.what + ": the summary '" + shown.toStdString() + "' fits its room, width over room",
               (double) (width - te->advancedSummaryRoom()), "pt");
    }
    for (const char* id : { "transpose", "reference", "tracking", "throat" })
        p->apvts.getParameter (id)->setValueNotifyingHost (p->apvts.getParameter (id)->getDefaultValue());
    setParam (*p, "formantPreserve", 1.0f);

    // The two readouts beside the note, the widest each can say: the highest pitch Soprano follows, and a one-note scale's correction.
    const juce::String widest[] = { "1300.0 Hz", juce::String::fromUTF8 ("\xe2\x88\x92") + "600 cents" };
    float worstOver = -1.0e9f;
    juce::String worstAt;
    for (auto size : { juce::Point<int> (840, 1200), juce::Point<int> (840, 900), juce::Point<int> (900, 1200), juce::Point<int> (840, 580), juce::Point<int> (900, 660), juce::Point<int> (1600, 1200) })
    {
        ed->setSize (size.x, size.y);
        auto& d = te->getDisplay();
        const auto valueFont = ui::font (ui::Weight::medium, d.readoutValueSize(), true);
        for (const auto& text : widest)
        {
            const float over = juce::GlyphArrangement::getStringWidth (valueFont, text) - (float) d.readoutRoom();
            if (over > worstOver) { worstOver = over; worstAt = juce::String (size.x) + "x" + juce::String (size.y) + " '" + text + "'"; }
        }
    }
    check (worstOver <= 0.0f, "Phase 2: the Sung and Correction readouts fit beside the note at every size, worst " + worstAt.toStdString() + ", width over room", worstOver, "pt");
}

// tune-r3-2: the end of a corrected vowel going straight into an S is corrected to its end, not played as sung.
struct VowelTail
{
    double asSungMs = 0.0;       // the vowel's last stretch that comes out exactly as sung
    double nearerCorrectedDb = 0.0;   // over the stretch 1.2.2 played as sung: how much nearer the corrected vowel going on the output is than the vowel as sung
    double intoTheSMs = 0.0;     // how far past the vowel's end the output is not yet the S as sung
};

inline VowelTail vowelTail (int type, double hz)
{
    // One vowel, 40 cents sharp, synthesised 0.8 s long. Cut at 0.6 s straight into an S, and the same vowel going on: up to the cut both
    // have the same input, so the second says what the first's vowel should come out as.
    const auto whole = voice::synth (voice::steady (hz * std::pow (2.0, 40.0 / 1200.0), 0.8, SR), voice::vowelA, SR);
    const size_t cut = (size_t) (0.6 * SR);
    const std::vector<float> v (whole.begin(), whole.begin() + (long) cut);
    const auto s = coloured ((int) (0.2 * SR), 0.08, 4000, 10000, 5);
    std::vector<float> x = v;
    x.insert (x.end(), s.begin(), s.end());
    x.insert (x.end(), v.begin(), v.end());
    auto p = tuner (type, 0.0f);
    const auto out = mono (render (*p, x, 256));
    auto q = tuner (type, 0.0f);
    const auto on = mono (render (*q, whole, 256));
    const long delay = p->getLatencySamples();
    const long end = (long) cut - 1 + delay;   // where the vowel's last sample comes out

    VowelTail r;
    // Walk back from there while the output is the input at the reported delay, exactly.
    long same = 0;
    while (end - same - delay > 0 && std::abs (out[(size_t) (end - same)] - x[(size_t) (end - same - delay)]) <= 1.0e-6f)
        ++same;
    r.asSungMs = 1000.0 * (double) same / SR;

    // The stretch 1.2.2 left as sung in Low Male, 7.1 ms: the output against the corrected vowel going on, and against the vowel as sung.
    const long from = end + 1 - (long) std::lround (0.0071 * SR);
    double toCorrected = 0.0, toSung = 0.0;
    for (long i = from; i <= end; ++i)
    {
        toCorrected += std::pow ((double) out[(size_t) i] - on[(size_t) i], 2.0);
        toSung += std::pow ((double) out[(size_t) i] - x[(size_t) (i - delay)], 2.0);
    }
    r.nearerCorrectedDb = 10.0 * std::log10 ((toSung + 1.0e-30) / (toCorrected + 1.0e-30));

    // The S: the last sample before the next vowel where the output is not the input at the delay, after the vowel's end.
    long last = end;
    for (long i = end + 1; i < end + (long) s.size(); ++i)
        if (std::abs (out[(size_t) i] - x[(size_t) (i - delay)]) > 1.0e-6f)
            last = i;
    r.intoTheSMs = 1000.0 * (double) (last - end) / SR;
    return r;
}

inline void testVowelTail()
{
    const struct { int type; double hz; const char* name; } cases[] = { { lowMale, 110.0, "Low Male, 110 Hz" }, { wideRange, 110.0, "Wide Range, 110 Hz" }, { altoTenor, 220.0, "Alto/Tenor, 220 Hz" } };
    for (const auto& c : cases)
    {
        const auto t = vowelTail (c.type, c.hz);
        // Up to 1.2.2 an INFO line (NOT FIXED, Luke's call): 7.1 ms in Low Male and Wide Range, 0.5 ms in Alto/Tenor, against the limit written
        // for it before it was first run. Made a check on 2026-11-14 (round 5; Luke: "do those 4 then"), before the fix. Fixed in 1.2.3: in
        // Low Male and Wide Range, where a vowel at its own level is cut off by a loud, bright consonant, the grains read its marks to the
        // last one with the correction it had (PsolaShifter::carried, cutOffHere). Alto/Tenor is left as it was.
        check (t.asSungMs <= phase2limits::tailUncorrectedMs, std::string ("Phase 2: ") + c.name + ", 40 cents sharp straight into an S, the vowel's end played as sung", t.asSungMs, "ms");
        // Not by passing the input on some other way: over the stretch 1.2.2 played as sung, the output is nearer the same vowel going on,
        // corrected, than the vowel as sung (1.2.2: -309 dB in Low Male, it was the vowel as sung).
        check (t.nearerCorrectedDb > phase2limits::tailNearerCorrectedDb, std::string ("Phase 2: ") + c.name + ", the vowel's last 7.1 ms nearer the corrected vowel going on than the vowel as sung by", t.nearerCorrectedDb, "dB");
        // And the vowel is not carried on over the S: the S comes out as sung from one pass crossfade (5 ms) after the vowel's end at most.
        check (t.intoTheSMs <= phase2limits::tailIntoTheSMs, std::string ("Phase 2: ") + c.name + ", the S after it comes out as sung from this far past the vowel's end", t.intoTheSMs, "ms");
    }
}

// tune-r3-4: under Bypass, Correction Applied says nothing is applied.
inline void testBypassReadings()
{
    const auto x = voice::synth (voice::steady (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.0, SR), voice::vowelA, SR);
    auto p = tuner (altoTenor, 0.0f);
    setParam (*p, "bypass", 1.0f);
    render (*p, x, 256);
    juce::String text;
    for (auto* prm : p->getParameters())
        if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            if (w->paramID == "correctionApplied")
                text = prm->getCurrentValueAsText();
    check (p->getReadings().correctionCents == 0.0f && text == "0 cents", "Phase 2: Bypass on, a voice 30 cents sharp: Correction Applied reads " + text.toStdString(), p->getReadings().correctionCents, "cents");

    auto q = tuner (altoTenor, 0.0f);
    juce::AudioBuffer<float> buffer (1, 256);
    juce::MidiBuffer midi;
    for (int pos = 0; pos + 256 <= (int) x.size(); pos += 256)
    {
        buffer.copyFrom (0, 0, x.data() + pos, 256);
        q->processBlockBypassed (buffer, midi);
    }
    check (q->getReadings().correctionCents == 0.0f, "Phase 2: the host's own bypass, the same voice: Correction Applied", q->getReadings().correctionCents, "cents");
}

// HostStress paramtext: every reading's text reads back as the value it shows (Key 1.7.2 did the same).
inline void testReadingText()
{
    auto p = make();
    int wrong = 0, tried = 0;
    juce::String example;
    for (auto* prm : p->getParameters())
    {
        if (dynamic_cast<ReadingParameter*> (prm) == nullptr)
            continue;
        for (int k = 0; k <= 5; ++k)
        {
            const float v = (float) k / 5.0f;
            const auto text = prm->getText (v, 64);
            const auto back = prm->getText (prm->getValueForText (text), 64);
            ++tried;
            if (back != text)
            {
                ++wrong;
                if (example.isEmpty()) example = prm->getName (64) + ": \"" + text + "\" reads back as \"" + back + "\"";
            }
        }
    }
    check (wrong == 0 && tried == 30, "Phase 2: every reading's text reads back as the value it shows, at 6 values each; wrong" + (example.isEmpty() ? std::string() : " (e.g. " + example.toStdString() + ")"), (double) wrong);
}

// key-file-link-r2-1: an offline bounce takes up a Follow Key change at the same song time as playback does, not after a wall clock
// wait. And key-file-link-r1-1 and the writer change from Key 1.7.2 (key-readers.md): a fresh Manual key, written with listening false,
// is followed as a Locked one is, and a closed Key's last line, which keeps its old time, goes stale as any old line does.
inline void testKeyFileReaders()
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("smooth-tune-phase2-bounce-" + juce::String (getpid()) + ".json");
    writeKey (file, keyJson ("C", "major", 0.9, nowUnix()));
    auto p = make (file.getFullPathName());
    setParam (*p, "retune", 0.0f); setParam (*p, "humanize", 0.0f); setParam (*p, "followKey", 1.0f);
    prepare (*p, 1, SR, 512);
    p->pollKeyFileNow();
    p->setNonRealtime (true);
    const auto x = voice::synth (voice::steady (noteHz (66) * std::pow (2.0, 20.0 / 1200.0), 6.0, SR), voice::vowelA, SR);
    const int changeBlock = (int) std::lround (1.0 * SR / 512.0);
    int seenAt = -1;
    render (*p, x, 512, nullptr, [&] (int block)
    {
        if (block == changeBlock)
            writeKey (file, keyJson ("D", "major", 0.9, nowUnix()));
        if (seenAt < 0 && block > changeBlock && p->getReadings().activeKey == 2)
            seenAt = block;
    });
    const double lag = seenAt < 0 ? 5.0 : (double) (seenAt - changeBlock) * 512.0 / SR;
    check (seenAt >= 0 && lag <= phase2limits::bounceKeySeconds, "Phase 2: an offline bounce as fast as it goes takes up a Follow Key change from C to D major within, in song time (5 = not in the 5 s rendered)", lag, "s");
    p->setNonRealtime (false);

    const auto line = [] (const char* key, const char* scale, double time, bool listening, const char* source)
    {
        char b[320];
        std::snprintf (b, sizeof b, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":1.00,\"time\":%.1f,\"listening\":%s,\"altKey\":null,\"altScale\":null,"
                                    "\"altConfidence\":0.00,\"margin\":1.00,\"unsure\":false,\"locked\":false,\"source\":\"%s\"}", key, scale, time, listening ? "true" : "false", source);
        return std::string (b);
    };
    auto q = make (file.getFullPathName());
    setParam (*q, "followKey", 1.0f);
    writeKey (file, line ("A", "minor", nowUnix() - 200.0, false, "manual"));
    q->pollKeyFileNow();
    prepare (*q, 1, SR, 256);
    render (*q, std::vector<float> (256, 0.0f), 256);
    check (q->getReadings().activeKey == 9 && q->getReadings().activeScale == smoothtune::scaleMinor, "Phase 2: a Manual key from Key 1.7.2 (listening false, written 200 s ago) is followed");
    auto r = make (file.getFullPathName());
    setParam (*r, "followKey", 1.0f);
    writeKey (file, line ("E", "major", nowUnix() - 660.0, false, "heard"));
    r->pollKeyFileNow();
    prepare (*r, 1, SR, 256);
    render (*r, std::vector<float> (256, 0.0f), 256);
    check (r->getReadings().activeKey == 0 && r->getReadings().activeScale == smoothtune::scaleChromatic, "Phase 2: a closed Key's last line, 11 minutes old, is not taken up");
    file.deleteFile();
}

inline void run()
{
    testHostReset();
    testTransposeTyping();
    testFarFromTarget();
    testTransposeExtremes();
    testPresetSaved();
    testBypassedNotesKeepVibrato();
    testAdvancedAfterLoad();
    testFollowKeyWindow();
    testWindowTextFits();
    testVowelTail();
    testBypassReadings();
    testReadingText();
    testKeyFileReaders();
}
} // namespace phase2
