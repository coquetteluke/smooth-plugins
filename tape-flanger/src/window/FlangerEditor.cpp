#include "FlangerEditor.h"
#include "../FlangerDsp.h"

using namespace ui;

namespace
{
    constexpr int margin = 24;
    constexpr int headerTop = 16, headerHeight = 44;
    constexpr int fieldTop = 76, fieldBottom = 506;
    constexpr int stripWidth = 50;
    constexpr int cellsTop = 524, cellsHeight = 66;
    constexpr int footerTop = 620, footerHeight = 30;
    constexpr int sideWidth = 296, sideGap = 28;

    // Each frame costs about 3 million instructions before anything is drawn, so this is the number that sets the window's CPU.
    // 30 is an even cadence on a 60 or 120 Hz screen. See ~/smooth-resonance/docs/window-cpu.md.
    constexpr double maxFramesPerSecond = 30.0;

    const char* const lfoHint = "Drag across the field to set how far it sweeps. Click any value to type it. Double-click to reset.";
    const char* const manualHint = "Drag across the field to ride Flange. Hold Shift to slow your hand. Double-click to go back to 0.";

    const juce::String minusSign (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
}

// ---- preset name with next and previous arrows ------------------------------------------------------------------

class PresetBox final : public juce::Component,
                        public Hinted
{
public:
    explicit PresetBox (PluginProcessor& p) : processor (p)
    {
        hint = "Starting points. Click the name for the full list.";
        setComponentID ("presets");
        setTitle ("Presets");
    }

    void refresh()
    {
        if (processor.getCurrentProgram() != shown)
        {
            shown = processor.getCurrentProgram();
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (zone != none ? tone::raisedLit : tone::raised);
        g.fillRect (b);
        g.setColour (tone::text);
        g.setFont (font (Weight::regular, 14.0f));
        g.drawText (processor.getProgramName (processor.getCurrentProgram()), b.reduced (arrowWidth, 0.0f), juce::Justification::centred, true);

        const auto chevron = [&] (float cx, float direction, bool lit)
        {
            juce::Path p;
            p.startNewSubPath (cx - 2.5f * direction, b.getCentreY() - 5.0f);
            p.lineTo (cx + 2.5f * direction, b.getCentreY());
            p.lineTo (cx - 2.5f * direction, b.getCentreY() + 5.0f);
            g.setColour (lit ? tone::text : tone::muted);
            g.strokePath (p, juce::PathStrokeType (1.5f));
        };

        chevron (arrowWidth * 0.5f, -1.0f, zone == previous);
        chevron (b.getRight() - arrowWidth * 0.5f, 1.0f, zone == next);
    }

    void mouseMove (const juce::MouseEvent& e) override { setZone (zoneAt (e.x)); }
    void mouseExit (const juce::MouseEvent&) override   { setZone (none); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int count = processor.getNumPrograms();
        const int current = processor.getCurrentProgram();
        const auto hit = zoneAt (e.x);

        if (hit == previous)
        {
            processor.setCurrentProgram ((current + count - 1) % count);
        }
        else if (hit == next)
        {
            processor.setCurrentProgram ((current + 1) % count);
        }
        else
        {
            juce::PopupMenu menu;
            for (int i = 0; i < count; ++i)
                menu.addItem (i + 1, processor.getProgramName (i), true, i == current);

            juce::Component::SafePointer<PresetBox> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()),
                                [safe] (int chosen)
                                {
                                    if (safe != nullptr && chosen > 0)
                                        safe->processor.setCurrentProgram (chosen - 1);
                                });
        }

        refresh();
    }

private:
    enum Zone { none, previous, name, next };
    static constexpr float arrowWidth = 32.0f;

    Zone zoneAt (int x) const noexcept
    {
        if ((float) x < arrowWidth) return previous;
        if ((float) x > (float) getWidth() - arrowWidth) return next;
        return name;
    }

    void setZone (Zone z)
    {
        if (z != zone)
        {
            zone = z;
            repaint();
        }
    }

    PluginProcessor& processor;
    Zone zone = none;
    int shown = -1;
};

// ---- the big number: where the wet deck is right now ------------------------------------------------------------

class DeckReadout final : public juce::Component,
                          public Hinted
{
public:
    DeckReadout()
    {
        hint = "Where the wet deck is against the dry one, right now. Through zero is the moment it flips from early to late.";
        setOpaque (true);
        setComponentID ("readout");
    }

    void update (float ms, float leadCrossing, float gate, bool manualDry)
    {
        const float absMs = std::abs (ms);
        const int side = absMs < 0.005f ? 0 : (ms < 0.0f ? -1 : 1);
        const auto number = juce::String (absMs, absMs >= 10.0f ? 1 : 2);
        const bool hot = leadCrossing > 0.2f || absMs < 0.06f;
        const bool dry = manualDry && gate < 0.02f;

        if (side != shownSide || hot != shownHot || dry != shownDry)
        {
            shownNumber = number;
            shownSide = side;
            shownHot = hot;
            shownDry = dry;
            everyOther = false;
            repaint();
        }
        else if (number != shownNumber)
        {
            // Half the frame rate: a number changing faster than this can not be read, and only its own digits need drawing.
            everyOther = ! everyOther;
            if (everyOther)
            {
                shownNumber = number;
                repaint (getLocalBounds().withSizeKeepingCentre ((int) numberWidth, getHeight()));
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (std::abs (scale - tileScale) > 0.01f)
            buildTiles (scale);

        g.fillAll (tone::base);

        const auto b = getLocalBounds().toFloat();
        const auto wordFont = font (Weight::regular, 13.0f);
        g.setFont (wordFont);
        const float wordWidth = 44.0f;
        const auto numberBox = b.withSizeKeepingCentre (numberWidth, b.getHeight());
        const auto word = [&] (const char* text, juce::Rectangle<float> area, juce::Justification j, bool on)
        {
            if (! g.clipRegionIntersects (area.getSmallestIntegerContainer()))
                return;

            g.setColour (on ? (shownHot ? tone::violetPale : tone::text) : tone::faint);
            g.drawText (text, area, j, false);
        };

        word ("Early", juce::Rectangle<float> (numberBox.getX() - wordWidth - 6.0f, 0.0f, wordWidth, b.getHeight() - 7.0f), juce::Justification::bottomRight, shownSide < 0);
        word ("Late", juce::Rectangle<float> (numberBox.getRight() + 6.0f, 0.0f, wordWidth, b.getHeight() - 7.0f), juce::Justification::bottomLeft, shownSide > 0);

        if (shownDry)
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::medium, 32.0f));
            g.drawText ("Dry", numberBox, juce::Justification::centred, false);
            return;
        }

        // Fixed width digit tiles: the number cannot jitter sideways, and drawing an image is far cheaper than drawing text.
        const auto& tiles = shownHot ? hotTiles : plainTiles;
        juce::String text = shownNumber;
        float width = 0.0f;
        for (int i = 0; i < text.length(); ++i)
            width += text[i] == '.' ? dotAdvance : digitAdvance;

        float x = numberBox.getCentreX() - width * 0.5f;

        for (int i = 0; i < text.length(); ++i)
        {
            const auto c = text[i];
            const int index = c == '.' ? 10 : (int) (c - '0');
            const float advance = c == '.' ? dotAdvance : digitAdvance;
            g.drawImage (tiles[(size_t) index], juce::Rectangle<float> (x, 0.0f, advance, b.getHeight()), juce::RectanglePlacement::stretchToFit);
            x += advance;
        }
    }

private:
    void buildTiles (float scale)
    {
        tileScale = scale;
        const auto f = font (Weight::medium, 34.0f);
        digitAdvance = juce::GlyphArrangement::getStringWidth (f, "0") + 1.0f;
        dotAdvance = juce::GlyphArrangement::getStringWidth (f, ".") + 1.0f;
        const int h = juce::roundToInt ((float) getHeight() * scale);

        const auto make = [&] (std::array<juce::Image, 11>& set, juce::Colour colour)
        {
            for (int i = 0; i < 11; ++i)
            {
                const float advance = i == 10 ? dotAdvance : digitAdvance;
                juce::Image tile (juce::Image::ARGB, juce::jmax (1, juce::roundToInt (advance * scale)), h, true, juce::NativeImageType());
                juce::Graphics g (tile);
                g.addTransform (juce::AffineTransform::scale ((float) tile.getWidth() / advance, (float) h / (float) getHeight()));
                g.setColour (colour);
                g.setFont (f);
                g.drawText (i == 10 ? juce::String (".") : juce::String (i), juce::Rectangle<float> (0.0f, 0.0f, advance, (float) getHeight()), juce::Justification::centred, false);
                set[(size_t) i] = tile;
            }
        };

        make (plainTiles, tone::text);
        make (hotTiles, tone::violetPale);
    }

    static constexpr float numberWidth = 100.0f;
    std::array<juce::Image, 11> plainTiles, hotTiles;
    float tileScale = 0.0f, digitAdvance = 20.0f, dotAdvance = 10.0f;
    juce::String shownNumber;
    int shownSide = 2;
    bool shownHot = false, shownDry = false, everyOther = false;
};

// ---- a picture of the sweep shape, cached until a parameter moves ----------------------------------------------

class SweepPicture final : public juce::Component,
                           public Hinted
{
public:
    explicit SweepPicture (Apvts& s)
    {
        hint = "The shape of the sweep over two cycles. The middle line is zero: every time a line goes through it, the sound flips.";
        shape = s.getRawParameterValue ("lfoshape");
        symmetry = s.getRawParameterValue ("lfosymmetry");
        phase = s.getRawParameterValue ("lfophase");
        swing = s.getRawParameterValue ("lfoswing");
        spread = s.getRawParameterValue ("spread");
        delay = s.getRawParameterValue ("delay");
        depth = s.getRawParameterValue ("depth");
        setComponentID ("sweep");
    }

    void refresh()
    {
        const std::array<float, 7> now { shape->load(), symmetry->load(), phase->load(), swing->load(), spread->load(), delay->load(), depth->load() };
        if (now != drawn)
        {
            drawn = now;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tone::well);
        g.fillRect (b);
        const float rise = 0.5f + 0.45f * drawn[1] * 0.01f, offset = drawn[2] / 360.0f, share = 0.5f + 0.25f * drawn[3] * 0.01f, spreadTurns = drawn[4] / 360.0f;
        const float del = drawn[5], dep = drawn[6];
        const float reach = juce::jmax (1.0f, del + dep) * 1.1f;
        const float cy = b.getCentreY(), half = b.getHeight() * 0.5f - 2.0f;

        g.setColour (tone::text.withAlpha (0.24f));
        g.fillRect (juce::Rectangle<float> (b.getX(), cy - 0.5f, b.getWidth(), 1.0f));

        for (int ch = 1; ch >= 0; --ch)
        {
            juce::Path p;
            const int n = juce::jmax (2, getWidth());

            for (int i = 0; i <= n; ++i)
            {
                const double t = (double) i / (double) n;   // the pair of cycles
                const double cyclePos = flanger::swingCycle (t, (double) share);
                const double w = flanger::lfoWave (cyclePos + (double) offset + (ch == 0 ? 0.0 : (double) spreadTurns), (double) rise, (double) drawn[0] * 0.01);
                const float ms = del + dep * (float) w;
                const float y = cy - ms / reach * half;
                if (i == 0) p.startNewSubPath (b.getX() + (float) i, y);
                else        p.lineTo (b.getX() + (float) i, y);
            }

            g.setColour (ch == 0 ? tone::text : tone::violet);
            g.strokePath (p, juce::PathStrokeType (ch == 0 ? 1.5f : 1.25f));
        }
    }

private:
    std::atomic<float>* shape, *symmetry, *phase, *swing, *spread, *delay, *depth;
    std::array<float, 7> drawn {};
};

// ---- a small text button for opening Advanced ---------------------------------------------------------------------

class TextPill final : public juce::Button,
                       public Hinted
{
public:
    TextPill (const juce::String& textToShow, const juce::String& hintText) : juce::Button (textToShow)
    {
        hint = hintText;
    }

    void setOpenState (bool nowOpen)
    {
        if (open != nowOpen)
        {
            open = nowOpen;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isOver, bool) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (open ? tone::steel : (isOver ? tone::raisedLit : tone::raised));
        g.fillRect (b);
        g.setColour (open ? tone::well : (isOver ? tone::text : tone::muted));
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (getButtonText(), b, juce::Justification::centred, false);
    }

private:
    bool open = false;
};

// ---- the panel, everything laid out at the design size ------------------------------------------------------------

class FlangerEditor::Panel final : public juce::Component
{
public:
    Panel (FlangerEditor& e, PluginProcessor& p)
        : owner (e),
          processor (p),
          field (p),
          presets (p),
          mode (p.apvts, "mode", "LFO sweeps by itself. Manual holds the wet deck where you put it, for riding a flange by hand."),
          inputStrip (p.apvts, "inputgain", "In", "Input Gain. Before everything, so more of it pushes the Saturation harder. Drag up and down."),
          outputStrip (p.apvts, "outputgain", "Out", "Output Gain. After everything. Drag up and down."),
          rate (p.apvts, "rate", "Rate", "How fast it sweeps.", ParamCell::Size::main, ParamCell::Unit::hz),
          division (p.apvts, "division", "Length", "How long one sweep lasts, in time with the song.", ParamCell::Size::main, ParamCell::Unit::words),
          depth (p.apvts, "depth", "Depth", "How far the wet deck moves to each side. You can also drag across the field.", ParamCell::Size::main, ParamCell::Unit::ms),
          flange (p.apvts, "flange", "Flange", "Where the wet deck sits. Plus is late, minus is early. Exactly 0 takes the wet out.", ParamCell::Size::main, ParamCell::Unit::flangeMs),
          fade (p.apvts, "fade", "Fade", "How long the wet takes to fade in when Flange leaves 0, and out when it returns.", ParamCell::Size::main, ParamCell::Unit::ms),
          feedback (p.apvts, "feedback", "Feedback", "Sharpens the comb into a metal ring. Below 0 it hollows out at the crossing.", ParamCell::Size::main, ParamCell::Unit::signedPercent),
          saturation (p.apvts, "saturation", "Saturation", "Tape grit going onto the wet deck, inside the feedback.", ParamCell::Size::main, ParamCell::Unit::percent),
          toneCell (p.apvts, "tone", "Tone", "Brighter or darker wet, above 4 kHz.", ParamCell::Size::main, ParamCell::Unit::signedDb),
          spread (p.apvts, "spread", "Spread", "Puts the right side's sweep ahead of the left, the second line in the field. LFO only.", ParamCell::Size::main, ParamCell::Unit::degrees),
          mix (p.apvts, "mix", "Mix", "Dry and wet. 50% gives the deepest notches.", ParamCell::Size::main, ParamCell::Unit::percent),
          sync (p.apvts, "sync", "Sync", "Take the sweep speed from the song's tempo."),
          invert (p.apvts, "invert", "Invert phase", "Flips the wet. At Mix 50 the sound cancels right at the crossing."),
          bypass (p.apvts, "bypass", "Bypass", "Fades the wet out and keeps the same delay."),
          advancedButton ("Advanced", "Sweep shape, tape drift and cuts on the wet."),
          sweepPicture (p.apvts),
          lfoShape (p.apvts, "lfoshape", "Shape", "Triangle at 0, Sine at 50, Square at 100.", ParamCell::Size::compact, ParamCell::Unit::words),
          lfoSymmetry (p.apvts, "lfosymmetry", "Symmetry", "How much of each cycle is spent rising.", ParamCell::Size::compact, ParamCell::Unit::signedPercent),
          lfoPhase (p.apvts, "lfophase", "Phase", "Moves where the sweep starts. Synced, it lands the crossing on a chosen beat.", ParamCell::Size::compact, ParamCell::Unit::degrees),
          lfoSwing (p.apvts, "lfoswing", "Swing", "Makes every other cycle longer and the next one shorter.", ParamCell::Size::compact, ParamCell::Unit::signedPercent),
          delayCell (p.apvts, "delay", "Delay", "Moves the whole sweep later. Far enough and it never crosses zero: a classic flanger.", ParamCell::Size::compact, ParamCell::Unit::ms),
          wowDepth (p.apvts, "wowdepth", "Wow", "Slow tape speed drift on the wet deck.", ParamCell::Size::compact, ParamCell::Unit::ms),
          wowRate (p.apvts, "wowrate", "Wow rate", "How fast the wow drifts.", ParamCell::Size::compact, ParamCell::Unit::hz),
          flutterDepth (p.apvts, "flutterdepth", "Flutter", "Quicker tape speed wobble on the wet deck.", ParamCell::Size::compact, ParamCell::Unit::ms),
          flutterRate (p.apvts, "flutterrate", "Flutter rate", "How fast the flutter wobbles.", ParamCell::Size::compact, ParamCell::Unit::hz),
          randomDepth (p.apvts, "randomdepth", "Random", "Smooth random drift on the wet deck.", ParamCell::Size::compact, ParamCell::Unit::ms),
          randomRate (p.apvts, "randomrate", "Random rate", "How fast the random drift wanders.", ParamCell::Size::compact, ParamCell::Unit::hz),
          randomUnsync (p.apvts, "randomunsync", "Unsync", "Gives left and right their own random drift, which widens the sound."),
          lowCut (p.apvts, "lowcut", "Low cut", "Keeps the flange out of the low end. Wet only.", ParamCell::Size::compact, ParamCell::Unit::frequencyOrOff),
          highCut (p.apvts, "highcut", "High cut", "Keeps the flange off S sounds. Wet only.", ParamCell::Size::compact, ParamCell::Unit::frequencyOrOff),
          feedbackComp (p.apvts, "feedbackcomp", "Hold level", "Feedback Compensation. On holds the level as Feedback rises. Off is louder and rings harder.")
    {
        setOpaque (true);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &field, &presets, &readout, &mode, &inputStrip, &outputStrip, &rate, &division, &depth, &flange, &fade,
                                                                            &feedback, &saturation, &toneCell, &spread, &mix, &sync, &invert, &bypass, &advancedButton })
            addAndMakeVisible (c);

        for (juce::Component* c : advancedControls())
            addChildComponent (c);

        hintLine.setFont (font (Weight::regular, 13.0f));
        hintLine.setColour (juce::Label::textColourId, tone::muted);
        hintLine.setJustificationType (juce::Justification::centredLeft);
        hintLine.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (hintLine);

        advancedButton.onClick = [this] { owner.setAdvancedOpen (! owner.isAdvancedOpen(), true); };
        addMouseListener (this, true);

        restHint();
    }

    ~Panel() override { removeMouseListener (this); }

    std::vector<juce::Component*> advancedControls()
    {
        return { &sweepPicture, &lfoShape, &lfoSymmetry, &lfoPhase, &lfoSwing, &delayCell, &wowDepth, &wowRate, &flutterDepth, &flutterRate,
                 &randomDepth, &randomRate, &randomUnsync, &lowCut, &highCut, &feedbackComp };
    }

    void restHint()
    {
        hintLine.setText (mode.getSelected() == 1 ? manualHint : lfoHint, juce::dontSendNotification);
    }

    void mouseEnter (const juce::MouseEvent& e) override { hintFrom (e.eventComponent); }

    void mouseExit (const juce::MouseEvent&) override
    {
        const auto under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
        if (under == nullptr || ! isParentOf (under))
            restHint();
        else
            hintFrom (under);
    }

    void hintFrom (juce::Component* c)
    {
        for (; c != nullptr && c != this; c = c->getParentComponent())
            if (auto* h = dynamic_cast<Hinted*> (c))
                if (h->hint.isNotEmpty())
                {
                    hintLine.setText (h->hint, juce::dontSendNotification);
                    return;
                }

        restHint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::base);

        if (g.clipRegionIntersects ({ margin, headerTop, 160, headerHeight }))
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::medium, 23.0f));
            g.drawText ("Tape Flanger", juce::Rectangle<int> (margin, headerTop, 170, headerHeight), juce::Justification::centredLeft, false);
        }

        if (advancedAmount > 0.01f)
        {
            g.setColour (tone::muted.withMultipliedAlpha (advancedAmount));
            g.setFont (font (Weight::medium, 13.0f));
            for (const auto& head : headings)
                g.drawText (head.text, head.area, juce::Justification::centredLeft, false);
        }
    }

    void resized() override { layout(); }

    void layout()
    {
        const int width = getWidth();
        const int side = juce::roundToInt ((float) (sideWidth + sideGap) * advancedAmount);
        const int contentRight = width - margin - side;

        // Header. The number stays over the middle of the window, so it does not move when Advanced opens.
        presets.setBounds (margin + 164, headerTop + 6, 216, 32);
        mode.setBounds (width - margin - 156, headerTop + 6, 156, 32);
        readout.setBounds (width / 2 - 104, headerTop, 208, headerHeight);

        // The field, framed by input and output.
        inputStrip.setBounds (margin, fieldTop, stripWidth, fieldBottom - fieldTop - CombField::axisHeight + 28);
        outputStrip.setBounds (contentRight - stripWidth, fieldTop, stripWidth, fieldBottom - fieldTop - CombField::axisHeight + 28);
        field.setBounds (margin + stripWidth + 10, fieldTop, contentRight - margin - 2 * stripWidth - 20, fieldBottom - fieldTop);

        // One row of main controls. The first two change with Mode.
        const bool manual = mode.getSelected() == 1;
        const bool synced = sync.getToggleState();
        rate.setVisible (! manual && ! synced);
        division.setVisible (! manual && synced);
        depth.setVisible (! manual);
        sync.setVisible (! manual);
        flange.setVisible (manual);
        fade.setVisible (manual);
        spread.setAlpha (manual ? 0.45f : 1.0f);   // still a live parameter, it just does nothing in Manual

        ParamCell* row[] = { manual ? &flange : (synced ? &division : &rate), manual ? &fade : &depth, &feedback, &mix, &saturation, &toneCell, &spread };
        const int count = (int) std::size (row);
        const int gap = 26;
        const int cellWidth = (contentRight - margin - gap * (count - 1)) / count;

        for (int i = 0; i < count; ++i)
            row[i]->setBounds (margin + i * (cellWidth + gap), cellsTop, cellWidth, cellsHeight);

        const int syncWidth = 44;
        sync.setBounds (margin + cellWidth - syncWidth, cellsTop, syncWidth, 20);
        rate.setHeaderInset (syncWidth);
        division.setHeaderInset (syncWidth);

        // Footer
        const int y = footerTop;
        int right = width - margin;
        advancedButton.setBounds (right - 92, y, 92, footerHeight);
        right -= 92 + 8;
        bypass.setBounds (right - 76, y, 76, footerHeight);
        right -= 76 + 8;
        invert.setBounds (right - 104, y, 104, footerHeight);
        right -= 104 + 16;
        hintLine.setBounds (margin - 3, y, right - margin + 3, footerHeight);

        // Advanced slides in from the right.
        const int sideX = width - margin - sideWidth + juce::roundToInt ((float) (sideWidth + sideGap) * (1.0f - advancedAmount));
        const bool open = advancedAmount > 0.01f;

        for (auto* c : advancedControls())
            c->setVisible (open);

        headings.clear();
        int top = fieldTop;
        const int colGap = 20, colWidth = (sideWidth - colGap) / 2, rowHeight = 44, rowGap = 8;
        const auto heading = [&] (const char* text)
        {
            headings.push_back ({ text, juce::Rectangle<int> (sideX, top, sideWidth, 18) });
            top += 24;
        };
        const auto pair = [&] (juce::Component& a, juce::Component* b)
        {
            a.setBounds (sideX, top, colWidth, rowHeight);
            if (b != nullptr)
                b->setBounds (sideX + colWidth + colGap, top, colWidth, rowHeight);
            top += rowHeight + rowGap;
        };

        heading ("Sweep shape");
        sweepPicture.setBounds (sideX, top, sideWidth, 40);
        top += 48;
        pair (lfoShape, &lfoSymmetry);
        pair (lfoPhase, &lfoSwing);
        delayCell.setBounds (sideX, top, sideWidth, rowHeight);
        top += rowHeight + rowGap;

        top += 8;
        heading ("Tape drift");
        randomUnsync.setBounds (sideX + sideWidth - 64, top - 24, 64, 20);
        pair (wowDepth, &wowRate);
        pair (flutterDepth, &flutterRate);
        pair (randomDepth, &randomRate);

        top += 8;
        heading ("Wet only");
        feedbackComp.setBounds (sideX + sideWidth - 84, top - 24, 84, 20);
        pair (lowCut, &highCut);
    }

    struct Heading { juce::String text; juce::Rectangle<int> area; };

    FlangerEditor& owner;
    PluginProcessor& processor;
    juce::SharedResourcePointer<ui::Assets> assets;

    CombField field;
    PresetBox presets;
    DeckReadout readout;
    ParamSwitch mode;
    GainStrip inputStrip, outputStrip;
    ParamCell rate, division, depth, flange, fade, feedback, saturation, toneCell, spread, mix;
    ParamToggle sync, invert, bypass;
    TextPill advancedButton;
    SweepPicture sweepPicture;
    ParamCell lfoShape, lfoSymmetry, lfoPhase, lfoSwing, delayCell, wowDepth, wowRate, flutterDepth, flutterRate, randomDepth, randomRate;
    ParamToggle randomUnsync;
    ParamCell lowCut, highCut;
    ParamToggle feedbackComp;
    juce::Label hintLine;

    std::vector<Heading> headings;
    float advancedAmount = 0.0f;
    int modeShown = -1;
    bool syncShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
};

// ---- the editor ---------------------------------------------------------------------------------------------------

FlangerEditor::FlangerEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p)
{
    setLookAndFeel (&lookAndFeel);
    panel = std::make_unique<Panel> (*this, p);
    addAndMakeVisible (*panel);
    setOpaque (true);   // saves clearing the window under every frame

    using Proc = PluginProcessor;
    const double ratio = (double) designWidth / (double) designHeight;
    setResizable (true, true);
    setResizeLimits (Proc::minWindowWidth, juce::roundToInt (Proc::minWindowWidth / ratio), Proc::maxWindowWidth, juce::roundToInt (Proc::maxWindowWidth / ratio));
    getConstrainer()->setFixedAspectRatio (ratio);

    panel->advancedAmount = p.advancedOpen.load (std::memory_order_relaxed) ? 1.0f : 0.0f;

    const int savedWidth = juce::jlimit (Proc::minWindowWidth, Proc::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    constructed = true;

    tick (1.0 / 30.0);

    // At most one frame per screen refresh, and never over the cap on faster screens.
    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
    {
        const double elapsed = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / 60.0;
        lastFrameTime = timestampSeconds;
        sinceDrawn += elapsed;

        // Spend whole refreshes against a running budget, so the average lands on the cap at any screen rate.
        frameBudget = juce::jmin (frameBudget + elapsed, 0.05);

        if (frameBudget + 0.001 < 1.0 / maxFramesPerSecond)
            return;

        frameBudget -= 1.0 / maxFramesPerSecond;
        const double step = sinceDrawn;
        sinceDrawn = 0.0;
        tick (step);
    });
}

FlangerEditor::~FlangerEditor()
{
    vblank.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    panel.reset();
    setLookAndFeel (nullptr);
}

void FlangerEditor::paint (juce::Graphics& g)
{
    // The panel covers everything but the sliver rounding can leave at some window sizes.
    g.setColour (tone::base);
    g.fillRect (getWidth() - 2, 0, 2, getHeight());
    g.fillRect (0, getHeight() - 2, getWidth(), 2);
}

void FlangerEditor::resized()
{
    panel->setBounds (0, 0, designWidth, designHeight);
    panel->setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));

    if (constructed)
        processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

bool FlangerEditor::isAdvancedOpen() const noexcept { return processorRef.advancedOpen.load (std::memory_order_relaxed); }
ui::CombField& FlangerEditor::getField() noexcept   { return panel->field; }
juce::Component& FlangerEditor::getPanel() noexcept { return *panel; }

void FlangerEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processorRef.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);

    if (! animate)
    {
        panel->advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
        panel->layout();
        panel->repaint();
    }
}

void FlangerEditor::tick (double dtSeconds)
{
    auto& p = *panel;
    p.field.advance (dtSeconds);

    const auto r = processorRef.getReadings();
    const bool manual = p.mode.getSelected() == 1;
    const bool mono = processorRef.getActiveOutputChannels() < 2;
    p.readout.update (r.deckOffsetMs[0], p.field.getLeadCrossing(), r.wetGate, manual);
    p.inputStrip.setLevels (r.inputPeak[0], r.inputPeak[1], mono, dtSeconds);
    p.outputStrip.setLevels (r.outputPeak[0], r.outputPeak[1], mono, dtSeconds);
    p.presets.refresh();
    p.advancedButton.setOpenState (isAdvancedOpen());

    if (p.advancedAmount > 0.01f)
        p.sweepPicture.refresh();

    // The first two controls follow Mode and Sync.
    const int modeNow = p.mode.getSelected();
    const bool syncNow = p.sync.getToggleState();

    if (modeNow != p.modeShown || syncNow != p.syncShown)
    {
        p.modeShown = modeNow;
        p.syncShown = syncNow;
        p.layout();
        p.restHint();
        p.repaint (0, cellsTop - 4, designWidth, cellsHeight + 8);
    }

    // Advanced slides in about a sixth of a second. The picture is stretched until it lands, then worked out again.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;

    if (p.advancedAmount != target)
    {
        const float next = p.advancedAmount + (target - p.advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        p.advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        p.field.setHoldPicture (p.advancedAmount != target);
        p.layout();
        p.repaint();
    }
}

void FlangerEditor::previewHover (const juce::String& paramId)
{
    if (auto* c = panel->findChildWithID (paramId))
    {
        if (auto* cell = dynamic_cast<ParamCell*> (c))
            cell->setForcedHover (true);

        panel->hintFrom (c);
    }
}
