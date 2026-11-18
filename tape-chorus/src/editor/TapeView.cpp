#include "TapeView.h"

namespace ui
{
    namespace
    {
        constexpr float lineSpacing = 6.0f;    // pixels between the points of a line, at design size
        constexpr float barPitch = 5.0f;
        constexpr float barWidth = 3.0f;
        constexpr float lineThickness = 1.55f;
        constexpr float gapBelow = 0.33f;      // a voice dipped under this draws a gap: worn tape
        constexpr double runOnAfterChange = 3.0, runOnAfterSound = 3.0;

        // Everything the picture's shape depends on.
        const char* const watchedIds[] = { "voices", "rate", "depth", "delay", "spread", "wow", "flutter", "random", "wowRate", "flutterRate", "bypass" };

        float levelAmount (float db)
        {
            return juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);
        }

        float pinchCurve (float t)
        {
            t = juce::jlimit (0.0f, 1.0f, t);
            return t * t * (3.0f - 2.0f * t);
        }
    }

    TapeView::TapeView (PluginProcessor& p)
        : processor (p)
    {
        setOpaque (true);   // JUCE then skips everything behind this on every frame
        setComponentID ("tape");
        setTitle ("Tape");
        hint = "Each line is one voice, pale left, aqua right. The bars are what is on the tape. Drag up or down for Depth, left or right for Rate.";

        for (size_t i = 0; i < watched.size(); ++i)
            watched[i] = p.apvts.getRawParameterValue (watchedIds[i]);

        seconds = juce::jlimit (0.4f, 7.5f, 2.25f / juce::jmax (0.05f, watched[1]->load()));
        scaleMs = scaleShownMs = wantedScaleMs();
        lastCount = p.getEngine().getTraceCount();
        lastFingerprint = settingsFingerprint();
        updateCaptions();
    }

    uint32_t TapeView::settingsFingerprint() const
    {
        uint32_t h = 2166136261u;
        for (auto* value : watched)
        {
            const float v = value->load (std::memory_order_relaxed);
            uint32_t bits = 0;
            std::memcpy (&bits, &v, sizeof bits);
            h = (h ^ bits) * 16777619u;
        }
        return h;
    }

    float TapeView::wantedScaleMs() const
    {
        const int voices = juce::jlimit (1, TapeChorusEngine::maxVoices, juce::roundToInt (watched[0]->load()));
        float reach = 0.0f;
        for (int v = 0; v < voices; ++v)
            reach = juce::jmax (reach, std::abs (TapeChorusEngine::voiceOffsetMs (voices, v)));

        const float reachAll = reach
                             + watched[4]->load() * 0.01f * TapeChorusEngine::maxSpreadOffsetMs
                             + watched[2]->load()
                             + watched[5]->load() * 0.01f * TapeChorusEngine::maxWowMs
                             + watched[6]->load() * 0.01f * TapeChorusEngine::maxFlutterMs
                             + watched[7]->load() * 0.01f * TapeChorusEngine::maxRandomMs;

        // The engine shrinks everything together when the delay is too short to hold it.
        const float room = juce::jmax (0.5f, watched[3]->load() - (float) processor.getMinDelayMs());
        const float wanted = juce::jmin (reachAll, room) * 1.04f;

        for (float step : { 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f })
            if (wanted <= step)
                return step;

        return 24.0f;
    }

    void TapeView::updateCaptions()
    {
        const auto scale = juce::String (juce::roundToInt (scaleMs)) + " ms";
        const auto span = seconds >= 0.95f ? juce::String (seconds, 1) + " s" : juce::String (juce::roundToInt (seconds * 1000.0f / 10.0f) * 10) + " ms";

        if (scale != scaleCaption || span != timeCaption)
        {
            scaleCaption = scale;
            timeCaption = span;
            repaint (0, 0, getWidth(), captionBand);
            repaint (0, getHeight() - captionBand, getWidth(), captionBand);
        }
    }

    void TapeView::resized()
    {
        const float h = (float) getHeight();
        reelRadius = 39.4f + 0.092f * h;
        leftX = 32.0f + reelRadius;
        rightX = (float) getWidth() - 32.0f - reelRadius;
        centreY = reelY = h * 0.5f;
        halfHeight = h * 0.5f - (float) captionBand - 2.0f;

        const float hubR = reelRadius * 0.27f, toothW = hubR * 0.17f, toothL = hubR * 0.3f, holeR = hubR * 0.62f;
        teeth.clear();
        for (int i = 0; i < 3; ++i)
        {
            juce::Path one;
            one.addRectangle (holeR - toothL, -toothW * 0.5f, toothL + 1.0f, toothW);
            teeth.addPath (one, juce::AffineTransform::rotation ((float) i * juce::MathConstants<float>::twoPi / 3.0f));
        }

        rebuild (processor, mode == Mode::live);
    }

    float TapeView::pinchAt (float x) const noexcept
    {
        // The lines gather as they reach a reel, like tape leaving a spool. Only the first and last stretch beside a reel are drawn in, and the middle is true to scale.
        const float fromReel = juce::jmin (x - leftX, rightX - x);
        const float atReel = juce::jmin (1.0f, 0.86f * reelRadius / juce::jmax (1.0f, halfHeight));
        return atReel + (1.0f - atReel) * pinchCurve ((fromReel - reelRadius * 0.6f) / (reelRadius * 1.9f));
    }

    void TapeView::addLine (juce::Path& path, const float* lineYs, const uint8_t* lineOn, int count)
    {
        // A ribbon filled as one shape. Cheaper than a stroke, and the thickness holds on the steep parts.
        const float half = lineThickness * 0.5f;

        const auto thickAt = [&] (int i)
        {
            const int a = juce::jmax (0, i - 1), b = juce::jmin (count - 1, i + 1);
            const float dx = xs[(size_t) b] - xs[(size_t) a];
            const float slope = dx > 0.0f ? (lineYs[b] - lineYs[a]) / dx : 0.0f;
            return half * juce::jmin (3.0f, std::sqrt (1.0f + slope * slope));
        };

        int i = 0;
        while (i < count)
        {
            while (i < count && lineOn[i] == 0) ++i;
            int end = i;
            while (end < count && lineOn[end] != 0) ++end;

            if (end - i >= 2)
            {
                path.startNewSubPath (xs[(size_t) i], lineYs[i] - thickAt (i));
                for (int k = i + 1; k < end; ++k)
                    path.lineTo (xs[(size_t) k], lineYs[k] - thickAt (k));
                for (int k = end - 1; k >= i; --k)
                    path.lineTo (xs[(size_t) k], lineYs[k] + thickAt (k));
                path.closeSubPath();
            }

            i = end;
        }
    }

    void TapeView::rebuild (const PluginProcessor& p, bool fromEngine)
    {
        leftPath.clear();
        rightPath.clear();
        barRects.clear();

        if (getWidth() <= 0 || getHeight() <= 0)
            return;

        const auto& engine = p.getEngine();
        const double hz = (double) engine.getTraceHz();
        const float plotWidth = rightX - leftX;
        const double rowsShown = juce::jmin ((double) seconds * hz, (double) TapeChorusEngine::traceRows - 64.0);
        const float pixelsPerRow = (float) ((double) plotWidth / rowsShown);
        const int step = juce::jmax (1, (int) std::ceil (lineSpacing / pixelsPerRow));
        const float pixelsPerMs = halfHeight / scaleShownMs;
        const int voices = juce::jlimit (1, TapeChorusEngine::maxVoices, juce::roundToInt (watched[0]->load()));
        const float gainUnit = std::sqrt ((float) voices);

        // Points stay on the same rows from frame to frame, so the lines glide and do not shimmer.
        const double whole = std::floor (position / (double) step) * (double) step;
        const float offset = (float) (position - whole) * pixelsPerRow;
        const float pointPixels = (float) step * pixelsPerRow;

        // Only the points that can be seen. The ends sit under the reels.
        const float seenFrom = leftX + reelRadius * 0.45f, seenTo = rightX - reelRadius * 0.45f;
        const int firstPoint = juce::jmax (0, (int) std::floor ((seenFrom - leftX - offset) / pointPixels));
        const int count = juce::jmax (2, (int) std::ceil ((seenTo - leftX - offset) / pointPixels) - firstPoint + 1);

        xs.resize ((size_t) count);
        ys.resize ((size_t) count);
        on.resize ((size_t) count);
        pinch.resize ((size_t) count);

        for (int k = 0; k < count; ++k)
        {
            xs[(size_t) k] = leftX + offset + (float) (firstPoint + k) * pointPixels;
            pinch[(size_t) k] = pinchAt (xs[(size_t) k]);
        }

        if (fromEngine)
        {
            const auto newest = (uint32_t) (int64_t) whole;
            const float centre = engine.getTraceRow (engine.getTraceCount() - 1).centreMs.load (std::memory_order_relaxed);
            const int averaged = juce::jmin (step, 6);

            for (int side = 1; side >= 0; --side)
            {
                auto& path = side == 0 ? leftPath : rightPath;

                for (int v = 0; v < voices; ++v)
                {
                    bool any = false;

                    for (int k = 0; k < count; ++k)
                    {
                        // The average of the rows this point stands for, so fast shake cannot alias into a jagged line.
                        float d = 0.0f, g = 1.0f;
                        bool silent = false;
                        for (int r = 0; r < averaged; ++r)
                        {
                            const auto& row = engine.getTraceRow (newest - (uint32_t) ((firstPoint + k) * step + r));
                            const float here = row.delayMs[(size_t) side][(size_t) v].load (std::memory_order_relaxed);
                            silent = silent || here <= 0.0f;
                            d += here;
                            g = juce::jmin (g, row.voiceGain[(size_t) v].load (std::memory_order_relaxed) * gainUnit);
                        }
                        d /= (float) averaged;
                        ys[(size_t) k] = centreY - (d - centre) * pixelsPerMs * pinch[(size_t) k];
                        on[(size_t) k] = (! silent && g > gapBelow) ? 1 : 0;
                        any = any || on[(size_t) k] != 0;
                    }

                    if (any)
                        addLine (path, ys.data(), on.data(), count);
                }
            }

            // What is on the tape, as flat bars.
            const int barStep = juce::jmax (1, juce::roundToInt (barPitch / pixelsPerRow));
            const double barWhole = std::floor (position / (double) barStep) * (double) barStep;
            const auto barNewest = (uint32_t) (int64_t) barWhole;
            const float barOffset = (float) (position - barWhole) * pixelsPerRow;
            const float barPixels = (float) barStep * pixelsPerRow;
            const float tallest = halfHeight * 0.94f;

            for (int b = 0;; ++b)
            {
                const float x = leftX + barOffset + (float) b * barPixels;
                if (x > seenTo)
                    break;
                if (x < seenFrom)
                    continue;

                float most = 0.0f;
                for (int r = 0; r < barStep; ++r)
                    most = juce::jmax (most, engine.getTraceRow (barNewest - (uint32_t) (b * barStep + r)).tapePeak.load (std::memory_order_relaxed));

                if (most > 0.002f)
                {
                    const float db = 20.0f * std::log10 (most);
                    const float height = tallest * pinchAt (x) * juce::jlimit (0.0f, 1.0f, (db + 54.0f) / 54.0f);
                    if (height > 0.5f)
                        barRects.addWithoutMerging ({ x, centreY - height, barWidth, height * 2.0f });
                }
            }
        }
        else
        {
            // No audio is running, so draw what the settings will do. The same layout of voices as the engine, without the random parts.
            const float rate = watched[1]->load(), depth = watched[2]->load(), delay = watched[3]->load(), spread = watched[4]->load() * 0.01f;
            float reach = 0.0f;
            for (int v = 0; v < voices; ++v)
                reach = juce::jmax (reach, std::abs (TapeChorusEngine::voiceOffsetMs (voices, v)));
            const float excursion = reach + spread * TapeChorusEngine::maxSpreadOffsetMs + depth;
            const float room = juce::jmax (0.0f, delay - (float) p.getMinDelayMs());
            const float shrink = excursion > room && excursion > 0.0f ? room / excursion : 1.0f;

            for (int k = 0; k < count; ++k)
                on[(size_t) k] = 1;

            for (int side = 1; side >= 0; --side)
            {
                auto& path = side == 0 ? leftPath : rightPath;
                const float sideOffset = (side == 0 ? -1.0f : 1.0f) * spread * TapeChorusEngine::maxSpreadOffsetMs;

                for (int v = 0; v < voices; ++v)
                {
                    const float base = TapeChorusEngine::voiceOffsetMs (voices, v) + sideOffset;
                    const float phase = TapeChorusEngine::voicePhaseTurns (voices, v) + (0.5f / (float) voices) * spread * (float) side;

                    for (int k = 0; k < count; ++k)
                    {
                        const float age = (float) ((double) ((firstPoint + k) * step) / hz);
                        const float l = std::sin (juce::MathConstants<float>::twoPi * (phase - age * rate));
                        ys[(size_t) k] = centreY - shrink * (base + depth * l) * pixelsPerMs * pinch[(size_t) k];
                    }

                    addLine (path, ys.data(), on.data(), count);
                }
            }
        }
    }

    TapeView::~TapeView()
    {
        endDrag();
    }

    TapeView::Zone TapeView::zoneAt (juce::Point<float> at) const
    {
        const juce::Point<float> left (leftX, reelY), right (rightX, reelY);
        if (at.getDistanceFrom (left) <= reelRadius)   return Zone::leftReel;
        if (at.getDistanceFrom (right) <= reelRadius)  return Zone::rightReel;
        return Zone::tape;
    }

    void TapeView::mouseMove (const juce::MouseEvent& e)
    {
        const auto zone = zoneAt (e.position);
        setMouseCursor (zone == Zone::tape ? juce::MouseCursor::UpDownLeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
        hint = zone == Zone::leftReel  ? "The left reel winds out with your input level. Drag it up or down for Input."
             : zone == Zone::rightReel ? "The right reel winds out with the output level. Drag it up or down for Output."
                                       : "Each line is one voice, pale left, aqua right. The bars are what is on the tape. Drag up or down for Depth, left or right for Rate.";
        if (onHintChanged != nullptr)
            onHintChanged (hint);
    }

    void TapeView::endDrag()
    {
        for (auto* target : { &dragX, &dragY })
        {
            if (*target != nullptr)
                (*target)->endChangeGesture();
            *target = nullptr;
        }
    }

    void TapeView::mouseDown (const juce::MouseEvent& e)
    {
        endDrag();
        const auto zone = zoneAt (e.position);
        dragX = zone == Zone::tape ? processor.apvts.getParameter ("rate") : nullptr;
        dragY = processor.apvts.getParameter (zone == Zone::tape ? "depth" : (zone == Zone::leftReel ? "inputGain" : "outputGain"));
        dragXStart = dragX != nullptr ? dragX->getValue() : 0.0f;
        dragYStart = dragY->getValue();
        if (dragX != nullptr) dragX->beginChangeGesture();
        dragY->beginChangeGesture();
    }

    void TapeView::mouseDrag (const juce::MouseEvent& e)
    {
        const auto offset = e.getOffsetFromDragStart().toFloat();
        const auto setIfMoved = [] (juce::RangedAudioParameter& p, float value)
        {
            value = juce::jlimit (0.0f, 1.0f, value);
            if (std::abs (value - p.getValue()) > 1.0e-5f)
                p.setValueNotifyingHost (value);
        };

        if (dragX != nullptr) setIfMoved (*dragX, dragXStart + offset.x / 600.0f);
        if (dragY != nullptr) setIfMoved (*dragY, dragYStart - offset.y / 300.0f);
    }

    void TapeView::mouseUp (const juce::MouseEvent&)
    {
        endDrag();
    }

    void TapeView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const auto zone = zoneAt (e.position);
        const auto reset = [this] (const char* id)
        {
            auto* p = processor.apvts.getParameter (id);
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        };

        if (zone == Zone::tape) { reset ("rate"); reset ("depth"); }
        else reset (zone == Zone::leftReel ? "inputGain" : "outputGain");
    }

    bool TapeView::tick (double dt)
    {
        const auto& engine = processor.getEngine();
        const uint32_t count = engine.getTraceCount();
        const uint32_t fresh = count - lastCount;
        const bool running = fresh > 0;
        const double hz = (double) engine.getTraceHz();

        // Has anything been written to the tape lately?
        if (running)
        {
            float most = 0.0f;
            for (uint32_t i = 0, n = juce::jmin (fresh, 256u); i < n; ++i)
                most = juce::jmax (most, engine.getTraceRow (count - 1 - i).tapePeak.load (std::memory_order_relaxed));
            silentFor = most > 1.0e-4f ? 0.0 : silentFor + dt;
            staleFor = 0.0;
            everRan = true;
        }
        else
        {
            staleFor += dt;
        }
        lastCount = count;

        const uint32_t fingerprint = settingsFingerprint();
        const bool changed = fingerprint != lastFingerprint;
        lastFingerprint = fingerprint;
        sinceChange = changed ? 0.0 : sinceChange + dt;

        const bool nowBypassed = watched[10]->load (std::memory_order_relaxed) > 0.5f;
        const bool bypassChanged = nowBypassed != bypassed;
        bypassed = nowBypassed;

        const float wantedSeconds = juce::jlimit (0.4f, 7.5f, 2.25f / juce::jmax (0.05f, watched[1]->load (std::memory_order_relaxed)));
        scaleMs = wantedScaleMs();
        const bool settling = std::abs (seconds - wantedSeconds) > 0.002f || std::abs (scaleShownMs - scaleMs) > 0.002f;

        const bool live = running && (silentFor < runOnAfterSound || sinceChange < runOnAfterChange);
        const bool preview = ! live && staleFor > 0.25 && (changed || settling || needsPreview || (! everRan && mode != Mode::preview));

        // Scale and time move together, smoothly.
        if (live || preview)
        {
            const float blend = 1.0f - std::exp (-(float) dt / 0.09f);
            seconds += (wantedSeconds - seconds) * blend;
            scaleShownMs += (scaleMs - scaleShownMs) * blend;
            if (std::abs (seconds - wantedSeconds) < 0.002f) seconds = wantedSeconds;
            if (std::abs (scaleShownMs - scaleMs) < 0.002f) scaleShownMs = scaleMs;
            updateCaptions();
        }

        bool drew = false;

        if (live)
        {
            // Run steadily between the audio's blocks, and lean toward what has really been written.
            const double aim = (double) count - 1.0 - hz * 0.03;
            const bool wrapped = aim + 0.5 * hz < position;
            if (mode != Mode::live || wrapped || std::abs (aim - position) > 0.3 * hz)
                position = aim;
            else
                position = juce::jmin ((double) count - 1.0, position + dt * hz + (aim - position - dt * hz) * 0.15);

            mode = Mode::live;
            needsPreview = false;
            rebuild (processor, true);
            drew = true;
        }
        else if (preview)
        {
            mode = Mode::preview;
            needsPreview = false;
            rebuild (processor, false);
            drew = true;
        }
        else if (mode == Mode::live)
        {
            mode = Mode::nothing;   // the tape has stopped. The last picture stays, and nothing repaints
        }

        // Reels. The left winds out with the input, the right with the output. They turn with the tape and take up its real wobble.
        const float inDb = processor.getInputLevel()->get(), outDb = processor.getOutputLevel()->get();
        const float blendUp = 1.0f - std::exp (-(float) dt / 0.012f), blendDown = 1.0f - std::exp (-(float) dt / 0.12f);
        const auto follow = [&] (float& shown, float aim) { shown += (aim - shown) * (aim > shown ? blendUp : blendDown); };
        const float inBefore = inLevel, outBefore = outLevel;
        const bool active = staleFor < 0.25;
        follow (inLevel, active ? levelAmount (inDb) : 0.0f);
        follow (outLevel, active ? levelAmount (outDb) : 0.0f);
        const bool inOverNow = active && inDb > 0.0f, outOverNow = active && outDb > 0.0f;
        const bool levelMoved = std::abs (inLevel - inBefore) * reelRadius > 0.12f || std::abs (outLevel - outBefore) * reelRadius > 0.12f
                             || inOverNow != inOver || outOverNow != outOver;
        inOver = inOverNow; outOver = outOverNow;

        if (live)
        {
            const auto& newest = engine.getTraceRow (count - 1);
            const auto& older = engine.getTraceRow (count - 1 - (uint32_t) juce::jmax (1.0, hz * 0.04));
            const float slope = (newest.delayMs[0][0].load (std::memory_order_relaxed) - older.delayMs[0][0].load (std::memory_order_relaxed)) / 0.04f;   // ms a second
            const float wobble = juce::jlimit (-0.7f, 0.7f, slope * 0.03f);
            angle = std::fmod (angle + (float) dt * 2.4f * (1.0f + wobble), juce::MathConstants<float>::twoPi);
        }

        if (bypassChanged)
            repaint (0, 0, getWidth(), captionBand);   // the key at the top follows the lines' colours

        const int top = captionBand, height = juce::jmax (0, getHeight() - captionBand * 2);

        if (drew || levelMoved || bypassChanged)
            repaint (0, top, getWidth(), height);

        return drew || levelMoved || bypassChanged;
    }

    void TapeView::paintReel (juce::Graphics& g, float centreX, float amount, bool over, float reelAngle) const
    {
        const float r = reelRadius, hubR = r * 0.27f;
        const auto circle = [&] (float radius) { return juce::Rectangle<float> (centreX - radius, reelY - radius, radius * 2.0f, radius * 2.0f); };

        g.setColour (over ? tone::hot : tone::flange);
        g.fillEllipse (circle (r));

        if (over)
        {
            g.setColour (tone::flange);
            g.fillEllipse (circle (r - 2.5f));
        }

        // The wound tape, which is the level.
        if (amount > 0.0f)
        {
            g.setColour (tone::pack);
            g.fillEllipse (circle (hubR + (r - 2.5f - hubR) * amount));
        }

        // The hub, with the three teeth of a spool turning in its hole.
        g.setColour (tone::mint);
        g.fillEllipse (circle (hubR));
        g.setColour (tone::base);
        g.fillEllipse (circle (hubR * 0.62f));
        g.setColour (tone::mint);
        g.fillPath (teeth, juce::AffineTransform::rotation (reelAngle).translated (centreX, reelY));
    }

    void TapeView::paint (juce::Graphics& g)
    {
        ++paintCount;
        g.fillAll (tone::base);

        const auto bandClip = [&] (int y) { return g.clipRegionIntersects ({ 0, y, getWidth(), captionBand }); };

        if (bandClip (0) || bandClip (getHeight() - captionBand))
        {
            // Only when the scale changes: text is the dearest thing on this screen.
            const float textLeft = leftX + reelRadius + 14.0f, textWidth = rightX - leftX - reelRadius * 2.0f - 28.0f;
            g.setFont (font (Weight::medium, 11.5f));
            g.setColour (tone::faint);
            g.drawText ("+" + scaleCaption, juce::Rectangle<float> (textLeft, 1.0f, 120.0f, (float) captionBand - 2.0f), juce::Justification::centredLeft, false);
            g.drawText (minusSign() + scaleCaption, juce::Rectangle<float> (textLeft, (float) (getHeight() - captionBand) + 1.0f, 120.0f, (float) captionBand - 2.0f), juce::Justification::centredLeft, false);
            g.drawText (timeCaption + " of tape", juce::Rectangle<float> (textLeft + textWidth - 120.0f, (float) (getHeight() - captionBand) + 1.0f, 120.0f, (float) captionBand - 2.0f), juce::Justification::centredRight, false);

            const float keyRight = textLeft + textWidth;
            g.drawText ("Right", juce::Rectangle<float> (keyRight - 36.0f, 1.0f, 36.0f, (float) captionBand - 2.0f), juce::Justification::centredRight, false);
            g.drawText ("Left", juce::Rectangle<float> (keyRight - 100.0f, 1.0f, 30.0f, (float) captionBand - 2.0f), juce::Justification::centredRight, false);
            g.setColour (bypassed ? tone::rest : tone::aqua);
            g.fillRect (keyRight - 62.0f, (float) captionBand * 0.5f - 0.5f, 22.0f, 2.0f);
            g.setColour (bypassed ? tone::rest : tone::mint);
            g.fillRect (keyRight - 126.0f, (float) captionBand * 0.5f - 0.5f, 22.0f, 2.0f);
        }

        // The middle of the sweep, which is the Delay Time.
        g.setColour (tone::flange);
        g.fillRect (leftX, centreY - 0.5f, rightX - leftX, 1.0f);

        g.setColour (tone::bars);
        g.fillRectList (barRects);

        {
            juce::Graphics::ScopedSaveState clipped (g);
            g.reduceClipRegion (0, captionBand, getWidth(), getHeight() - captionBand * 2);
            const float alpha = mode == Mode::preview && ! everRan ? 0.8f : 1.0f;
            g.setColour ((bypassed ? tone::rest : tone::aqua).withAlpha (alpha));
            g.fillPath (rightPath);
            g.setColour ((bypassed ? tone::rest : tone::mint).withAlpha (alpha));
            g.fillPath (leftPath);
        }

        paintReel (g, leftX, inLevel, inOver, angle);
        paintReel (g, rightX, outLevel, outOver, angle + 0.9f);
    }
}
