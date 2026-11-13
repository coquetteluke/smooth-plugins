#include "TuneFxEditor.h"

using namespace smoothtune;
using namespace fxui;
using Kind = ParamSlider::Kind;

namespace
{
    const char* const defaultHint = "Drag a control to change it. Click a value to type one. Double-click resets.";

    juce::String noteName (int midi) { return midi < 0 ? juce::String ("no note") : juce::String (noteNames[midi % 12]) + juce::String (midi / 12 - 1); }

    // Design size layout, from the top down.
    constexpr int margin = 24, headerHeight = 64;
    constexpr int heroTop = 64, heroHeight = 186;
    constexpr int stripLeft = 316, stripRight = 958;    // the note reading sits to the right of it
    constexpr int rowTop = 264, rowHeight = 80 + ModuleRow::tabDepth;
    constexpr int panelTop = rowTop + rowHeight, footerHeight = 32, footerBottom = 12;
    constexpr int panelBottom = TuneFxEditor::designHeight - footerBottom - footerHeight - 12;
    constexpr int panelHeight = panelBottom - panelTop;

    // Inside the panel.
    constexpr int pad = 24, titleWidth = 176, graphWidth = 392, gridGap = 24;
}

// ------------------------------------------------------------------------------------------------- the live reading

// The note coming out, large, and how far your voice was moved to get there, as a bar running out from the middle.
class TuneFxEditor::Reading final : public juce::Component,
                                    public Hinted
{
public:
    explicit Reading (PluginProcessor& p) : processor (p)
    {
        hint = "The note coming out, and how far your voice was moved to get there.";
        setTitle ("Correction");
        setOpaque (true);
        setBufferedToImage (true);      // text is the dearest thing to draw, and macOS merges nearby repaints, so this is only ever drawn when the words change
        bypassParam = processor.apvts.getRawParameterValue ("bypass");
        harmonyParam = processor.apvts.getRawParameterValue ("harmonyOn");
    }

    /** It sits over this component but is not a child of it, or every move of it would redraw this one's cached image too. */
    juce::Component& getBar() noexcept  { return bar; }
    int barPixels() const noexcept       { return bar.pixels; }
    juce::Rectangle<int> barBounds() const  { return getBounds().withTrimmedTop (58).withHeight (12).withTrimmedRight (6); }

    bool advance (double dt, int note)
    {
        const auto r = processor.getReadings();
        const bool nowBypassed = bypassParam->load (std::memory_order_relaxed) >= 0.5f;
        // Bypassed, nothing is moved, so the bar says so (1.2.5; it kept showing the correction the processor still works out).
        const float target = r.voiced && ! nowBypassed ? juce::jlimit (-100.0f, 100.0f, r.correctionCents) : 0.0f;
        shownCents += (target - shownCents) * (1.0f - std::exp (-(float) dt / 0.035f));

        const bool barMoved = bar.show (juce::roundToInt (shownCents * 0.5f));
        clock += dt;

        juce::String first = lineOne, second = lineTwo;
        const bool nowHarmony = harmonyParam->load (std::memory_order_relaxed) >= 0.5f;

        // Words change slowly enough to read.
        if (clock >= 0.125)
        {
            clock = 0.0;
            const int cents = juce::roundToInt (r.correctionCents);
            if (nowBypassed)                 { first = "Bypassed"; second = {}; }
            else if (! r.voiced)             { first = "No pitch"; second = {}; }
            else if (std::abs (cents) < 1)   { first = "On pitch"; second = {}; }
            else
            {
                first = cents > 0 ? "Moved up" : "Moved down";
                second = std::abs (cents) >= 100 ? juce::String (std::abs (cents) / 100.0f, 1) + " semitones" : juce::String (std::abs (cents)) + " cents";
            }
        }

        if (note != noteDrawn || first != lineOne || second != lineTwo || nowHarmony != harmonyDrawn)
        {
            noteDrawn = note;
            lineOne = first;
            lineTwo = second;
            harmonyDrawn = nowHarmony;
            repaint();
            return true;
        }
        return barMoved;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::black);
        auto b = getLocalBounds().toFloat();

        // The note, or nothing at all with no voice, so a gap between words stays quiet.
        const auto noteArea = b.removeFromTop (58.0f);
        if (noteDrawn >= 0)
        {
            g.setColour (tone::white);
            g.setFont (font (Weight::medium, 46.0f));
            g.drawText (noteName (noteDrawn), noteArea, juce::Justification::centredLeft, false);
        }

        b.removeFromTop (14.0f);    // the bar is its own small piece, the only part that moves every frame
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (lineOne, b.removeFromTop (18.0f), juce::Justification::centredLeft, false);
        g.setColour (tone::white);
        g.setFont (font (Weight::medium, 13.0f));
        g.drawText (lineTwo, b.removeFromTop (18.0f), juce::Justification::centredLeft, false);

        // What the lines on the strip are.
        const auto key = [&] (const juce::String& name, juce::Colour colour, float thickness)
        {
            auto r = b.removeFromBottom (18.0f);
            g.setColour (colour);
            g.fillRect (juce::Rectangle<float> (r.getX(), r.getCentreY() - thickness * 0.5f, 14.0f, thickness));
            r.removeFromLeft (20.0f);
            g.setColour (tone::muted);
            g.setFont (font (Weight::regular, 12.0f));
            g.drawText (name, r, juce::Justification::centredLeft, false);
        };
        if (harmonyDrawn)
            key ("Harmony", modules()[3].colour, 2.0f);
        key ("Output", tone::magenta, 3.0f);
        key ("Your voice", tone::white, 1.2f);
    }

private:
    struct Bar final : public juce::Component
    {
        Bar()
        {
            setOpaque (true);
            setInterceptsMouseClicks (false, false);
        }

        bool show (int newPixels)
        {
            if (newPixels == pixels)
                return false;
            pixels = newPixels;
            repaint();
            return true;
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (tone::black);
            const auto track = getLocalBounds().toFloat().withSizeKeepingCentre ((float) getWidth(), 8.0f);
            g.setColour (juce::Colour (0xff212121));
            g.fillRect (track);
            const float mid = track.getCentreX(), reach = track.getWidth() * 0.5f * (float) pixels / 50.0f;
            g.setColour (tone::magenta);
            g.fillRect (juce::Rectangle<float> (juce::jmin (mid, mid + reach), track.getY(), std::abs (reach), track.getHeight()));
            g.setColour (tone::white);
            g.fillRect (mid - 1.0f, 0.0f, 2.0f, (float) getHeight());
        }

        int pixels = 0;
    };

    PluginProcessor& processor;
    Bar bar;
    std::atomic<float>* bypassParam = nullptr, *harmonyParam = nullptr;
    float shownCents = 0.0f;
    int noteDrawn = -1;
    double clock = 1.0;
    bool harmonyDrawn = false;
    juce::String lineOne { "No pitch" }, lineTwo;
};

// ----------------------------------------------------------------------------------------------------- Advanced button

class TuneFxEditor::AdvancedButton final : public juce::Button,
                                           public Hinted
{
public:
    AdvancedButton() : juce::Button ("Advanced")
    {
        hint = "How it listens, MIDI, and your own scale.";
        setClickingTogglesState (false);
    }

    void setState (float openAmount, int changesInside)
    {
        if (openAmount != amount || changesInside != changes)
        {
            amount = openAmount;
            changes = changesInside;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isOver, bool isDown) override
    {
        auto b = getLocalBounds().toFloat();
        const bool lit = isOver || isDown || amount > 0.5f;

        juce::Path chevron;
        const float cx = b.getRight() - 8.0f, cy = b.getCentreY(), flip = 2.0f * amount - 1.0f;
        chevron.startNewSubPath (cx - 4.5f, cy - 2.5f * flip);
        chevron.lineTo (cx, cy + 2.5f * flip);
        chevron.lineTo (cx + 4.5f, cy - 2.5f * flip);
        g.setColour (lit ? tone::white : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        b.removeFromRight (22.0f);

        g.setFont (font (Weight::medium, 13.5f));
        g.drawText (label(), b, juce::Justification::centredRight, false);
    }

    // "Advanced, 2 changed" did not fit beside the arrow at any size and lost its last word (1.2.5).
    juce::String label() const  { return changes > 0 && amount < 0.5f ? "Advanced (" + juce::String (changes) + ")" : juce::String ("Advanced"); }
    int labelRoom() const       { return getWidth() - 22; }

private:
    float amount = 0.0f;
    int changes = 0;
};

// ---------------------------------------------------------------------------------------------------------- your own scale

// One octave as a flat keyboard, one real parameter per key. A click goes round Tune, Remove and Bypass. It lights the note coming out and the scale in use.
class TuneFxEditor::CustomKeys final : public juce::Component,
                                       public Hinted
{
public:
    explicit CustomKeys (PluginProcessor& p) : processor (p)
    {
        hint = "With Scale on Custom, click a key: Tune, Remove or Bypass. Double-click resets.";
        setTitle ("Custom scale");
        for (size_t i = 0; i < 12; ++i)
        {
            params[i] = processor.apvts.getParameter ("note" + juce::String ((int) i));
            attachments[i] = std::make_unique<juce::ParameterAttachment> (*params[i], [this, i] (float v) { modes[i] = juce::roundToInt (v); repaint(); }, nullptr);
            attachments[i]->sendInitialUpdate();
        }
    }

    // True when the scale chosen has changed, so the words beside the keys can follow.
    bool advance (int note)
    {
        const int pitchClass = note >= 0 ? note % 12 : -1;
        const auto scale = processor.getScaleMask();
        const int chosen = juce::roundToInt (processor.apvts.getRawParameterValue ("scale")->load (std::memory_order_relaxed));
        if (pitchClass != lit || scale != maskShown || chosen != scaleChosen)
        {
            lit = pitchClass;
            maskShown = scale;
            const bool wordsChanged = (chosen == scaleCustom) != (scaleChosen == scaleCustom);
            scaleChosen = chosen;
            repaint();
            return wordsChanged;
        }
        return false;
    }

    void cycle (int key)  { attachments[(size_t) key]->setValueAsCompleteGesture ((float) ((modes[(size_t) key] + 1) % 3)); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int key = keyAt (e.position);
        if (key >= 0 && e.getNumberOfClicks() == 1)
            cycle (key);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int key = keyAt (e.position);
        if (key >= 0)
            attachments[(size_t) key]->setValueAsCompleteGesture (0.0f);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int now = keyAt (e.position);
        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = -1;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto ownScale = scaleChosen == scaleCustom;

        for (const bool sharps : { false, true })
        {
            for (int i = 0; i < 12; ++i)
            {
                if (isSharp (i) != sharps)
                    continue;

                const auto r = keyBounds (i);
                const bool inScale = ((maskShown >> i) & 1) != 0;
                const int mode = modes[(size_t) i];
                auto fill = sharps ? juce::Colour (0xff3d3d3d) : juce::Colour (0xffe6e6e6);
                auto ink = sharps ? tone::white : tone::black;

                if (! inScale)
                {
                    fill = sharps ? juce::Colour (0xff1d1d1d) : juce::Colour (0xff333333);
                    ink = tone::muted;
                }

                if (i == lit)
                {
                    fill = tone::magenta;
                    ink = tone::white;
                }

                if (i == hovered)
                    fill = fill.brighter (0.15f);

                if (sharps)
                {
                    g.setColour (tone::raised);
                    g.fillRect (r.expanded (2.0f, 0.0f).withTrimmedTop (-2.0f).withBottom (r.getBottom() + 2.0f));
                }

                g.setColour (fill);
                g.fillRoundedRectangle (r, 3.0f);

                auto text = r.withTrimmedBottom (6.0f);
                g.setColour (ink);
                g.setFont (font (Weight::medium, 12.5f));
                g.drawText (noteNames[i], text.removeFromBottom (16.0f), juce::Justification::centred, false);

                if (ownScale || mode != 0)
                {
                    g.setColour (ink.withMultipliedAlpha (ownScale ? 0.75f : 0.45f));
                    g.setFont (font (Weight::regular, 10.5f));
                    g.drawText (mode == 1 ? "Remove" : (mode == 2 ? "Bypass" : "Tune"), text.removeFromBottom (14.0f), juce::Justification::centred, false);
                }
            }
        }
    }

private:
    static bool isSharp (int i) { return i == 1 || i == 3 || i == 6 || i == 8 || i == 10; }

    juce::Rectangle<float> keyBounds (int i) const
    {
        static constexpr int whiteIndex[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
        const float w = (float) getWidth() / 7.0f, h = (float) getHeight();
        if (! isSharp (i))
            return juce::Rectangle<float> (w * (float) whiteIndex[i], 0.0f, w, h).reduced (1.0f, 0.0f);
        return { w * (float) (whiteIndex[i] + 1) - w * 0.36f, 0.0f, w * 0.72f, h * 0.56f };
    }

    int keyAt (juce::Point<float> p) const
    {
        for (int i = 0; i < 12; ++i)
            if (isSharp (i) && keyBounds (i).contains (p))
                return i;
        for (int i = 0; i < 12; ++i)
            if (! isSharp (i) && keyBounds (i).contains (p))
                return i;
        return -1;
    }

    PluginProcessor& processor;
    std::array<juce::RangedAudioParameter*, 12> params {};
    std::array<std::unique_ptr<juce::ParameterAttachment>, 12> attachments;
    std::array<int, 12> modes {};
    std::uint16_t maskShown = 0x0FFF;
    int lit = -1, hovered = -1, scaleChosen = 0;
};

// --------------------------------------------------------------------------------------------------------- module page

// The controls of one module, in the panel, on a grid of two columns. Hidden unless selected.
struct TuneFxEditor::ModulePage final : public juce::Component
{
    ModulePage (PluginProcessor& p, int index) : module (index), processor (p),
        accent (modules()[(size_t) index].colour),
        onSwitch (p.apvts, modules()[(size_t) index].onId, "On", juce::String ("Turns ") + modules()[(size_t) index].name + " on or off.", accent, "Off")
    {
        setInterceptsMouseClicks (false, true);
        setBufferedToImage (true);
        addAndMakeVisible (onSwitch);

        const auto s = [&] (const char* id, const char* label, const char* text, int span = 1) { add (std::make_unique<ParamSlider> (p.apvts, id, label, text, Kind::main, accent), span, 64); };
        const auto big = [&] (const char* id, const char* label, const char* text, int span = 1) { add (std::make_unique<ParamSlider> (p.apvts, id, label, text, Kind::lead, accent), span, 92); };
        const auto t = [&] (const char* id, const char* label, const char* text) { add (std::make_unique<ParamToggle> (p.apvts, id, label, text, accent), 1, 34, true); };
        const auto c = [&] (const char* id, const char* label, const char* text, int span = 1, int rows = 1) { add (std::make_unique<ParamSegments> (p.apvts, id, label, text, accent, rows), span, 22 + 34 * rows, rows > 1); };
        const auto m = [&] (const char* id, const char* label, const char* text) { add (std::make_unique<ParamMenu> (p.apvts, id, label, text), 1, 56); };

        switch (index)
        {
            case 0:
                big ("pitchSemitones", "Semitones", "Moves the voice in semitones. 7 is a fifth up, 12 is an octave.", 2);
                big ("pitchOctave", "Octave", "Moves the voice in whole octaves.", 2);
                t ("pitchFormants", "Moves Formants", "Off keeps the character of the voice. On gives the chipmunk or monster sound.");
                break;
            case 1:
                c ("motionPattern", "Pattern", "The shape each note walks through. It stays in key.", 2, 2);
                m ("motionRate", "Rate", "How fast it steps, in time with the song.");
                s ("motionGlide", "Glide", "How long it takes to slide from step to step.");
                break;
            case 2:
                big ("throatLength", "Length", "100% is your own throat. Higher sounds bigger, lower sounds smaller.", 2);
                break;
            case 3:
                c ("harmonyVoices", "Voices", "One added voice, or two spread left and right.");
                t ("harmonyInKey", "In Key", "Moves each voice to the nearest note in the scale.");
                s ("harmonyInterval1", "Voice 1", "How far from the lead, in semitones. 7 is a fifth, 12 an octave.");
                s ("harmonyInterval2", "Voice 2", "How far from the lead, in semitones. Only heard with two voices.");
                s ("harmonyLevel", "Level", "How loud the added voices are.");
                s ("harmonyWidth", "Width", "How far apart two voices spread.");
                break;
            case 4:
                big ("doublerLevel", "Level", "How loud the copies are.");
                big ("doublerWidth", "Width", "How far apart the copies spread.");
                big ("doublerTiming", "Timing", "How late the copies come in. It wanders a little, like a real double.");
                big ("doublerDetune", "Detune", "How far out of tune the copies are, one up and one down.");
                break;
            case 5:
                c ("vocoderWave", "Wave", "The synth sound under the voice.", 2);
                big ("vocoderMix", "Mix", "The voice stays whole up to 50. Above that, the voice fades and only the tone is left.");
                big ("vocoderOctave", "Octave", "Puts the tone above or below the voice.");
                break;
            case 6:
                c ("distType", "Type", "Tube is warm, Clip is hard, Fold is wild.", 2);
                s ("distDrive", "Drive", "How hard the voice is pushed.");
                s ("distTone", "Tone", "Takes the top off the distortion.");
                s ("distMix", "Mix", "How much of the distorted voice you hear.");
                break;
            case 7:
                c ("filterType", "Type", "High Pass cuts lows, Low Pass cuts highs, Band Pass does both.", 2);
                s ("filterCutoff", "Cutoff", "Where the filter starts cutting.");
                s ("filterResonance", "Resonance", "Boosts right at the cutoff, for a sharper sound.");
                s ("filterSweepDepth", "Sweep Depth", "How far the cutoff swings. Zero holds it still.");
                m ("filterSweepRate", "Sweep Rate", "How long one swing takes, in time with the song.");
                break;
            case 8:
                big ("ringFrequency", "Frequency", "The tone the voice is multiplied by. Low is a tremble, high is a bell.", 2);
                big ("ringMix", "Mix", "How much of the ring sound you hear.", 2);
                break;
            case 9:
                m ("delayTime", "Time", "How far apart the repeats are, in time with the song.");
                t ("delayPingPong", "Ping Pong", "Bounces the repeats between left and right.");
                s ("delayFeedback", "Feedback", "How many repeats, and how slowly they fade.");
                s ("delayTone", "Tone", "Darkens each repeat.");
                s ("delayMix", "Mix", "How loud the repeats are.");
                break;
            default:
                c ("reverbType", "Type", "Plate is smooth, Room is close, Hall is big.", 2);
                s ("reverbDecay", "Decay", "How long the space rings.");
                s ("reverbPreDelay", "Pre-Delay", "A gap before the space starts, so the voice stays clear.");
                s ("reverbMix", "Mix", "How much of the space you hear.");
                break;
        }
    }

    void add (std::unique_ptr<juce::Component> c, int span, int height, bool bottomAligned = false)
    {
        addAndMakeVisible (*c);
        cells.push_back ({ std::move (c), span, height, bottomAligned });
    }

    void paint (juce::Graphics& g) override
    {
        const auto& m = modules()[(size_t) module];
        auto column = juce::Rectangle<float> ((float) pad, (float) pad, (float) titleWidth, (float) (getHeight() - pad * 2));

        g.setColour (accent);
        g.setFont (font (Weight::medium, 30.0f));
        g.drawText (m.name, column.removeFromTop (38.0f), juce::Justification::centredLeft, false);

        column.removeFromTop (6.0f);
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.5f));
        juce::AttributedString about;
        about.setText (m.about);
        about.setFont (font (Weight::regular, 13.5f));
        about.setColour (tone::muted);
        about.setLineSpacing (3.0f);
        about.draw (g, column.withTrimmedBottom (44.0f));
    }

    void resized() override
    {
        onSwitch.setBounds (pad, getHeight() - pad - 34, 76, 34);

        const int x = pad + titleWidth + 16 + graphWidth + gridGap;
        const int width = getWidth() - pad - x, cell = (width - gridGap) / 2;

        // Rows fill the height evenly, so a module with few controls sits the same way as one with many.
        int rows = 0, col = 0, lastHeight = 0;
        std::vector<int> heights;
        for (auto& c : cells)
        {
            if (col + c.span > 2) { heights.push_back (lastHeight); col = 0; lastHeight = 0; }
            lastHeight = juce::jmax (lastHeight, c.height);
            col += c.span;
        }
        heights.push_back (lastHeight);
        rows = (int) heights.size();
        int total = 0;
        for (auto h : heights) total += h;

        const int available = getHeight() - pad * 2;
        // Top row level with the picture's top, bottom row level with its bottom. A single row sits in the middle.
        const int gap = rows > 1 ? (available - total) / (rows - 1) : 0;

        int y = rows > 1 ? pad : pad + (available - total) / 2, row = 0;
        col = 0;
        for (auto& c : cells)
        {
            if (col + c.span > 2)
            {
                y += heights[(size_t) row++] + gap;
                col = 0;
            }
            const int w = c.span == 2 ? width : cell;
            const int rowHeight2 = heights[(size_t) row];
            c.component->setBounds (x + col * (cell + gridGap), c.bottomAligned ? y + rowHeight2 - c.height : y, w, c.height);
            col += c.span;
        }
    }

    struct Cell { std::unique_ptr<juce::Component> component; int span, height; bool bottomAligned; };

    const int module;
    PluginProcessor& processor;
    const juce::Colour accent;
    ParamToggle onSwitch;
    std::vector<Cell> cells;
};

// ------------------------------------------------------------------------------------------------------- advanced page

struct TuneFxEditor::AdvancedPage final : public juce::Component
{
    explicit AdvancedPage (PluginProcessor& p)
        : processor (p),
          inputType (p.apvts, "inputType", "Input Type", "The range of the voice. A lower range hears lower notes but adds more delay.", tone::magenta),
          midiTarget (p.apvts, "midiTarget", "MIDI Target", "Held MIDI notes set the pitch. As Played uses the exact note, Any Octave uses the nearest octave of it.", tone::magenta),
          reference (p.apvts, "reference", "Reference A4", "Where A sits. 440 Hz unless the song was tuned differently.", Kind::main, tone::magenta),
          tracking (p.apvts, "tracking", "Tracking", "Which sounds count as pitched. Low is choosy, high tunes rough sound too.", Kind::main, tone::magenta),
          keys (p)
    {
        setOpaque (true);
        setBufferedToImage (true);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &inputType, &midiTarget, &reference, &tracking, &keys })
            addAndMakeVisible (c);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::black);
        g.setColour (tone::raised);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

        auto column = juce::Rectangle<float> ((float) pad, (float) pad, (float) titleWidth, (float) (getHeight() - pad * 2));
        g.setColour (tone::white);
        g.setFont (font (Weight::medium, 30.0f));
        g.drawText ("Advanced", column.removeFromTop (38.0f), juce::Justification::centredLeft, false);
        column.removeFromTop (6.0f);
        juce::AttributedString about;
        about.setText ("How it listens to your voice, what MIDI does, and your own scale.");
        about.setFont (font (Weight::regular, 13.5f));
        about.setColour (tone::muted);
        about.setLineSpacing (3.0f);
        about.draw (g, column);

        const int keysX = pad + titleWidth + 16;
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText ("Your own scale", keysX, pad, graphWidth, 18, juce::Justification::centredLeft, false);

        // What the keys do, and when they count.
        const bool ownScale = processor.apvts.getRawParameterValue ("scale")->load (std::memory_order_relaxed) >= (float) scaleCustom - 0.5f;
        g.setColour (ownScale ? tone::muted : tone::faint);
        g.drawText (ownScale ? "Click a key: Tune, Remove or Bypass." : "Only used with Scale on Custom.", keysX, panelHeight - pad - 18, graphWidth, 18, juce::Justification::centredLeft, false);
    }

    // Laid out for the open panel's full height, from the top: while it glides open or shut it is only as tall as the part showing, so it
    // never reaches over the footer below the panel (1.2.5).
    void resized() override
    {
        const int keysX = pad + titleWidth + 16;
        keys.setBounds (keysX, pad + 24, graphWidth, 196);

        const int x = keysX + graphWidth + gridGap, width = getWidth() - pad - x, cell = (width - gridGap) / 2;
        const int inner = panelHeight - pad * 2, gap = (inner - 56 - 56 - 64) / 2;
        inputType.setBounds (x, pad, width, 56);
        midiTarget.setBounds (x, pad + 56 + gap, width, 56);
        reference.setBounds (x, panelHeight - pad - 64, cell, 64);
        tracking.setBounds (x + cell + gridGap, panelHeight - pad - 64, cell, 64);
    }

    PluginProcessor& processor;
    ParamSegments inputType, midiTarget;
    ParamSlider reference, tracking;
    CustomKeys keys;
};

// ------------------------------------------------------------------------------------------------------------- panel

struct TuneFxEditor::Panel final : public juce::Component
{
    Panel (TuneFxEditor& e, PluginProcessor& p)
        : owner (e), processor (p),
          reading (std::make_unique<Reading> (p)),
          advancedButton (std::make_unique<AdvancedButton>()),
          presets (p),
          key (p.apvts, "key", {}, "The key your song is in."),
          scale (p.apvts, "scale", {}, "Which notes the voice is pulled to. Chromatic means all of them."),
          followKey (p.apvts, "followKey", "Follow Key", "Take the key from the Key plug-in instead of these menus.", tone::white),
          bypass (p.apvts, "bypass", "Bypass", "Turns everything off, with the same timing.", tone::white),
          retune (p.apvts, "retune", "Retune Speed", "How fast the voice gets pulled to the note. Instant is the hard tuned effect.", Kind::lead, tone::magenta),
          humanize (p.apvts, "humanize", "Humanize", "Lets held notes keep their natural movement, while the start of each note stays quick.", Kind::main, tone::magenta),
          outputGain (p.apvts, "outputGain", "Output Gain", "How loud it all comes out.", Kind::inlineBar, tone::white),
          row (p),
          graph (p),
          advancedPage (p)
    {
        setInterceptsMouseClicks (false, true);
        row.setBufferedToImage (true);

        for (juce::Component* c : std::initializer_list<juce::Component*> { reading.get(), &reading->getBar(), advancedButton.get(), &presets, &key, &scale, &followKey, &bypass, &retune, &humanize, &outputGain, &status, &latency, &hintLine, &row })
            addAndMakeVisible (c);

        for (int i = 0; i < numModules; ++i)
        {
            pages.push_back (std::make_unique<ModulePage> (p, i));
            addChildComponent (*pages.back());
        }
        addAndMakeVisible (graph);
        addChildComponent (advancedPage);

        advancedButton->onClick = [this] { owner.setAdvancedOpen (! owner.isAdvancedOpen(), true); };
        row.onSelect = [this] (int m) { owner.selectModule (m); };
        row.onHint = [this] (const juce::String& text) { owner.showHint (text); };
        row.onSwitched = [this] { repaint (panelArea()); };

        const auto quiet = [] (juce::Label& l, juce::Justification j)
        {
            l.setFont (font (Weight::regular, 13.0f));
            l.setColour (juce::Label::textColourId, tone::muted);
            l.setInterceptsMouseClicks (false, false);
            l.setJustificationType (j);
            l.setBorderSize ({});
        };
        quiet (status, juce::Justification::centredRight);
        quiet (latency, juce::Justification::centredRight);
        quiet (hintLine, juce::Justification::centredLeft);
        status.setComponentID ("status");
        latency.setComponentID ("latency");
        hintLine.setComponentID ("hintLine");
        hintLine.setText (defaultHint, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (tone::white);
        g.setFont (font (Weight::medium, 24.0f));
        g.drawText (owner.headerTitle(), juce::Rectangle<int> (margin, 0, 110, headerHeight), juce::Justification::centredLeft, false);

        // The panel that belongs to the selected block: its colour's tint, and a solid edge that runs on from the block when it is on.
        if (owner.selected >= 0 && owner.selected < numModules && g.clipRegionIntersects (panelArea()))
        {
            const auto& m = modules()[(size_t) owner.selected];
            const bool on = row.isOn (owner.selected);
            auto area = panelArea().toFloat();
            const float dim = processor.apvts.getRawParameterValue ("bypass")->load (std::memory_order_relaxed) >= 0.5f ? 0.4f : 1.0f;
            const auto tint = panelTint (m.colour, on);
            const bool atLeft = owner.selected == 0, atRight = owner.selected == numModules - 1;
            juce::Path shape;
            shape.addRoundedRectangle (area.getX(), area.getY(), area.getWidth(), area.getHeight(), 8.0f, 8.0f, ! atLeft, ! atRight, true, true);
            g.setColour (tint);
            g.fillPath (shape);

            if (on)
            {
                juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (shape);
                g.setColour (m.colour.withMultipliedAlpha (dim));
                g.fillRect (area.withHeight (3.0f));
            }
        }
    }

    juce::Rectangle<int> panelArea() const { return { margin, panelTop, designWidth - 2 * margin, panelHeight }; }

    void layout (float amount)
    {
        // Header
        presets.setBounds (136, 16, 196, 32);
        key.setBounds (348, 16, 64, 32);
        scale.setBounds (418, 16, 158, 32);
        followKey.setBounds (582, 16, 92, 32);
        bypass.setBounds (designWidth - margin - 76, 16, 76, 32);
        status.setBounds (designWidth - margin - 76 - 300 - 14, 16, 300, 32);

        // Tuning, and the reading on the other side of the strip
        retune.setBounds (margin, heroTop + 2, stripLeft - margin - 24, 92);
        humanize.setBounds (margin, heroTop + heroHeight - 64 - 2, stripLeft - margin - 24, 64);
        reading->setBounds (stripRight + 20, heroTop, designWidth - margin - stripRight - 20, heroHeight);
        reading->getBar().setBounds (reading->barBounds());

        row.setBounds (margin, rowTop, designWidth - 2 * margin, rowHeight);

        // One panel. Advanced rises over it.
        const auto area = panelArea();
        for (auto& page : pages)
            page->setBounds (area);
        graph.setBounds (area.getX() + pad + titleWidth + 16, area.getY() + pad, graphWidth, area.getHeight() - pad * 2);

        const int openHeight = juce::roundToInt ((float) area.getHeight() * amount);
        advancedPage.setBounds (area.getX(), area.getBottom() - openHeight, area.getWidth(), openHeight);
        advancedPage.setVisible (openHeight > 0);
        graph.setCovered (openHeight >= area.getHeight());

        // Footer
        const int footerY = designHeight - footerBottom - footerHeight;
        outputGain.setBounds (margin, footerY, 300, footerHeight);
        latency.setBounds (designWidth - margin - 164 - 96, footerY, 92, footerHeight);
        hintLine.setBounds (margin + 324, footerY, latency.getX() - 12 - (margin + 324), footerHeight);   // ends short of the latency (1.2.5; it ran 32 px into it)
        advancedButton->setBounds (designWidth - margin - 150, footerY, 150, footerHeight);
    }

    int countChanges() const
    {
        int changes = 0;
        for (auto* s : { &advancedPage.reference, &advancedPage.tracking })
            changes += s->isChanged() ? 1 : 0;
        for (auto* c : { &advancedPage.inputType, &advancedPage.midiTarget })
            changes += c->isChanged() ? 1 : 0;
        for (int i = 0; i < 12; ++i)
            changes += processor.apvts.getRawParameterValue ("note" + juce::String (i))->load (std::memory_order_relaxed) > 0.5f ? 1 : 0;
        return changes;
    }

    TuneFxEditor& owner;
    PluginProcessor& processor;
    std::unique_ptr<Reading> reading;
    std::unique_ptr<AdvancedButton> advancedButton;
    PresetBox presets;
    ParamMenu key, scale;
    ParamToggle followKey, bypass;
    ParamSlider retune, humanize, outputGain;
    ModuleRow row;
    std::vector<std::unique_ptr<ModulePage>> pages;
    ModuleGraph graph;
    AdvancedPage advancedPage;
    juce::Label status, latency, hintLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
};

// ------------------------------------------------------------------------------------------------------------- editor

TuneFxEditor::TuneFxEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), strip (p)
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    panel = std::make_unique<Panel> (*this, p);
    addAndMakeVisible (*panel);
    addAndMakeVisible (strip);
    addMouseListener (this, true);

    const double ratio = (double) designWidth / (double) designHeight;
    setResizable (true, true);
    setResizeLimits (PluginProcessor::minWindowWidth, juce::roundToInt (PluginProcessor::minWindowWidth / ratio),
                     PluginProcessor::maxWindowWidth, juce::roundToInt (PluginProcessor::maxWindowWidth / ratio));
    getConstrainer()->setFixedAspectRatio (ratio);

    advancedAmount = isAdvancedOpen() ? 1.0f : 0.0f;
    advancedShown = isAdvancedOpen();
    showModule (juce::jlimit (0, numModules - 1, p.selectedModule.load (std::memory_order_relaxed)));
    panel->row.setSelected (isAdvancedOpen() ? -1 : selected);

    const int savedWidth = juce::jlimit (PluginProcessor::minWindowWidth, PluginProcessor::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed));
    constructed = true;
    setSize (savedWidth, juce::roundToInt (savedWidth / ratio));
    resized();     // the resize limits above may already have set exactly this size, and then setSize alone would not lay anything out

    tick (1.0 / maxFramesPerSecond);
    startTimerHz (5);
}

TuneFxEditor::~TuneFxEditor()
{
    stopTimer();
    vblank.reset();
    removeMouseListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

ModuleRow& TuneFxEditor::getRow() noexcept     { return panel->row; }
ModuleGraph& TuneFxEditor::getGraph() noexcept { return panel->graph; }

void TuneFxEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::black);
}

void TuneFxEditor::placeStrip()
{
    const float s = (float) getWidth() / (float) designWidth;
    const int left = juce::roundToInt ((float) stripLeft * s), right = juce::roundToInt ((float) stripRight * s);
    const int top = juce::roundToInt ((float) heroTop * s), height = juce::roundToInt ((float) heroHeight * s);
    strip.setTypeScale (s);
    strip.setBounds (left, top, right - left, height);
}

void TuneFxEditor::resized()
{
    if (! constructed)
        return;

    panel->setBounds (0, 0, designWidth, designHeight);
    panel->setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) designWidth));
    panel->layout (advancedAmount);
    placeStrip();
    processor.windowWidth.store (getWidth(), std::memory_order_relaxed);
}

void TuneFxEditor::showModule (int module)
{
    selected = module;
    processor.selectedModule.store (module, std::memory_order_relaxed);

    for (int i = 0; i < numModules; ++i)
        panel->pages[(size_t) i]->setVisible (i == module);

    panel->graph.setModule (module);
    panel->repaint();
}

void TuneFxEditor::selectModule (int module)
{
    showModule (module);

    // Picking a module means you want its panel, so Advanced steps aside.
    if (isAdvancedOpen())
        setAdvancedOpen (false, true);

    panel->row.setSelected (module);
}

void TuneFxEditor::setAdvancedOpen (bool shouldBeOpen, bool animate)
{
    processor.advancedOpen.store (shouldBeOpen, std::memory_order_relaxed);
    advancedShown = shouldBeOpen;
    panel->row.setSelected (shouldBeOpen ? -1 : selected);

    if (! animate)
    {
        advancedAmount = shouldBeOpen ? 1.0f : 0.0f;
        panel->layout (advancedAmount);
        panel->advancedButton->setState (advancedAmount, panel->countChanges());
        panel->repaint();
    }
    else
    {
        // Frames from the screen do the glide, from the first one. Up to 1.2.4 a 0.2 s step was taken here, which moved it 98% at the click.
        stillFor = 0.0;
        startFrames();
    }
}

void TuneFxEditor::showHint (const juce::String& text)
{
    panel->hintLine.setText (text.isNotEmpty() ? text : juce::String (defaultHint), juce::dontSendNotification);
}

void TuneFxEditor::hintFromComponent (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            showHint (hinted->hint);
            return;
        }
    }
    showHint ({});
}

void TuneFxEditor::mouseEnter (const juce::MouseEvent& e)
{
    hintFromComponent (e.eventComponent);
}

void TuneFxEditor::mouseExit (const juce::MouseEvent&)
{
    showHint ({});
}

juce::Component* TuneFxEditor::findControl (const juce::String& paramId)
{
    std::function<juce::Component* (juce::Component&)> search = [&] (juce::Component& holder) -> juce::Component*
    {
        for (auto* c : holder.getChildren())
        {
            if (c->getComponentID() == paramId)
                return c;
            if (auto* found = search (*c))
                return found;
        }
        return nullptr;
    };
    return search (*panel);
}

void TuneFxEditor::clickCustomKey (int key)
{
    panel->advancedPage.keys.cycle (key);
}

juce::String TuneFxEditor::headerTitle() const  { return processor.getName(); }   // Glitter (1.2.5; it still said Tune FX after the rename)
juce::String TuneFxEditor::advancedLabel() const { return panel->advancedButton->label(); }
int TuneFxEditor::advancedLabelRoom() const      { return panel->advancedButton->labelRoom(); }
int TuneFxEditor::correctionBarPixels() const    { return panel->reading->barPixels(); }

void TuneFxEditor::previewHover (const juce::String& paramId)
{
    if (auto* c = findControl (paramId))
    {
        if (auto* slider = dynamic_cast<ParamSlider*> (c))
            slider->setForcedHover (true);
        if (auto* toggle = dynamic_cast<ParamToggle*> (c))
            toggle->setForcedHover (true);
        if (auto* hinted = dynamic_cast<Hinted*> (c))
            showHint (hinted->hint);
    }
}

void TuneFxEditor::tick (double dtSeconds)
{
    bool moved = strip.advance (dtSeconds);
    const int note = strip.getOutputNote();
    moved = panel->reading->advance (dtSeconds, note) || moved;
    // A state loaded while the window is open (a Logic preset, say) can change the module and Advanced underneath it: follow it (1.2.5).
    const int wantedModule = juce::jlimit (0, numModules - 1, processor.selectedModule.load (std::memory_order_relaxed));
    if (wantedModule != selected)
        showModule (wantedModule);
    if (isAdvancedOpen() != advancedShown)
    {
        advancedShown = isAdvancedOpen();
        panel->row.setSelected (advancedShown ? -1 : selected);
    }

    moved = panel->row.refresh() || moved;
    moved = panel->graph.refresh() || moved;
    if (panel->advancedPage.keys.advance (note))
        panel->advancedPage.repaint();
    panel->presets.refresh();

    // Advanced glides open and shut, in about a fifth of a second.
    const float target = isAdvancedOpen() ? 1.0f : 0.0f;
    if (advancedAmount != target)
    {
        const float next = advancedAmount + (target - advancedAmount) * (1.0f - std::exp (-(float) dtSeconds / 0.05f));
        advancedAmount = std::abs (next - target) < 0.01f ? target : next;
        panel->layout (advancedAmount);
        panel->advancedButton->setState (advancedAmount, panel->countChanges());
        panel->repaint (panel->panelArea());
        moved = true;
    }

    // Things that change rarely.
    if (--slowCountdown <= 0)
    {
        slowCountdown = dtSeconds > 0.1 ? 2 : 15;   // about twice a second, at either frame rate

        // Voice 2 only sounds with two voices. It stays usable, just quieter to look at.
        if (auto* voice2 = panel->pages[3]->findChildWithID ("harmonyInterval2"))
            voice2->setAlpha (processor.apvts.getRawParameterValue ("harmonyVoices")->load (std::memory_order_relaxed) >= 0.5f ? 1.0f : 0.45f);
        panel->advancedButton->setState (advancedAmount, panel->countChanges());

        const double rate = processor.getSampleRate();
        const auto text = rate > 0.0 ? "Adds " + juce::String (processor.getLatencySamples() * 1000.0 / rate, 1) + " ms" : juce::String();
        panel->latency.setText (text, juce::dontSendNotification);

        const auto r = processor.getReadings();
        juce::StringArray bits;
        // Only when a key from the file is in use (1.2.5): with none yet, or a Custom scale, the menus' key is in use, and it said Following.
        if (r.following)
            bits.add (juce::String ("Following ") + noteNames[juce::jlimit (0, 11, r.activeKey)] + " " + scaleNames[juce::jlimit (0, numScales - 1, r.activeScale)]);
        if (r.heldNotes > 0)
            bits.add ("MIDI notes held: " + juce::String (r.heldNotes));
        panel->status.setText (bits.joinIntoString (", "), juce::dontSendNotification);
    }

    stillFor = moved ? 0.0 : stillFor + dtSeconds;
}

void TuneFxEditor::timerCallback()
{
    // Nothing has been moving, so look only a few times a second. Move again as soon as there is something to show.
    tick (0.2);
    startFrames();
}

void TuneFxEditor::startFrames()
{
    if (stillFor < 1.0 && vblank == nullptr)
    {
        stopTimer();
        lastFrameTime = 0.0;
        frameBudget = 0.0;
        sinceDrawn = 0.0;

        // At most one frame per cap on any screen rate, spending whole refreshes against a running budget.
        vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestampSeconds)
        {
            const double elapsed = lastFrameTime > 0.0 ? timestampSeconds - lastFrameTime : 1.0 / maxFramesPerSecond;
            lastFrameTime = timestampSeconds;
            sinceDrawn += elapsed;
            frameBudget = juce::jmin (frameBudget + elapsed, 0.05);

            if (frameBudget + 0.001 < 1.0 / maxFramesPerSecond)
                return;

            frameBudget -= 1.0 / maxFramesPerSecond;
            const double step = sinceDrawn;
            sinceDrawn = 0.0;
            tick (step);

            if (stillFor >= 1.0)
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TuneFxEditor> (this)]
                {
                    if (safe != nullptr && safe->stillFor >= 1.0)
                    {
                        safe->vblank.reset();
                        safe->startTimerHz (5);
                    }
                });
            }
        });
    }
}

juce::String TuneFxEditor::readingText() const
{
    const auto r = processor.getReadings();
    auto text = juce::String ("Active Key ") + noteNames[juce::jlimit (0, 11, r.activeKey)] + " " + scaleNames[juce::jlimit (0, numScales - 1, r.activeScale)];
    if (! r.voiced)
        return text + "  |  no pitch";
    return text + "  |  " + juce::String (r.detectedHz, 1) + " Hz  |  Target " + noteName (r.targetNote)
           + "  |  Correction " + juce::String (juce::roundToInt (r.correctionCents)) + " cents"
           + (r.heldNotes > 0 ? "  |  MIDI notes held " + juce::String (r.heldNotes) : juce::String());
}
