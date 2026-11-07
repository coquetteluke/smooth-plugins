#include "StageView.h"

using namespace smoothharmony;

namespace ui
{
    namespace
    {
        constexpr int gutter = 40;              // note names, on the left
        constexpr int stageWidth = 310;
        constexpr float stageMargin = 38.0f;    // a singer at full pan still fits
        constexpr float topAndBottom = 14.0f;
        constexpr float stageFooter = 26.0f;    // Left and Right, under the singers
        constexpr int minSpan = 24;             // semitones shown, at least

        const juce::Colour colourFor (int singer)
        {
            return singer == StageView::leadIndex ? tone::text : tone::voice[singer];
        }

        juce::String noteText (int note)
        {
            if (note < 0)
                return "-";

            return juce::String (noteNames[note % 12]) + juce::String (note / 12 - 1);
        }

        float levelAmount (float peak) noexcept
        {
            // 0 at -48 dB and below, 1 at full scale
            return peak <= 0.004f ? 0.0f : juce::jlimit (0.0f, 1.0f, 1.0f + std::log10 (peak) * 20.0f / 48.0f);
        }
    }

    StageView::StageView (PluginProcessor& p) : processor (p)
    {
        auto& apvts = p.apvts;

        for (int v = 0; v < PluginProcessor::maxVoices; ++v)
        {
            const auto base = "voice" + juce::String (v + 1);
            levelValue[(size_t) v] = apvts.getRawParameterValue (base + "Level");
            panValue[(size_t) v] = apvts.getRawParameterValue (base + "Pan");
            intervalValue[(size_t) v] = apvts.getRawParameterValue (base + "Interval");
        }

        modeValue = apvts.getRawParameterValue ("mode");
        voicesValue = apvts.getRawParameterValue ("voices");
        keyValue = apvts.getRawParameterValue ("key");
        scaleValue = apvts.getRawParameterValue ("scale");
        followValue = apvts.getRawParameterValue ("followKey");
        chordRootValue = apvts.getRawParameterValue ("chordRoot");
        chordTypeValue = apvts.getRawParameterValue ("chordType");
        leadPanValue = apvts.getRawParameterValue ("leadPan");
        leadLevelValue = apvts.getRawParameterValue ("leadLevel");
        bypassValue = apvts.getRawParameterValue ("bypass");

        setOpaque (true);
        setTitle ("Voices on stage");
        drawnTo = processor.getPitchHistoryCount();
    }

    StageView::~StageView() = default;

    juce::Rectangle<int> StageView::timeArea() const noexcept
    {
        return { gutter, 0, juce::jmax (10, getWidth() - gutter - stageWidth), getHeight() };
    }

    juce::Rectangle<int> StageView::stageArea() const noexcept
    {
        return { getWidth() - stageWidth, 0, stageWidth, getHeight() };
    }

    float StageView::semitonePixels() const noexcept
    {
        return ((float) getHeight() - 2.0f * topAndBottom - stageFooter) / (float) (rangeHigh - rangeLow);
    }

    float StageView::yFor (float pitch) const noexcept
    {
        return topAndBottom + ((float) rangeHigh - pitch) * semitonePixels();
    }

    float StageView::xForPan (float pan) const noexcept
    {
        const auto stage = stageArea().toFloat();
        return stage.getX() + stageMargin + (pan + 100.0f) * 0.005f * (stage.getWidth() - 2.0f * stageMargin);
    }

    float StageView::diameterFor (const Singer& s) const noexcept
    {
        // Louder singers look nearer. Off is the smallest, and it is drawn hollow.
        // With Advanced open the rows are tight, so everyone stands a little smaller and overlaps less.
        const float tightness = juce::jlimit (0.7f, 1.0f, juce::jmap (semitonePixels(), 5.0f, 11.0f, 0.7f, 1.0f));
        return tightness * juce::jmap (juce::jlimit (-36.0f, 6.0f, s.level), -36.0f, 6.0f, 25.0f, 46.0f);
    }

    juce::Point<float> StageView::singerPosition (int singer) const
    {
        const auto& s = state[(size_t) singer];
        return { xForPan (s.pan), yFor (s.pitch) };
    }

    void StageView::resized()
    {
        needsRebuild = true;
    }

    void StageView::setHighlight (int singer)
    {
        if (highlight != singer)
        {
            highlight = singer;
            repaintStage();
        }
    }

    void StageView::repaintStage()
    {
        repaint (stageArea());
    }

    juce::RangedAudioParameter* StageView::panParam (int singer) const
    {
        return processor.apvts.getParameter (singer == leadIndex ? juce::String ("leadPan") : "voice" + juce::String (singer + 1) + "Pan");
    }

    juce::RangedAudioParameter* StageView::intervalParam (int singer) const
    {
        return singer == leadIndex ? nullptr : processor.apvts.getParameter ("voice" + juce::String (singer + 1) + "Interval");
    }

    // ---- what each singer is doing --------------------------------------------------------------------------------

    void StageView::updateKey()
    {
        const int mode = juce::jlimit (0, numModes - 1, (int) modeValue->load (std::memory_order_relaxed));
        std::uint16_t mask = 0x0FFF;
        int root = 0;

        if (mode == modeChord)
        {
            root = (int) chordRootValue->load (std::memory_order_relaxed);
            mask = chordMask (root, (int) chordTypeValue->load (std::memory_order_relaxed));
        }
        else if (mode == modeScale || mode == modeFixed)
        {
            int scale = (int) scaleValue->load (std::memory_order_relaxed);
            root = (int) keyValue->load (std::memory_order_relaxed);

            // The processor only knows the key in use once audio has run.
            if (followValue->load (std::memory_order_relaxed) >= 0.5f && processor.getPitchHistoryCount() > 0)
            {
                const auto r = processor.getReadings();
                root = r.activeKey;
                scale = r.activeScale;
            }

            mask = scaleMask (root, scale);
        }

        if (mask != laneMask || root != laneRoot || mode != shownMode)
        {
            laneMask = mask;
            laneRoot = root;
            shownMode = mode;
            needsRebuild = true;
            repaint();
        }
    }

    void StageView::restPitches (std::array<float, singers>& out, std::array<bool, singers>& sings) const
    {
        const int mode = shownMode;
        const float lead = lastLeadPitch > 0.0f ? (float) juce::roundToInt (lastLeadPitch) : (float) (60 + laneRoot);
        out[(size_t) leadIndex] = lead;
        sings[(size_t) leadIndex] = true;

        for (int v = 0; v < PluginProcessor::maxVoices; ++v)
        {
            const int interval = juce::roundToInt (intervalValue[(size_t) v]->load (std::memory_order_relaxed));
            const int leadNote = juce::roundToInt (lead);
            float pitch = lead;
            bool known = true;

            if (mode == modeScale)
                pitch = (float) harmonyNote (nearestInMask (lead, laneMask), interval, laneMask);
            else if (mode == modeFixed)
                pitch = (float) (leadNote + interval);
            else if (mode == modeChord)
                pitch = (float) nearestInMask ((double) (leadNote + interval), laneMask);
            else
            {
                // MIDI decides, and none is playing: they wait in a row by the lead.
                pitch = lead + 2.0f * (float) v - 3.0f;
                known = false;
            }

            out[(size_t) v] = pitch;
            sings[(size_t) v] = known;
        }
    }

    void StageView::updateSingers (const PluginProcessor::Readings& r, double dt)
    {
        const auto count = processor.getPitchHistoryCount();
        std::array<float, singers> newest {};

        // The newest few points, so a singer ends where its line ends. With no new points the audio has stopped, and nobody is singing.
        for (long long i = count - 1; count != lastRead && i >= juce::jmax (0ll, count - 6); --i)
        {
            const auto& point = processor.getPitchPoint (i);

            if (newest[(size_t) leadIndex] <= 0.0f)
                newest[(size_t) leadIndex] = point.lead.load (std::memory_order_relaxed);

            for (int v = 0; v < PluginProcessor::maxVoices; ++v)
                if (newest[(size_t) v] <= 0.0f)
                    newest[(size_t) v] = point.voice[(size_t) v].load (std::memory_order_relaxed);
        }

        if (newest[(size_t) leadIndex] > 0.0f)
        {
            sinceVoiced = 0.0;
            lastLeadPitch = newest[(size_t) leadIndex];
        }
        else
        {
            sinceVoiced += dt;
        }

        live = sinceVoiced < 1.0;

        lastRead = count;

        std::array<float, singers> rest {};
        std::array<bool, singers> restKnown {};
        restPitches (rest, restKnown);

        const int count_ = juce::jlimit (1, PluginProcessor::maxVoices, juce::roundToInt (voicesValue->load (std::memory_order_relaxed)));
        const auto& latest = processor.getPitchPoint (juce::jmax (0ll, count - 1));
        const float k = (float) (1.0 - std::exp (-dt / (live ? 0.03 : 0.12)));

        for (int i = 0; i < singers; ++i)
        {
            auto& s = state[(size_t) i];
            const bool lead = i == leadIndex;
            s.pan = lead ? leadPanValue->load (std::memory_order_relaxed) : panValue[(size_t) i]->load (std::memory_order_relaxed);
            s.level = lead ? leadLevelValue->load (std::memory_order_relaxed) : levelValue[(size_t) i]->load (std::memory_order_relaxed);
            s.shown = lead || i < count_;

            // A voice that holds a note with no lead to go on keeps its note, as in a MIDI chord.
            const int noteNow = lead ? r.leadNote : r.voiceNote[(size_t) i];
            const bool singing = live && (newest[(size_t) i] > 0.0f || noteNow >= 0);

            if (singing)
            {
                if (newest[(size_t) i] > 0.0f)
                    s.target = newest[(size_t) i];

                s.known = true;
            }
            else
            {
                s.target = rest[(size_t) i];
                s.known = restKnown[(size_t) i];
            }

            s.pitch = started ? s.pitch + k * (s.target - s.pitch) : s.target;
            if (std::abs (s.target - s.pitch) < 0.002f)
                s.pitch = s.target;

            // Loudness, for the ring. Rises at once, falls gently, so it reads as a pulse and not a flicker.
            const float level = levelAmount (lead ? latest.leadLevel.load (std::memory_order_relaxed)
                                                  : latest.voiceLevel[(size_t) i].load (std::memory_order_relaxed));
            s.loud = juce::jmax (live ? level : 0.0f, s.loud * (float) std::exp (-dt / 0.14));
            if (s.loud < 0.01f)
                s.loud = 0.0f;

            juce::String label;

            if (! s.known)
                label = "-";
            else if (singing && noteNow >= 0)
                label = noteText (noteNow);
            else
                label = noteText (juce::roundToInt (s.target));

            if (label != s.label)
                s.label = label;
        }

        started = true;
    }

    bool StageView::updateRange()
    {
        if (dragging >= 0)
            return false;

        float low = 1000.0f, high = -1000.0f;
        const auto take = [&low, &high] (float p)
        {
            if (p > 0.0f)
            {
                low = juce::jmin (low, p);
                high = juce::jmax (high, p);
            }
        };

        for (const auto& s : state)
            if (s.shown)
                take (s.target);

        const auto count = processor.getPitchHistoryCount();
        float historyLow = 1000.0f, historyHigh = -1000.0f;
        const auto takeHistory = [&take, &historyLow, &historyHigh] (float p)
        {
            take (p);

            if (p > 0.0f)
            {
                historyLow = juce::jmin (historyLow, p);
                historyHigh = juce::jmax (historyHigh, p);
            }
        };

        for (long long i = juce::jmax (0ll, count - visiblePoints); i < count; i += 4)
        {
            const auto& point = processor.getPitchPoint (i);
            takeHistory (point.lead.load (std::memory_order_relaxed));

            for (const auto& v : point.voice)
                takeHistory (v.load (std::memory_order_relaxed));
        }

        linesLow = historyLow;
        linesHigh = historyHigh;

        if (low > high)
            return false;

        const int wantedLow = (int) std::floor (low) - 2, wantedHigh = (int) std::ceil (high) + 2;
        const int wantedSpan = juce::jmax (minSpan, wantedHigh - wantedLow);
        const int span = rangeHigh - rangeLow;

        // Keep the scale still while everything fits with room to spare. Only move when something is about to leave it, or there is far too much room.
        if (wantedLow >= rangeLow && wantedHigh <= rangeHigh && span <= wantedSpan + 10)
            return false;

        const int centre = (wantedLow + wantedHigh) / 2;
        rangeLow = centre - wantedSpan / 2;
        rangeHigh = rangeLow + wantedSpan;
        return true;
    }

    void StageView::advance (double dt)
    {
        const auto r = processor.getReadings();
        const bool bypassNow = bypassValue->load (std::memory_order_relaxed) >= 0.5f;

        if (bypassNow != bypassed)
        {
            bypassed = bypassNow;
            repaint();
        }

        updateKey();
        updateSingers (r, dt);

        // Checking the scale means reading the whole visible history. Three times a second is plenty.
        if (--framesToRangeCheck <= 0)
        {
            framesToRangeCheck = 10;

            if (updateRange())
            {
                needsRebuild = true;
                repaint();
            }
        }

        bool drew = false;

        // The new columns of the lines. Skipped if a rebuild is due, because that draws everything.
        const auto count = processor.getPitchHistoryCount();

        // A host can start the audio over. The history starts over with it, so everything is drawn again.
        if (count < drawnTo)
        {
            needsRebuild = true;
            drawnTo = count;
            lastSung = -1000000;
            repaint (timeArea());
        }

        if (count != drawnTo)
        {
            for (long long i = drawnTo; i < count; ++i)
            {
                const auto& point = processor.getPitchPoint (i);
                const float leadPitch = point.lead.load (std::memory_order_relaxed);
                bool any = leadPitch > 0.0f;

                if (any)
                {
                    linesLow = juce::jmin (linesLow, leadPitch);
                    linesHigh = juce::jmax (linesHigh, leadPitch);
                }

                for (const auto& v : point.voice)
                {
                    const float p = v.load (std::memory_order_relaxed);

                    if (p > 0.0f)
                    {
                        any = true;
                        linesLow = juce::jmin (linesLow, p);
                        linesHigh = juce::jmax (linesHigh, p);
                    }
                }

                if (any)
                    lastSung = i;
            }

            if (! needsRebuild && ring.isValid() && count - drawnTo < visiblePoints)
            {
                juce::Graphics g (ring);
                clearColumns (g, columnFor (drawnTo), columnFor (count));
                drawLines (g, drawnTo, count);
                drawnTo = count;
            }
            else
            {
                needsRebuild = true;
                drawnTo = count;
            }

            // Once the last sung point has run off the left, nothing is moving and nothing needs painting.
            if (count - lastSung <= visiblePoints + 4)
            {
                // Only the rows the lines cover, plus what they covered last frame, so nothing stale is left when the band shrinks.
                const auto time = timeArea();
                const int top = juce::jlimit (0, time.getBottom(), (int) std::floor (yFor (linesHigh)) - 8);
                const int bottom = juce::jlimit (top, time.getBottom(), (int) std::ceil (yFor (linesLow)) + 8);
                const juce::Rectangle<int> band (time.getX(), top, time.getWidth(), bottom - top);
                repaint (band.getUnion (bandShown.isEmpty() ? band : bandShown));
                bandShown = band;
                drew = true;
            }
        }

        const bool empty = count - lastSung > visiblePoints;

        if (empty != emptyShown)
        {
            emptyShown = empty;
            repaint (timeArea());
        }

        // The singers: only if one moved, changed or changed its ring.
        bool changed = false;

        for (auto& s : state)
        {
            const float size = diameterFor (s);

            if (s.shown && (s.pitch != s.target || s.loud > 0.0f || s.spriteLabel != s.label || std::abs (s.spriteSize - size) > 0.01f))
                changed = true;
        }

        std::array<juce::uint32, 2> stamp {};

        for (const auto& s : state)
        {
            stamp[0] = stamp[0] * 31u + (juce::uint32) juce::roundToInt (s.pan) + (s.shown ? 1000u : 0u);
            stamp[1] = stamp[1] * 31u + (juce::uint32) juce::roundToInt (s.level * 10.0f);
        }

        if (changed || stamp != lastStamp)
        {
            lastStamp = stamp;
            repaintStage();
            drew = true;
        }

        moving = drew;
    }

    // ---- the pictures ---------------------------------------------------------------------------------------------

    void StageView::rebuild (float scale)
    {
        cachedScale = scale;
        needsRebuild = false;

        const int w = juce::jmax (1, juce::roundToInt ((float) getWidth() * scale)), h = juce::jmax (1, juce::roundToInt ((float) getHeight() * scale));

        if (background.getWidth() != w || background.getHeight() != h)
            background = juce::Image (juce::Image::RGB, w, h, false);

        drawBackground();

        ringWidth = juce::jmax (1, juce::roundToInt ((float) timeArea().getWidth() * scale));
        ringHeight = h;

        if (ring.getWidth() != ringWidth || ring.getHeight() != ringHeight)
            ring = juce::Image (juce::Image::RGB, ringWidth, ringHeight, false);

        const auto count = processor.getPitchHistoryCount();
        drawnTo = count;
        juce::Graphics g (ring);
        const int leftPixels = juce::roundToInt ((float) timeArea().getX() * scale);
        g.drawImageAt (background, -leftPixels, 0);
        // One point short of a full lap: the oldest segment would land on the newest columns.
        drawLines (g, juce::jmax (0ll, count - visiblePoints + 1), count);
    }

    void StageView::drawBackground()
    {
        juce::Graphics g (background);
        g.addTransform (juce::AffineTransform::scale (cachedScale));

        const auto bounds = getLocalBounds().toFloat();
        const auto stage = stageArea().toFloat();
        const float rowHeight = semitonePixels();

        g.fillAll (tone::base);

        {
            juce::Graphics::ScopedSaveState keep (g);
            juce::Path round;
            round.addRoundedRectangle (bounds, 12.0f);
            g.reduceClipRegion (round);
            g.setColour (tone::well);
            g.fillRect (bounds);
            g.setColour (tone::floor);
            g.fillRect (stage);

            // A row for each note in the key, stronger on the root, running under both views.
            const bool anyMask = laneMask != 0x0FFF;

            for (int note = rangeLow; note <= rangeHigh; ++note)
            {
                const bool isRoot = (note % 12) == laneRoot;
                const bool inKey = inMask (note, laneMask);

                if (anyMask ? ! inKey : ((note % 12) != 0 && ! inMask (note, (std::uint16_t) 0x0AB5)))
                    continue;

                const float y = yFor ((float) note) - rowHeight * 0.5f;
                const auto row = juce::Rectangle<float> (0.0f, y + 0.5f, bounds.getWidth(), juce::jmax (1.0f, rowHeight - 1.0f));
                g.setColour (tone::text.withAlpha (isRoot ? 0.08f : 0.028f));
                g.fillRect (row);
            }

            // Centre of the stage
            g.setColour (tone::text.withAlpha (0.07f));
            g.fillRect (juce::Rectangle<float> (xForPan (0.0f) - 0.5f, 0.0f, 1.0f, bounds.getHeight() - stageFooter));
        }

        // Note names. When rows are tight, only the roots and fifths.
        const bool tight = rowHeight < 11.0f;

        for (int note = rangeLow; note <= rangeHigh; ++note)
        {
            const bool isRoot = (note % 12) == laneRoot;
            const bool inKey = inMask (note, laneMask);
            const bool isFifth = ((note - laneRoot + 120) % 12) == 7;

            // With every note in play, name only the white keys, or the column turns to noise.
            const bool chromatic = laneMask == 0x0FFF;
            const bool named = chromatic ? (tight ? (note % 12) == 0 : inMask (note, (std::uint16_t) 0x0AB5)) : (inKey && (! tight || isRoot || (isFifth && rowHeight >= 7.0f)));

            if (! named)
                continue;

            if (yFor ((float) note) > bounds.getBottom() - stageFooter)
                continue;

            g.setColour (isRoot ? tone::muted : tone::faint);
            g.setFont (font (isRoot ? Weight::medium : Weight::regular, 11.0f));
            const auto label = isRoot || tight ? noteText (note) : juce::String (noteNames[note % 12]);
            g.drawText (label, juce::Rectangle<float> (0.0f, yFor ((float) note) - 8.0f, (float) gutter - 6.0f, 16.0f), juce::Justification::centredRight, false);
        }

        g.setColour (tone::faint);
        g.setFont (font (Weight::regular, 12.0f));
        const float footerY = bounds.getBottom() - stageFooter;
        g.drawText ("Left", juce::Rectangle<float> (stage.getX() + 16.0f, footerY, 60.0f, stageFooter - 4.0f), juce::Justification::centredLeft, false);
        g.drawText ("Right", juce::Rectangle<float> (stage.getRight() - 76.0f, footerY, 60.0f, stageFooter - 4.0f), juce::Justification::centredRight, false);
    }

    int StageView::columnFor (long long point) const noexcept
    {
        // One lap of the picture is exactly the visible points, so the seam never drifts.
        const auto inLap = point % visiblePoints;
        return (int) (inLap * ringWidth / visiblePoints);
    }

    void StageView::clearColumns (juce::Graphics& g, int from, int to)
    {
        const int leftPixels = juce::roundToInt ((float) timeArea().getX() * cachedScale);
        const auto wipe = [&] (int a, int b)
        {
            if (b <= a)
                return;

            juce::Graphics::ScopedSaveState keep (g);
            g.reduceClipRegion (juce::Rectangle<int> (a, 0, b - a, ringHeight));
            g.drawImageAt (background, -leftPixels, 0);
        };

        if (to >= from)
        {
            wipe (from, to);
        }
        else
        {
            wipe (from, ringWidth);
            wipe (0, to);
        }
    }

    void StageView::drawLines (juce::Graphics& g, long long fromPoint, long long toPoint)
    {
        // A column at a time as flat rectangles: far cheaper than stroking a path, and at this density it looks the same.
        // Thickness follows how loud that moment was, so the line reads as the phrase and not only as the pitch.
        const float scale = cachedScale;
        const float rowHeight = semitonePixels() * scale;
        const float top = topAndBottom * scale;
        const auto yPixels = [&] (float pitch) { return top + ((float) rangeHigh - pitch) * rowHeight; };

        for (int series = 0; series < singers; ++series)
        {
            const bool lead = series == leadIndex;
            g.setColour (colourFor (series));
            const float thin = (lead ? 2.0f : 1.5f) * scale, thick = (lead ? 5.0f : 4.0f) * scale;

            const auto rawAt = [&] (long long i)
            {
                const auto& point = processor.getPitchPoint (i);
                return lead ? point.lead.load (std::memory_order_relaxed) : point.voice[(size_t) series].load (std::memory_order_relaxed);
            };

            // What gets drawn at a point: nothing for a scrap shorter than 15 ms, and a 30 ms average, since the tracker's raw pitch
            // has a fuzz of a few cents from period to period that would read as a rough edge and not as anything the voice is doing.
            const auto pitchAt = [&] (long long i)
            {
                if (i < 2 || rawAt (i) <= 0.0f || rawAt (i - 1) <= 0.0f || rawAt (i - 2) <= 0.0f)
                    return 0.0f;

                float sum = 0.0f;
                int count = 0;

                for (long long back = i; back > juce::jmax (0ll, i - 6); --back)
                {
                    const float p = rawAt (back);

                    // Stop at a gap or a real jump, so a note change stays sharp.
                    if (p <= 0.0f || std::abs (p - rawAt (i)) > 0.7f)
                        break;

                    sum += p;
                    ++count;
                }

                return sum / (float) count;
            };

            for (long long i = juce::jmax (1ll, fromPoint); i < toPoint; ++i)
            {
                const float a = pitchAt (i - 1), b = pitchAt (i);

                if (a <= 0.0f || b <= 0.0f)
                    continue;

                const int first = columnFor (i - 1);
                int last = columnFor (i);
                if (last < first)
                    last += ringWidth;

                if (last == first)
                    continue;

                const auto& point = processor.getPitchPoint (i);
                const float amount = levelAmount (lead ? point.leadLevel.load (std::memory_order_relaxed) : point.voiceLevel[(size_t) series].load (std::memory_order_relaxed));
                const float half = 0.5f * (thin + (thick - thin) * amount);
                const float ya = yPixels (a), yb = yPixels (b);
                const float stepY = (yb - ya) / (float) (last - first);

                for (int c = first; c < last; ++c)
                {
                    const float y0 = ya + stepY * (float) (c - first), y1 = y0 + stepY;
                    const int column = c >= ringWidth ? c - ringWidth : c;
                    const float high = juce::jmin (y0, y1) - half, low = juce::jmax (y0, y1) + half;
                    g.fillRect (juce::Rectangle<float> ((float) column, high, 1.0f, low - high));
                }
            }
        }
    }

    void StageView::refreshSprite (Singer& s, juce::Colour colour, float scale)
    {
        const float size = diameterFor (s);
        const bool off = s.level <= -60.0f;
        const float alpha = off ? -1.0f : 1.0f;

        if (s.sprite.isValid() && s.spriteLabel == s.label && std::abs (s.spriteSize - size) < 0.01f && s.spriteAlpha == alpha && cachedScale == spriteScale)
            return;

        s.spriteLabel = s.label;
        s.spriteSize = size;
        s.spriteAlpha = alpha;

        const int pixels = juce::roundToInt ((size + 2.0f) * scale);
        s.sprite = juce::Image (juce::Image::ARGB, pixels, pixels, true);
        juce::Graphics g (s.sprite);
        g.addTransform (juce::AffineTransform::scale (scale));
        const auto b = juce::Rectangle<float> (1.0f, 1.0f, size, size);

        if (off)
        {
            g.setColour (colour.withAlpha (0.7f));
            g.drawEllipse (b.reduced (0.75f), 1.5f);
        }
        else
        {
            g.setColour (colour);
            g.fillEllipse (b);
        }

        g.setColour (off ? colour : tone::ink);
        g.setFont (font (Weight::medium, s.label.length() > 2 ? 11.5f : 12.5f));
        g.drawText (s.label, b, juce::Justification::centred, false);
    }

    void StageView::paint (juce::Graphics& g)
    {
        ++paintCount;
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (needsRebuild || std::abs (scale - cachedScale) > 0.001f || ! background.isValid())
            rebuild (scale);

        const auto clip = g.getClipBounds();
        const auto time = timeArea();

        {
            juce::Graphics::ScopedSaveState keep (g);
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.addTransform (juce::AffineTransform::scale (1.0f / scale));

            const int timeLeft = juce::roundToInt ((float) time.getX() * scale);

            // Two pieces of the tape loop, oldest on the left. Only the rest of the picture needs the background.
            if (clip.intersects (time))
            {
                const int head = columnFor (drawnTo);
                juce::Graphics::ScopedSaveState keepRing (g);
                g.reduceClipRegion (juce::Rectangle<int> (timeLeft, 0, ringWidth, ringHeight));

                // At a window size that is not a whole multiple, the two pieces can meet a hair off the pixel grid. The rows go under the join so it never shows.
                {
                    juce::Graphics::ScopedSaveState keepJoin (g);
                    g.reduceClipRegion (juce::Rectangle<int> (timeLeft + ringWidth - head - 1, 0, 2, ringHeight));
                    g.drawImageAt (background, 0, 0);
                }

                g.drawImageAt (ring, timeLeft - head, 0);
                g.drawImageAt (ring, timeLeft + ringWidth - head, 0);
            }

            if (clip.getX() < time.getX())
            {
                juce::Graphics::ScopedSaveState keepBack (g);
                g.reduceClipRegion (juce::Rectangle<int> (0, 0, timeLeft, ringHeight));
                g.drawImageAt (background, 0, 0);
            }

            if (clip.getRight() > time.getRight())
            {
                juce::Graphics::ScopedSaveState keepBack (g);
                g.reduceClipRegion (juce::Rectangle<int> (timeLeft + ringWidth, 0, background.getWidth() - timeLeft - ringWidth, ringHeight));
                g.drawImageAt (background, 0, 0);
            }
        }

        if (emptyShown && clip.intersects (time))
        {
            g.setColour (tone::faint);
            g.setFont (font (Weight::regular, 15.0f));
            g.drawText (shownMode == modeMidi || shownMode == modeChannel ? "Sing, and hold notes on this track's MIDI. Each voice draws its own line."
                                                                          : "Play your vocal. Each voice draws its own line around the lead.",
                        time.toFloat().withTrimmedBottom (stageFooter), juce::Justification::centred, false);
        }

        // The singers. A thin line runs from the end of each voice's line to the singer, so it is plain which is which.
        const float stageLeft = (float) stageArea().getX();
        const float alpha = bypassed ? 0.4f : 1.0f;
        const float dim = live || emptyShown ? 1.0f : 0.8f;

        for (int pass = 0; pass < singers; ++pass)
        {
            const auto& s = state[(size_t) pass];

            if (! s.shown || s.level <= -60.0f)
                continue;

            const auto at = singerPosition (pass);
            g.setColour (colourFor (pass).withAlpha ((live ? 0.5f : 0.18f) * alpha));
            g.fillRect (juce::Rectangle<float> (stageLeft, at.y - 0.5f, juce::jmax (0.0f, at.x - stageLeft), 1.0f));
        }

        for (int i = 0; i < singers; ++i)
        {
            auto& s = state[(size_t) i];

            if (! s.shown)
                continue;

            const auto colour = colourFor (i);
            refreshSprite (s, colour, scale);
            const auto at = singerPosition (i);
            const float size = s.spriteSize;

            // Loudness, as a flat ring that swells. There is no glow, just a paler disc behind the singer.
            if (s.loud > 0.0f && ! bypassed)
            {
                const float grow = size * 0.5f + 2.0f + size * 0.28f * s.loud;
                g.setColour (colour.withAlpha (0.20f));
                g.fillEllipse (at.x - grow, at.y - grow, grow * 2.0f, grow * 2.0f);
            }

            if (i == hovered || i == highlight || i == dragging)
            {
                const float ring_ = size * 0.5f + 4.0f;
                g.setColour (colour);
                g.drawEllipse (at.x - ring_, at.y - ring_, ring_ * 2.0f, ring_ * 2.0f, 1.5f);
            }

            juce::Graphics::ScopedSaveState keep (g);
            g.addTransform (juce::AffineTransform::scale (1.0f / scale));
            g.setOpacity (alpha * (i == leadIndex ? 1.0f : dim));
            const float half = (size * 0.5f + 1.0f);
            g.drawImageAt (s.sprite, juce::roundToInt ((at.x - half) * scale), juce::roundToInt ((at.y - half) * scale));
        }

        spriteScale = scale;

        if (bypassed)
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText ("Bypassed", stageArea().toFloat().removeFromTop (34.0f), juce::Justification::centred, false);
        }
    }

    // ---- moving singers -------------------------------------------------------------------------------------------

    int StageView::singerAt (juce::Point<float> p) const
    {
        int best = -1;
        float bestDistance = 1.0e9f;

        for (int i = 0; i < singers; ++i)
        {
            const auto& s = state[(size_t) i];

            if (! s.shown)
                continue;

            const float distance = p.getDistanceFrom (singerPosition (i));

            if (distance <= diameterFor (s) * 0.5f + 5.0f && distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
        }

        return best;
    }

    void StageView::showHintFor (int singer)
    {
        if (onHint == nullptr)
            return;

        if (singer == leadIndex)
            onHint ("The lead, your own voice. Drag left or right to place it. Its size is Lead Level.");
        else if (singer >= 0)
            onHint ("Voice " + juce::String (singer + 1) + ". Drag left or right to pan, up or down to change its note. Its size is its Level.");
        else
            onHint ({});
    }

    void StageView::mouseMove (const juce::MouseEvent& e)
    {
        const int now = singerAt (e.position);

        if (now != hovered)
        {
            hovered = now;
            setMouseCursor (now >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaintStage();
        }

        showHintFor (now);
    }

    void StageView::mouseExit (const juce::MouseEvent&)
    {
        if (hovered != -1 && dragging < 0)
        {
            hovered = -1;
            repaintStage();
        }
    }

    void StageView::mouseDown (const juce::MouseEvent& e)
    {
        dragging = singerAt (e.position);

        if (dragging < 0)
            return;

        const int mode = juce::jlimit (0, numModes - 1, (int) modeValue->load (std::memory_order_relaxed));
        dragMovesInterval = dragging != leadIndex && mode != modeMidi && mode != modeChannel;
        panParam (dragging)->beginChangeGesture();

        if (dragMovesInterval)
        {
            auto* interval = intervalParam (dragging);
            interval->beginChangeGesture();
            dragInterval = juce::roundToInt (interval->convertFrom0to1 (interval->getValue()));
        }

        dragStartY = e.position.y;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        repaintStage();
    }

    void StageView::mouseDrag (const juce::MouseEvent& e)
    {
        if (dragging < 0)
            return;

        const auto stage = stageArea().toFloat();
        const float pan = juce::jlimit (-100.0f, 100.0f, (e.position.x - stage.getX() - stageMargin) / (stage.getWidth() - 2.0f * stageMargin) * 200.0f - 100.0f);
        auto* panP = panParam (dragging);
        panP->setValueNotifyingHost (panP->convertTo0to1 (std::abs (pan) < 4.0f ? 0.0f : pan));

        if (dragMovesInterval)
        {
            const int moved = juce::roundToInt ((dragStartY - e.position.y) / semitonePixels());
            auto* interval = intervalParam (dragging);
            interval->setValueNotifyingHost (interval->convertTo0to1 ((float) juce::jlimit (-24, 24, dragInterval + moved)));
        }

        repaintStage();
    }

    void StageView::mouseUp (const juce::MouseEvent&)
    {
        if (dragging < 0)
            return;

        panParam (dragging)->endChangeGesture();

        if (dragMovesInterval)
            intervalParam (dragging)->endChangeGesture();

        dragging = -1;
        setMouseCursor (hovered >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaintStage();
    }

    void StageView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const int hit = singerAt (e.position);

        if (hit < 0)
            return;

        const auto reset = [] (juce::RangedAudioParameter* p)
        {
            if (p != nullptr)
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->getDefaultValue());
                p->endChangeGesture();
            }
        };

        reset (panParam (hit));
        reset (intervalParam (hit));
    }
}
