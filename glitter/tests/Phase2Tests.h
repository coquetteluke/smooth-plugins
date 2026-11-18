// Phase 2 (2026-11-13): one check for each bug the hunt kept, for HostStress's paramtext FAILs, and for the key file items, each
// written to fail on 1.2.4 for the reason its bug gives (or, where marked, a guard that passes on both). Run: PluginTest phase2.
// The numbers on 1.2.4 and on 1.2.5 are in the fix report (~/Bug Fixing/smooth-plugins/fix/tune-fx-REPORT.md).
// Included inside PluginTest.cpp's own namespace, after its helpers.

// Renders mono through p, with the first block a different size from the rest (a cycle wrap or a partial slice does that in a host).
std::vector<float> renderSized (PluginProcessor& p, const std::vector<float>& x, int first, int block, const std::function<void (int)>& before = nullptr)
{
    std::vector<float> out (x.size());
    juce::AudioBuffer<float> buf (std::max (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), std::max (first, block));
    juce::MidiBuffer midi;
    int idx = 0;
    for (size_t pos = 0; pos < x.size(); ++idx)
    {
        const int len = (int) std::min<size_t> ((size_t) (idx == 0 ? first : block), x.size() - pos);
        if (before) before (idx);
        buf.setSize (buf.getNumChannels(), len, false, false, true);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch) std::copy_n (x.data() + pos, len, buf.getWritePointer (ch));
        midi.clear();
        p.processBlock (buf, midi);
        std::copy_n (buf.getReadPointer (0), len, out.data() + pos);
        pos += (size_t) len;
    }
    return out;
}

void walk (juce::Component& c, const std::function<void (juce::Component&)>& f)
{
    f (c);
    for (auto* child : c.getChildren()) walk (*child, f);
}

std::string keyLine (const char* key, const char* scale, double confidence, double time, bool listening, const char* source)
{
    // Exactly the line Key 1.7.2 writes (~/smooth-plugins/key src/KeyFile.h, format()).
    char b[400];
    std::snprintf (b, sizeof b, "{\"key\":\"%s\",\"scale\":\"%s\",\"confidence\":%.2f,\"time\":%.1f,\"listening\":%s,\"altKey\":null,\"altScale\":null,"
                                "\"altConfidence\":0.00,\"margin\":1.00,\"unsure\":false,\"locked\":%s,\"source\":\"%s\"}",
                   key, scale, confidence, time, listening ? "true" : "false", std::strcmp (source, "locked") == 0 ? "true" : "false", source);
    return b;
}

void testPhase2Dsp()
{
    // glitter-dsp-r1-1: Reverb switched on when a block starts part way into a 16 sample chunk. 1.2.4 never woke the tank.
    for (const int first : { 5, 137 })
    {
        std::vector<float> x ((size_t) (6.0 * SR), 0.0f);
        std::vector<float> n ((size_t) (0.3 * SR));
        noise (n.data(), (int) n.size(), 0.25);
        std::copy (n.begin(), n.end(), x.begin() + (long) (0.5 * SR));
        auto p = with ({ { "reverbOn", 0 }, { "reverbDecay", 2.0f }, { "reverbMix", 100 }, { "reverbPreDelay", 0 } });
        const int block = first == 5 ? 256 : 137;
        const auto out = renderSized (*p, x, first, block, [&p] (int idx) { if (idx == 20) setParam (*p, "reverbOn", 1.0f); });
        const int lat = p->getLatencySamples();
        const double tail = toDb (rms (out.data() + (int) (0.85 * SR) + lat, (int) (0.3 * SR)));
        const std::string how = first == 5 ? "after one 5 sample block" : "in 137 sample blocks";
        check (tail > -60.0, "Reverb switched on " + how + ": its tail after the burst, level", tail, "dBFS");
        const double rt = measureRt60 (out, (int) (0.85 * SR) + lat, SR);
        check (std::abs (rt / 2.0 - 1.0) <= limits::reverbRt60Fraction, "Reverb switched on " + how + ": RT60 at Decay 2 s", rt, "s");
    }

    // glitter-dsp-r1-2: shifts past two octaves. Pitch -2 octaves on a sharp note, Harmony -24 on a sharp note, and Pitch +2 octaves +7.
    {
        const auto x = steadyVoice (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.5);
        auto p = with ({ { "pitchOn", 1 }, { "pitchOctave", -2 } });
        const auto out = left (render (*p, x, 256));
        const double e = percentile (absolute (centsOff (out.data(), (long) out.size(), SR, noteHz (33), 0.3, 1.4, 0.005, 0.1)), 95);
        check (e < limits::pitchCents, "Pitch -2 octaves on A3 sung 30 cents sharp: on A1, 95th percentile", e, "cents");
    }
    {
        const auto x = steadyVoice (220.0 * std::pow (2.0, 30.0 / 1200.0), 1.5);
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", -24 }, { "harmonyLevel", 6 } });
        const auto out = left (render (*p, x, 256));
        const auto pk = findPeak (out.data() + (int) (0.4 * SR), (int) SR, noteHz (33), 60.0);
        check (std::abs (pk.cents) < limits::harmonyCents, "Harmony -24 on A3 sung 30 cents sharp: the added voice on A1, off by", pk.cents, "cents");
    }
    {
        const auto x = steadyVoice (220.0, 1.5);
        auto p = with ({ { "pitchOn", 1 }, { "pitchOctave", 2 }, { "pitchSemitones", 7 } });
        const auto out = left (render (*p, x, 256));
        const double e = percentile (absolute (centsOff (out.data(), (long) out.size(), SR, noteHz (57 + 19), 0.3, 1.4, 0.005, 0.1)), 95);
        check (e < limits::pitchCents, "Pitch +2 octaves +7 on A3: E5, the most two octaves reach, in tune, 95th percentile", e, "cents");
    }

    // glitter-dsp-r2-1: Reverb Pre-Delay moved while playing. A jump of the read point was a step straight into the wet signal.
    {
        const auto x = tone (220.0, 4.0, 0.5);
        const Settings s { { "reverbOn", 1 }, { "reverbMix", 100 }, { "reverbType", 0 }, { "reverbPreDelay", 0 } };
        const auto stepOf = [&] (std::function<void (PluginProcessor&, int)> move, float pre)
        {
            auto p = with (s);
            setParam (*p, "reverbPreDelay", pre);
            prepare (*p, 1, SR, 256);
            const auto out = left (render (*p, x, 256, nullptr, [&p, &move] (int idx) { if (move) move (*p, idx); }));
            return maxJump (out.data() + (int) SR, (int) out.size() - (int) SR);
        };
        const double steady = std::max (stepOf (nullptr, 0.0f), stepOf (nullptr, 103.0f));
        const double moving = stepOf ([] (PluginProcessor& p, int idx) { if (idx > 0 && idx % 41 == 0) setParam (p, "reverbPreDelay", (idx / 41) % 2 ? 103.0f : 0.0f); }, 0.0f);
        check (moving <= limits::switchStepRatio * steady + limits::switchStepSlack, "Reverb Pre-Delay jumping 0 to 103 ms while playing, biggest step (steady "
               + juce::String (steady, 4).toStdString() + ")", moving);
    }

    // glitter-window-r2-1 and glitter-dsp-r1-3: a preset recalled while audio runs. 1.2.4 passed every module control through its default
    // first, so a block could start on all defaults: modules off, Delay Time 1/4, Output Gain 0 dB.
    {
        struct Watch final : public juce::AudioProcessorParameter::Listener
        {
            PluginProcessor& p;
            std::function<bool (PluginProcessor&)> wrong;
            int bad = 0, seen = 0;
            Watch (PluginProcessor& q, std::function<bool (PluginProcessor&)> w) : p (q), wrong (std::move (w)) {}
            void parameterValueChanged (int, float) override { ++seen; if (wrong (p)) ++bad; }
            void parameterGestureChanged (int, bool) override {}
        };
        const auto raw = [] (PluginProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        const auto run = [&] (int from, int to, std::function<bool (PluginProcessor&)> wrong, const std::string& what)
        {
            auto p = make();
            p->setCurrentProgram (from);
            Watch w (*p, wrong);
            for (auto* prm : p->getParameters()) prm->addListener (&w);
            p->setCurrentProgram (to);
            for (auto* prm : p->getParameters()) prm->removeListener (&w);
            check (w.bad == 0 && w.seen > 0, what + ", states a block could have started on that were wrong", w.bad);
        };
        run (7, 8, [&] (PluginProcessor& p) { return raw (p, "delayOn") < 0.5f || juce::roundToInt (raw (p, "delayTime")) != 6; },
             "Filter Riser to Dub Throw: Delay on at 1/8 Dotted throughout");
        run (6, 6, [&] (PluginProcessor& p) { return raw (p, "filterOn") < 0.5f || raw (p, "distOn") < 0.5f || std::abs (raw (p, "outputGain") - 10.8f) > 0.01f; },
             "Phone Line again: Filter, Distortion and +10.8 dB throughout");
    }

    // glitter-dsp-r3-1: a jump in Doubler Timing. 1.2.4 slid the read point, which bent the doubles' pitch by up to about 20 semitones.
    for (const auto& jump : { std::pair<float, float> { 5.0f, 60.0f }, std::pair<float, float> { 60.0f, 5.0f }, std::pair<float, float> { 22.0f, 40.0f } })
    {
        auto p = with ({ { "doublerOn", 1 }, { "doublerLevel", 6 }, { "doublerWidth", 100 }, { "doublerDetune", 0 }, { "doublerTiming", jump.first } }, 2);
        const auto x = tone (220.0, 2.0);
        const int at = 100;
        const auto out = left (render (*p, x, 256, nullptr, [&p, at, jump] (int idx) { if (idx == at) setParam (*p, "doublerTiming", jump.second); }));
        const double from = (at * 256 + p->getLatencySamples()) / SR;
        const auto e = absolute (centsOff (out.data(), (long) out.size(), SR, 220.0, from + 0.002, from + 0.15, 0.002, 0.15));
        const double worst = e.empty() ? 1.0e9 : *std::max_element (e.begin(), e.end());
        check (worst <= 60.0, "Doubler Timing jumping " + juce::String (jump.first, 0).toStdString() + " to " + juce::String (jump.second, 0).toStdString()
               + " ms, Detune 0: the doubled side's pitch over the next 150 ms, worst", worst, "cents");
    }

    // glitter-dsp-r1-4: Input Type changed with the Doubler on. The restart sat in the doubles' delay lines and came out one Timing later.
    {
        const auto x = tone (220.0, 2.0);
        const Settings s { { "doublerOn", 1 }, { "doublerLevel", 0 }, { "doublerDetune", 0 } };
        auto still = with (s, 2);
        const auto a = left (render (*still, x, 256));
        const int at = 100, start = at * 256;
        const double steady = maxJump (a.data() + start - 24000, 24000);
        auto moved = with (s, 2);
        const auto b = left (render (*moved, x, 256, nullptr, [&moved, at] (int idx) { if (idx == at) setParam (*moved, "inputType", 2.0f); }));
        const double during = maxJump (b.data() + start + (int) (0.010 * SR), (int) (0.020 * SR));
        check (during <= limits::switchStepRatio * steady + limits::switchStepSlack, "Input Type Alto/Tenor to Low Male with the Doubler on: biggest step 10 to 30 ms after (steady "
               + juce::String (steady, 4).toStdString() + ")", during);
    }

    // glitter-dsp-r1-5: mono in and out. The doubles and two Harmony voices folded down at half level when panned, so Width set their loudness.
    {
        const auto x = tone (220.0, 1.6);
        const auto added = [&x] (Settings s, const char* onId, int channels)
        {
            auto on = with (s, channels);
            setParam (*on, onId, 1.0f);
            prepare (*on, channels, SR, 256);
            auto off = with (s, channels);
            const auto a = left (render (*on, x, 256)), b = left (render (*off, x, 256));
            std::vector<float> d (a.size());
            for (size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
            return toDb (rms (d.data() + (int) (0.5 * SR), (int) SR));
        };
        const double d100 = added ({ { "doublerDetune", 0 }, { "doublerWidth", 100 } }, "doublerOn", 1), d0 = added ({ { "doublerDetune", 0 }, { "doublerWidth", 0 } }, "doublerOn", 1);
        check (std::abs (d100 - d0) <= 0.5, "mono in and out: the doubles at Width 100 against Width 0", d100 - d0, "dB");
        const Settings h { { "harmonyVoices", 1 }, { "harmonyInKey", 0 }, { "harmonyInterval1", 4 }, { "harmonyInterval2", 7 } };
        auto h80 = h; h80.push_back ({ "harmonyWidth", 80 });
        auto h0 = h; h0.push_back ({ "harmonyWidth", 0 });
        const double v80 = added (h80, "harmonyOn", 1), v0 = added (h0, "harmonyOn", 1);
        check (std::abs (v80 - v0) <= 0.5, "mono in and out: two Harmony voices at Width 80 against Width 0", v80 - v0, "dB");
    }

    // glitter-dsp-r1-6: Follow Key read the key and major or minor in separate loads. The audio thread now reads them in one load.
    {
        const auto src = juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getParentDirectory().getChildFile ("src/PluginProcessor.cpp").loadFileAsString();
        const int from = src.indexOf ("void PluginProcessor::readSettings()");
        const auto body = src.substring (from, src.indexOf (from, "activeKeyNow = root;"));
        int loads = 0;
        for (int at = body.indexOf ("follower."); at >= 0; at = body.indexOf (at + 1, "follower.")) ++loads;
        check (loads == 1, "Follow Key: the audio thread reads the followed key in one load, reads in readSettings", loads);

        // A stress run as well, for information: a thread rewrites the file between C major and A minor while blocks run. A torn read is C minor or A major.
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("glitter-key-tear.json");
        file.replaceWithText (keyJson ("C", "major", 0.9, nowUnix()));
        auto p = make (file.getFullPathName());
        setParam (*p, "followKey", 1.0f);
        p->pollKeyFileNow();    // C major from the first block (the first version counted the blocks before the first poll, on the menus' key)
        prepare (*p, 1, SR, 16);
        std::atomic<bool> running { true };
        std::thread writer ([&]
        {
            bool minor = false;
            while (running.load())
            {
                minor = ! minor;
                file.replaceWithText (minor ? keyJson ("A", "minor", 0.9, nowUnix()) : keyJson ("C", "major", 0.9, nowUnix()));
                for (int i = 0; i < 20; ++i) p->pollKeyFileNow();
            }
        });
        juce::AudioBuffer<float> buf (1, 16);
        juce::MidiBuffer midi;
        int torn = 0, blocks = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (1500);
        while (std::chrono::steady_clock::now() < until)
        {
            buf.clear();
            p->processBlock (buf, midi);
            const auto r = p->getReadings();
            const bool cMajor = r.activeKey == 0 && r.activeScale == smoothtune::scaleMajor, aMinor = r.activeKey == 9 && r.activeScale == smoothtune::scaleMinor;
            torn += (cMajor || aMinor) ? 0 : 1;
            ++blocks;
        }
        running = false;
        writer.join();
        file.deleteFile();
        info ("Follow Key stress: blocks read while the file flips between C major and A minor", blocks);
        info ("Follow Key stress: blocks that read a key neither file holds (C minor or A major)", torn);
    }

    // glitter-dsp-r2-2: Harmony Interval changed in a pause. 1.2.4 only moved the interval while the voice was pitched, so the next phrase
    // started at the old one and slid over.
    {
        auto x = steadyVoice (220.0, 0.6);
        x.resize (x.size() + (size_t) (0.3 * SR), 0.0f);
        const auto second = steadyVoice (220.0, 0.6);
        x.insert (x.end(), second.begin(), second.end());
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyInKey", 0 }, { "harmonyInterval1", 7 } });
        const int change = (int) (0.75 * SR) / 256;
        render (*p, x, 256, nullptr, [&p, change] (int idx) { if (idx == change) setParam (*p, "harmonyInterval1", -5.0f); });
        const auto count = p->getPitchHistoryCount();
        int unvoiced = 0, checked = 0;
        double worst = 0.0;
        for (long long i = 0; i < count && checked < 2; ++i)
        {
            const auto& pt = p->getPitchPoint (i);
            const float h = pt.harmony1.load(), o = pt.outputNote.load();
            if (h <= 0.0f) { ++unvoiced; continue; }
            if (unvoiced >= 30)    // past the pause
            {
                worst = std::max (worst, std::abs ((double) (h - o) + 5.0));
                ++checked;
            }
            else unvoiced = 0;
        }
        check (checked == 2 && worst <= 0.1, "Harmony Interval +7 to -5 set in a pause: the next phrase's first 10 ms against -5, worst", worst, "semitones");
    }

    // glitter-dsp-r2-3: the host's Reset. 1.2.4 had none, so tails, queued MIDI and held notes carried on.
    {
        const Settings tails { { "delayOn", 1 }, { "delayFeedback", 90 }, { "delayMix", 50 }, { "reverbOn", 1 }, { "reverbDecay", 3.0f } };
        const auto x = tone (220.0, 1.0);
        const std::vector<float> quiet ((size_t) (2.0 * SR), 0.0f);
        {
            auto p = with (tails, 2);
            render (*p, x, 256);
            p->reset();
            const auto out = render (*p, quiet, 256);
            check (toDb (std::max (peak (out.getReadPointer (0), out.getNumSamples()), peak (out.getReadPointer (1), out.getNumSamples()))) < -120.0,
                   "Reset after Delay at Feedback 90 and Reverb: peak of the 2 s after", toDb (peak (out.getReadPointer (0), out.getNumSamples())), "dBFS");
        }
        {
            auto p = with ({}, 1);
            const std::vector<float> one ((size_t) 256, 0.0f);
            juce::MidiBuffer m;
            m.addEvent (on (64), 255);
            render (*p, one, 256, &m);
            p->reset();
            render (*p, std::vector<float> ((size_t) (0.1 * SR), 0.0f), 256);
            check (p->getReadings().heldNotes == 0, "a note on still in the latency queue at a Reset is dropped, notes held after", p->getReadings().heldNotes);
        }
        {
            const auto y = steadyVoice (196.0, 1.0);
            auto p = with (allOn(), 2);
            render (*p, x, 256);
            const long before = testkit::allocs.load();
            p->reset();
            const long made = testkit::allocs.load() - before;
            const auto a = render (*p, y, 256);
            auto fresh = with (allOn(), 2);
            const auto b = render (*fresh, y, 256);
            check (made == 0, "Reset allocates nothing, allocations", (double) made);
            check (residualDb (a, b, 0) <= -200.0, "all on: play, Reset, play against a new instance, difference", residualDb (a, b, 0), "dB");
            auto unprepared = make();
            unprepared->reset();
            check (true, "Reset before prepareToPlay is safe");
        }
        {
            // The Reset the AU wrapper sends as the host's bypass goes on is left out, so the 20 ms fade to the dry voice is not cut.
            // A later one, while still bypassed, is not.
            auto p = with (tails, 2), q = with (tails, 2);
            render (*p, x, 256);
            render (*q, x, 256);
            setParam (*p, "bypass", 1.0f);
            setParam (*q, "bypass", 1.0f);
            p->reset();
            const auto a = render (*p, std::vector<float> ((size_t) 512, 0.0f), 256), b = render (*q, std::vector<float> ((size_t) 512, 0.0f), 256);
            check (residualDb (a, b, 0) <= -200.0, "the Reset that comes with bypass switching on changes nothing, difference", residualDb (a, b, 0), "dB");
            p->reset();
            setParam (*p, "bypass", 0.0f);
            const auto out = render (*p, quiet, 256);
            check (toDb (peak (out.getReadPointer (0), out.getNumSamples())) < -120.0, "a later Reset while bypassed clears the tails, peak after", toDb (peak (out.getReadPointer (0), out.getNumSamples())), "dBFS");
        }
    }

    // glitter-dsp-r3-2: Filter Sweep Depth and Rate moved while the sweep runs. 1.2.4 jumped the cutoff by octaves within one 16 sample chunk.
    // Judged against Filter Cutoff's own glide for its largest jump, 20 Hz to 20 kHz, measured the same way.
    {
        const auto x = tone (220.0, 2.0);
        const auto fastest = [&x] (Settings s, int at, const char* id, float to, double ppqStart)
        {
            auto p = with (s, 1, SR, 16);
            FakeHead head;
            p->setPlayHead (&head);
            double worst = 0.0, last = -1.0;
            render (*p, x, 16, nullptr, [&] (int idx)
            {
                head.ppq = ppqStart + idx * 16.0 * 120.0 / 60.0 / SR;
                const double hz = p->getFilterHz();
                if (idx > at && last > 0.0) worst = std::max (worst, std::abs (std::log2 (hz / last)));
                last = hz;
                if (idx == at) setParam (*p, id, to);
            });
            p->setPlayHead (nullptr);
            return worst;
        };
        const Settings base { { "filterOn", 1 }, { "filterType", 2 }, { "filterResonance", 80 }, { "filterCutoff", 400 } };
        const double lat = 659.0 * 120.0 / 60.0 / SR;
        auto cut = base; cut.push_back ({ "filterCutoff", 20 });
        const double reference = fastest (cut, 400, "filterCutoff", 20000.0f, 0.0);
        auto depth = base; depth.push_back ({ "filterSweepRate", 4 }); depth.push_back ({ "filterSweepDepth", 0 });
        // At a peak of the 1/4 sine: the jump to Depth 4 lands 4 octaves up.
        const double depthMove = fastest (depth, 400, "filterSweepDepth", 4.0f, 0.25 + lat - 400 * 16.0 * 120.0 / 60.0 / SR);
        auto rate = base; rate.push_back ({ "filterSweepRate", 2 }); rate.push_back ({ "filterSweepDepth", 2 });
        // A quarter of a beat in, 1 Bar's sine is at 0.38 and the 1/4 sine at its peak.
        const double rateMove = fastest (rate, 400, "filterSweepRate", 4.0f, 0.25 + lat - 400 * 16.0 * 120.0 / 60.0 / SR);
        info ("Filter Cutoff 20 Hz to 20 kHz, fastest move of the cutoff in 16 samples", reference, "octaves");
        check (depthMove <= limits::switchStepRatio * reference, "Filter Sweep Depth 0 to 4 at the top of the sweep: fastest move in 16 samples", depthMove, "octaves");
        check (rateMove <= limits::switchStepRatio * reference, "Filter Sweep Rate 1 Bar to 1/4 mid sweep: fastest move in 16 samples", rateMove, "octaves");
    }
}

void testPhase2Host()
{
    // HostStress paramtext: a reading's own text reads back as itself.
    {
        auto p = make();
        for (const char* id : { "targetNote", "activeKey" })
        {
            juce::AudioProcessorParameter* prm = nullptr;
            for (auto* q : p->getParameters())
                if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (q); w != nullptr && w->getParameterID() == id) prm = q;
            int wrong = 0, total = 0;
            const int steps = std::string (id) == "targetNote" ? 128 : 12 * smoothtune::numScales - 1;
            for (int k = 0; k <= steps; ++k)
            {
                const float v = (float) k / (float) steps;
                const auto text = prm->getText (v, 64);
                const float back = prm->getValueForText (text);
                wrong += prm->getText (back, 64) == text && std::abs (back - v) < 0.5f / (float) steps ? 0 : 1;
                ++total;
            }
            check (wrong == 0, std::string ("reading ") + id + ": its text reads back as itself, values that did not, of " + std::to_string (total), wrong);
        }
    }

    // key-file-link-r2-1: an offline bounce takes a Key change at the song position it happened, by audio time, not wall time.
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("glitter-key-offline.json");
    {
        file.replaceWithText (keyLine ("C", "major", 0.9, nowUnix(), true, "heard"));
        auto p = make (file.getFullPathName());
        setParam (*p, "followKey", 1.0f);
        p->pollKeyFileNow();
        prepare (*p, 1, SR, 512);
        p->setNonRealtime (true);
        render (*p, std::vector<float> ((size_t) (0.2 * SR), 0.0f), 512);
        const bool startC = p->getReadings().activeKey == 0;
        file.replaceWithText (keyLine ("E", "major", 0.9, nowUnix(), true, "heard"));
        render (*p, std::vector<float> ((size_t) (0.4 * SR), 0.0f), 512);
        check (startC && p->getReadings().activeKey == 4, "offline bounce: a Key change is taken within 0.4 s of audio, active key after (E is 4)", p->getReadings().activeKey);
        p->setNonRealtime (false);
    }
    // Key 1.7.2: a Manual key is written with listening false and rewritten at least every 240 s. Both are used, as a Locked one is.
    // Guards: these pass on 1.2.4 too, which never refused listening false.
    {
        auto p = make (file.getFullPathName());
        setParam (*p, "followKey", 1.0f);
        file.replaceWithText (keyLine ("E", "major", 1.0, nowUnix(), false, "manual"));
        p->pollKeyFileNow();
        prepare (*p, 1, SR, 256);
        render (*p, std::vector<float> (256, 0.0f), 256);
        check (p->getReadings().activeKey == 4, "a Manual key from Key 1.7.2 (listening false) is followed, active key (E is 4)", p->getReadings().activeKey);
        file.replaceWithText (keyLine ("A", "minor", 1.0, nowUnix() - 239.0, false, "manual"));
        p->pollKeyFileNow();
        render (*p, std::vector<float> (256, 0.0f), 256);
        check (p->getReadings().activeKey == 9 && p->getReadings().activeScale == smoothtune::scaleMinor, "a Manual key written 239 s ago is still followed (A minor)");
        file.replaceWithText (keyLine ("D", "major", 1.0, nowUnix() - 30.0, false, "locked"));
        p->pollKeyFileNow();
        render (*p, std::vector<float> (256, 0.0f), 256);
        check (p->getReadings().activeKey == 2, "a Locked key from a Key that has closed (listening false, old time) is followed, active key", p->getReadings().activeKey);
    }
    file.deleteFile();

    // glitter-window-r1-3 and the pattern from the other plug-ins: the chosen preset is saved with the session. Sessions from 1.2.4 name none.
    {
        auto a = make();
        a->setCurrentProgram (8);
        juce::MemoryBlock st;
        a->getStateInformation (st);
        auto b = make();
        b->setStateInformation (st.getData(), (int) st.getSize());
        check (b->getCurrentProgram() == 8, "Dub Throw chosen, saved and loaded: the preset named", b->getCurrentProgram());

        const auto folder = juce::File (SMOOTH_TUNE_CORPUS).getParentDirectory().getChildFile ("baseline");
        const auto readF32 = [] (const juce::File& f) { juce::MemoryBlock m; f.loadFileAsData (m); std::vector<float> v (m.getSize() / 4); std::memcpy (v.data(), m.getData(), v.size() * 4); return v; };
        const auto input = readF32 (folder.getChildFile ("input.f32"));
        int named = 0, states = 0;
        double worst = -240.0;
        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "v1.2.4-*.state"))
        {
            juce::MemoryBlock st124;
            f.loadFileAsData (st124);
            auto p = make();
            p->setStateInformation (st124.getData(), (int) st124.getSize());
            named += p->getCurrentProgram() >= 0 ? 1 : 0;
            ++states;
            prepare (*p, 2, SR, 512);
            const auto out = render (*p, input, 512);
            const auto want = readF32 (f.withFileExtension ("f32"));
            juce::AudioBuffer<float> ref (2, (int) input.size());
            if (want.size() == 2 * input.size())
            {
                std::copy_n (want.data(), input.size(), ref.getWritePointer (0));
                std::copy_n (want.data() + input.size(), input.size(), ref.getWritePointer (1));
            }
            const double d = residualDb (ref, out, 0);
            worst = std::max (worst, d);
            std::printf ("INFO  1.2.4 session %s: against what the installed 1.2.4 played, %.1f dB\n", f.getFileNameWithoutExtension().toRawUTF8(), d);
        }
        check (states >= 9 && named == 0, "sessions saved by 1.2.4 name no preset (Choose a preset), sessions", states);
        check (worst <= limits::nullDb, "sessions saved by 1.2.4 play what the installed 1.2.4 played, worst difference", worst, "dB");
    }
}

void testPhase2Window()
{
    const auto paintAll = [] (juce::Component& c)
    {
        juce::Image image (juce::Image::ARGB, juce::jmax (1, c.getWidth()), juce::jmax (1, c.getHeight()), true);
        juce::Graphics g (image);
        c.paintEntireComponent (g, false);
        return image;
    };
    const auto same = [] (const juce::Image& a, const juce::Image& b)
    {
        if (a.getWidth() != b.getWidth() || a.getHeight() != b.getHeight()) return false;
        const juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly), db (b, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < a.getHeight(); ++y)
            if (std::memcmp (da.getLinePointer (y), db.getLinePointer (y), (size_t) (a.getWidth() * da.pixelStride)) != 0) return false;
        return true;
    };

    // glitter-window-r3-1: with Advanced open and nothing playing, the window goes still, so its frames can stop.
    {
        auto p = make();
        p->advancedOpen = true;
        prepare (*p, 2, SR, 256);
        TuneFxEditor editor (*p);
        for (int i = 0; i < 40; ++i) { editor.tick (1.0 / 30.0); paintAll (editor); }
        int moving = 0;
        for (int i = 0; i < 10; ++i) moving += editor.getGraph().refresh() ? 1 : 0;
        check (moving == 0, "Advanced open, no audio: frames where the hidden module picture still says it moved, of 10", moving);
    }

    // glitter-window-r1-1: the strip asks to be drawn again when the last of the voice scrolls off, so the old trace goes and the hint comes back.
    {
        auto p = with ({}, 2);
        TuneFxEditor editor (*p);
        render (*p, steadyVoice (220.0, 1.0), 256);
        editor.tick (1.0 / 30.0);
        paintAll (editor.getStrip());
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        long long lastVoiced = -1;
        int asked = -1;
        for (int b = 0; b < (int) (6.5 * SR) / 256 && asked < 0; ++b)
        {
            buf.clear();
            p->processBlock (buf, midi);
            for (long long i = juce::jmax (0LL, lastVoiced); i < p->getPitchHistoryCount(); ++i)    // the voice's last points arrive a little after it stops
                if (p->getPitchPoint (i).outputNote.load() > 0.0f) lastVoiced = i;
            const bool repaintAsked = editor.getStrip().advance (256.0 / SR);
            if (lastVoiced >= 0 && p->getPitchHistoryCount() - 1 - lastVoiced >= PitchStrip::pointsShown)
                asked = repaintAsked ? 1 : 0;
        }
        check (lastVoiced >= 0 && asked == 1, "the strip asks to be drawn when the last voiced point leaves it (1 yes, 0 no)", asked);
    }

    // glitter-window-r1-2: the Advanced button's words fit beside its arrow, with every Advanced setting changed.
    {
        auto p = make();
        setParam (*p, "inputType", 0.0f);
        for (int i = 0; i < 12; ++i) setParam (*p, ("note" + std::to_string (i)).c_str(), 1.0f);
        prepare (*p, 2, SR, 256);
        TuneFxEditor editor (*p);
        editor.tick (1.0 / 30.0);
        const auto label = editor.advancedLabel();
        const float width = juce::GlyphArrangement::getStringWidth (fxui::font (fxui::Weight::medium, 13.5f), label);
        check (label != "Advanced" && width <= (float) editor.advancedLabelRoom(), "the Advanced button with 13 changes: '" + label.toStdString() + "' fits in "
               + std::to_string (editor.advancedLabelRoom()) + " px, width", width, "px");
    }

    // glitter-window-r1-4: the header says Glitter.
    {
        auto p = make();
        TuneFxEditor editor (*p);
        check (editor.headerTitle() == p->getName(), "the header names the plug-in: " + editor.headerTitle().toStdString());
        check (juce::GlyphArrangement::getStringWidth (fxui::font (fxui::Weight::medium, 24.0f), editor.headerTitle()) <= 110.0f, "the header's name fits its 110 px");
    }

    // glitter-window-r1-5: the Harmony picture names the note being sung now, not one seen on Motion's panel earlier.
    {
        auto p = with ({}, 2);
        TuneFxEditor seen (*p);
        seen.selectModule (1);
        render (*p, steadyVoice (220.0, 0.6), 256);
        seen.tick (1.0 / 30.0);
        seen.selectModule (3);
        render (*p, steadyVoice (330.0, 0.6), 256);
        for (int i = 0; i < 5; ++i) seen.tick (1.0 / 30.0);
        TuneFxEditor fresh (*p);
        fresh.selectModule (3);
        for (int i = 0; i < 5; ++i) fresh.tick (1.0 / 30.0);
        check (same (paintAll (seen.getGraph()), paintAll (fresh.getGraph())), "the Harmony picture after Motion showed A3 and E4 is sung: the same as a new window's (E4)");
    }

    // glitter-window-r1-6: Advanced glides open over about a fifth of a second, instead of jumping 98% at the click.
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        TuneFxEditor editor (*p);
        auto* page = editor.findControl ("inputType")->getParentComponent();
        const auto openShare = [&] { return page->isVisible() ? (float) (664 - page->getY()) / (float) (664 - (264 + 80 + ModuleRow::tabDepth)) : 0.0f; };
        editor.setAdvancedOpen (true, true);
        const float atClick = openShare();
        for (int i = 0; i < 12; ++i) editor.tick (1.0 / 30.0);
        check (atClick <= 0.5f && openShare() >= 0.999f, "Advanced at the click (glides) and fully open 0.4 s later, open at the click", atClick);
    }

    // glitter-window-r1-7: a state loaded with the window open brings the window's module and Advanced along.
    {
        auto donor = make();
        donor->advancedOpen = true;
        donor->selectedModule = 8;
        juce::MemoryBlock st;
        donor->getStateInformation (st);
        auto p = make();
        prepare (*p, 2, SR, 256);
        TuneFxEditor editor (*p);
        editor.selectModule (3);
        p->setStateInformation (st.getData(), (int) st.getSize());
        for (int i = 0; i < 3; ++i) editor.tick (1.0 / 30.0);
        check (editor.getSelectedModule() == p->selectedModule.load() && editor.getRow().getSelected() == -1,
               "a state with Advanced open on Ring loaded into an open window: the module shown (8 wanted)", editor.getSelectedModule());
        editor.setAdvancedOpen (false, false);
        check (editor.getRow().getSelected() == 8 && p->selectedModule.load() == 8, "closing Advanced then shows Ring's tab", editor.getRow().getSelected());
    }

    // glitter-window-r1-8: the strip places its picture by the count it drew, not by a second read that audio may have moved on.
    {
        auto p = with ({}, 2);
        TuneFxEditor editor (*p);
        editor.getStrip().setBounds (0, 0, 640, 186);
        std::atomic<bool> running { true };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, 64);
            juce::MidiBuffer midi;
            const auto x = steadyVoice (220.0, 1.0);
            size_t at = 0;
            while (running.load())
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 64; ++i) buffer.setSample (ch, i, x[(at + (size_t) i) % x.size()]);
                at += 64;
                p->processBlock (buffer, midi);
            }
        });
        int off = 0;
        for (int i = 0; i < 3000; ++i)
        {
            editor.getStrip().advance (1.0 / 30.0);
            paintAll (editor.getStrip());
            off += editor.getStrip().getPlacedCount() != editor.getStrip().getDrawnCount() ? 1 : 0;
        }
        running = false;
        audio.join();
        check (off == 0, "the strip's picture placed by the count it was drawn to, paints that were not, of 3000", off);
    }

    // glitter-window-r2-2: the footer's hint ends before the latency reading starts.
    {
        auto p = make();
        TuneFxEditor editor (*p);
        auto* hint = editor.findControl ("hintLine");
        auto* latency = editor.findControl ("latency");
        check (hint != nullptr && latency != nullptr && hint->getRight() <= latency->getX(), "the footer hint ends before 'Adds 13.7 ms', overlap",
               hint != nullptr && latency != nullptr ? (double) (hint->getRight() - latency->getX()) : 999.0, "px");
    }

    // glitter-window-r2-3: the bar under the note shows no correction while bypassed.
    {
        auto p = with ({ { "pitchOn", 1 }, { "pitchSemitones", 12 }, { "bypass", 1 } }, 2);
        TuneFxEditor editor (*p);
        render (*p, steadyVoice (210.0, 1.0), 256);
        for (int i = 0; i < 20; ++i) editor.tick (1.0 / 30.0);
        check (editor.correctionBarPixels() == 0, "bypassed with Pitch +12 on: the correction bar's reach", editor.correctionBarPixels(), "px");
    }

    // glitter-window-r2-4: on Band Pass, the Resonance dot dropped on the drawn peak keeps Resonance high.
    {
        auto p = make();
        TuneFxEditor editor (*p);
        editor.selectModule (7);
        setParam (*p, "filterType", 1.0f);
        setParam (*p, "filterResonance", 100.0f);
        editor.tick (1.0 / 30.0);
        editor.getGraph().dragHandleTo (0, { 0.5f, 24.0f / 60.0f });   // the 0 dB line, where the band's drawn peak is
        const float res = p->apvts.getRawParameterValue ("filterResonance")->load();
        check (res >= 95.0f, "Band Pass at Resonance 100, the dot dropped on the drawn peak: Resonance after", res, "%");
    }

    // glitter-window-r2-5: '2.5k Hz' typed, with a space before the unit.
    {
        auto p = make();
        TuneFxEditor editor (*p);
        auto* cutoff = dynamic_cast<fxui::ParamSlider*> (editor.findControl ("filterCutoff"));
        auto* toneSlider = dynamic_cast<fxui::ParamSlider*> (editor.findControl ("distTone"));
        cutoff->applyTyped ("2.5k Hz");
        toneSlider->applyTyped ("1.2 kHz");
        check (std::abs (p->apvts.getRawParameterValue ("filterCutoff")->load() - 2500.0f) < 1.0f, "typing '2.5k Hz' gives 2500 Hz", p->apvts.getRawParameterValue ("filterCutoff")->load(), "Hz");
        check (std::abs (p->apvts.getRawParameterValue ("distTone")->load() - 1200.0f) < 1.0f, "typing '1.2 kHz' gives 1200 Hz", p->apvts.getRawParameterValue ("distTone")->load(), "Hz");
    }

    // glitter-window-r3-2: with one Harmony voice two octaves up, the strip keeps the voice coming out on screen.
    {
        auto p = with ({ { "harmonyOn", 1 }, { "harmonyInterval1", 24 } }, 2);
        render (*p, steadyVoice (220.0, 2.0), 256);
        TuneFxEditor editor (*p);
        for (int i = 0; i < 20; ++i) editor.tick (1.0 / 30.0);
        const auto& s = editor.getStrip();
        const int out = s.getOutputNote();
        check (out >= 0 && (float) out >= s.getLowestNote() && (float) out <= s.getHighestNote(), "Harmony +24: the output note (A3, 57) inside the strip's range, lowest shown",
               s.getLowestNote());
    }

    // glitter-window-r3-3: the header says Following only when a key is really being followed.
    {
        const auto statusOf = [] (PluginProcessor& p)
        {
            TuneFxEditor editor (p);
            editor.tick (1.0 / 30.0);
            auto* label = dynamic_cast<juce::Label*> (editor.findControl ("status"));
            return label != nullptr ? label->getText() : juce::String ("(no status)");
        };
        auto p = make ("/nonexistent/glitter-key.json");
        setParam (*p, "followKey", 1.0f);
        prepare (*p, 1, SR, 256);
        p->pollKeyFileNow();
        render (*p, std::vector<float> (256, 0.0f), 256);
        const auto none = statusOf (*p);
        check (! none.startsWith ("Following"), "Follow Key on with no key file: the header says '" + none.toStdString() + "'");

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("glitter-key-status.json");
        file.replaceWithText (keyLine ("D", "major", 0.9, nowUnix(), true, "heard"));
        auto q = make (file.getFullPathName());
        setParam (*q, "followKey", 1.0f);
        setParam (*q, "scale", (float) smoothtune::scaleCustom);
        prepare (*q, 1, SR, 256);
        q->pollKeyFileNow();
        render (*q, std::vector<float> (256, 0.0f), 256);
        const auto custom = statusOf (*q);
        check (! custom.startsWith ("Following"), "Follow Key on with a Custom scale: the header says '" + custom.toStdString() + "'");
        setParam (*q, "scale", (float) smoothtune::scaleMajor);
        render (*q, std::vector<float> (256, 0.0f), 256);
        const auto followed = statusOf (*q);
        check (followed.startsWith ("Following D Major"), "Follow Key on with a good key file: the header says '" + followed.toStdString() + "'");
        file.deleteFile();
    }

    // glitter-window-r3-4: while Advanced closes, its page never covers the footer.
    {
        auto p = make();
        prepare (*p, 2, SR, 256);
        TuneFxEditor editor (*p);
        auto* page = editor.findControl ("inputType")->getParentComponent();
        auto* gain = editor.findControl ("outputGain");
        editor.setAdvancedOpen (true, false);
        editor.setAdvancedOpen (false, true);
        int covered = 0;
        for (int i = 0; i < 12; ++i)
        {
            covered += page->isVisible() && page->getBoundsInParent().intersects (gain->getBoundsInParent()) ? 1 : 0;
            editor.tick (1.0 / 30.0);
        }
        check (covered == 0, "Advanced closing: frames where its page covered Output Gain, of 12", covered);
    }
}

void testPhase2()
{
    testPhase2Dsp();
    testPhase2Host();
    testPhase2Window();
}
