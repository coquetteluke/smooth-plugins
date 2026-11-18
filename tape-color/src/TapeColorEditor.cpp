#include "TapeColorEditor.h"

using namespace ui;

namespace
{
    constexpr int margin = 20;
    constexpr int headerHeight = 58;
    constexpr int ageHeight = 112;
    constexpr int rowLabel = 22;
    constexpr int tileHeight = 62;
    constexpr int footerHeight = 40;
    constexpr int sectionGap = 14;
    constexpr double fastRate = 30.0, slowRate = 8.0;

    const juce::String restingHint = "Double-click anything to reset it. Click a value to type one.";

    juce::String minusSign() { return juce::String::fromUTF8 ("\xe2\x88\x92"); }

    juce::String percent (float v) { return juce::String (juce::roundToInt (v)) + "%"; }

    juce::String decibels (float v)
    {
        const float rounded = std::round (v * 10.0f) * 0.1f;
        return (rounded > 0.0f ? juce::String ("+") : (rounded < 0.0f ? minusSign() : juce::String())) + juce::String (std::abs (rounded), 1) + " dB";
    }

    juce::String signedPercent (float v)
    {
        const int n = juce::roundToInt (v);
        return (n > 0 ? juce::String ("+") : (n < 0 ? minusSign() : juce::String())) + juce::String (std::abs (n)) + "%";
    }

    juce::String hertz (float v)
    {
        return v >= 1000.0f ? juce::String (v * 0.001f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
    }

    juce::String ageWord (float percentValue)
    {
        if (percentValue < 8.0f)  return "Fresh reel";
        if (percentValue < 30.0f) return "Used reel";
        if (percentValue < 55.0f) return "Old reel";
        if (percentValue < 80.0f) return "Cassette";
        return "Worn cassette";
    }

    constexpr const char* watchedIds[] = { "drive", "bias", "speed", "age", "hardness", "spacing", "wow", "flutter", "noise", "lowcut", "highcut", "mix",
                                           "compander", "encode", "decode", "device", "devicemix", "bypass", "output", "autolevel", "azimuth", "random",
                                           "stereodrift", "dropdepth", "droprate", "texture" };
}

// ---- Age, in words ------------------------------------------------------------------------------------------------

class TapeColorEditor::AgeValue final : public juce::Component,
                                        public Hinted,
                                        private juce::Timer
{
public:
    AgeValue (juce::RangedAudioParameter& p, AgeRibbon& r) : param (p), ribbon (r)
    {
        hint = "One sweep from a fresh reel to a worn cassette: less top end, less headroom, more hiss, more wobble. Click the number to type one.";
        setOpaque (true);
        box.setFont (font (Weight::medium, 34.0f));
        box.setJustificationType (juce::Justification::centredLeft);
        box.setEditable (false, false, true);
        box.onTextChange = [this] { applyTyped(); };
        box.onEditorHide = [this] { box.setVisible (false); repaint(); };
        addChildComponent (box);
    }

    ~AgeValue() override  { stopTimer(); }

    void setValue (float newValue)
    {
        // Every move is kept. It is drawn again when the number or the word it shows changes. Up to 0.4.3 a move under 0.5 was dropped,
        // so 29.7 then 30 kept saying Used reel.
        const bool looksDifferent = percent (newValue) != percent (shown) || ageWord (newValue) != ageWord (shown);
        shown = newValue;
        if (looksDifferent)
            repaint();
    }

    juce::String shownText() const  { return percent (shown) + " " + ageWord (shown); }
    void typeText (const juce::String& text)  { box.setText (text, juce::sendNotificationSync); }

    void startTyping()
    {
        box.setBounds (numberArea());
        box.setText (juce::String (juce::roundToInt (shown)), juce::dontSendNotification);
        box.setVisible (true);
        box.showEditor();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const int w = juce::roundToInt ((float) getWidth() * scale), h = juce::roundToInt ((float) getHeight() * scale);

        // Drawn once for each value and kept. The reel's repaints can stretch the area being drawn over this, and text is what costs.
        if (! picture.isValid() || picture.getWidth() != w || picture.getHeight() != h || pictureText != shownText() || pictureTyping != box.isVisible())
        {
            pictureText = shownText();
            pictureTyping = box.isVisible();
            picture = juce::Image (juce::Image::RGB, juce::jmax (1, w), juce::jmax (1, h), true);
            juce::Graphics p (picture);
            p.addTransform (juce::AffineTransform::scale (scale));
            draw (p);
        }

        g.setColour (juce::Colours::black);
        g.drawImageTransformed (picture, juce::AffineTransform::scale (1.0f / scale));
    }

    void draw (juce::Graphics& g) const
    {
        g.fillAll (tone::base);
        const auto r = getLocalBounds().toFloat();
        const auto big = font (Weight::regular, 36.0f);

        g.setFont (big);
        g.setColour (tone::text);
        g.drawText ("Age", r.withWidth (wordWidth()), juce::Justification::centredLeft, false);

        const auto number = font (Weight::medium, 36.0f, true);
        if (! box.isVisible())
        {
            g.setFont (number);
            g.setColour (tone::amber);
            g.drawText (percent (shown), numberArea().toFloat(), juce::Justification::centredLeft, false);
        }

        g.setFont (font (Weight::regular, 20.0f));
        g.setColour (tone::muted);
        const float at = (float) numberArea().getX() + juce::GlyphArrangement::getStringWidth (number, percent (shown)) + 14.0f;
        g.drawText (ageWord (shown), r.withLeft (at).withTrimmedTop (5.0f), juce::Justification::centredLeft, false);
    }

    void mouseDown (const juce::MouseEvent&) override { stopTimer(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.getNumberOfClicks() == 1 && numberArea().contains (e.getPosition()))
            startTimer (juce::MouseEvent::getDoubleClickTimeout() + 30);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        stopTimer();
        param.beginChangeGesture();
        param.setValueNotifyingHost (param.getDefaultValue());
        param.endChangeGesture();
    }

private:
    float wordWidth() const { return juce::GlyphArrangement::getStringWidth (font (Weight::regular, 36.0f), "Age") + 12.0f; }
    juce::Rectangle<int> numberArea() const { return getLocalBounds().withTrimmedLeft ((int) wordWidth()).withWidth (98); }

    void timerCallback() override
    {
        stopTimer();
        startTyping();
    }

    void applyTyped()
    {
        const auto text = box.getText().trim();
        if (! text.containsAnyOf ("0123456789"))
            return;
        // A minus sign is kept, as the other blocks keep it, so "-20" is under the bottom and lands on 0 instead of turning into 20.
        const double value = juce::jlimit (0.0, 100.0, text.replace (minusSign(), "-").retainCharacters ("0123456789.-").getDoubleValue());
        param.beginChangeGesture();
        ribbon.setValue (value, juce::sendNotificationSync);
        param.endChangeGesture();
    }

    juce::RangedAudioParameter& param;
    AgeRibbon& ribbon;
    juce::Label box;
    juce::Image picture;
    float shown = -1.0f;
    juce::String pictureText;
    bool pictureTyping = false;
};

// ---- Advanced -----------------------------------------------------------------------------------------------------

class TapeColorEditor::AdvancedHeader final : public juce::Component,
                                              public Hinted
{
public:
    explicit AdvancedHeader (TapeColorEditor& o) : owner (o)
    {
        hint = "The tape's surface, the compander, the playback head, the device and the drift.";
        setComponentID ("advanced");
        setTitle ("Advanced");
    }

    juce::String getSummary() const  { return summary; }

    void setSummary (const juce::String& text)
    {
        if (text != summary)
        {
            summary = text;
            repaint();
        }
    }

    /** The hint line borrows the room while it has something long to say. */
    void setSummaryShown (bool shouldShow)
    {
        if (shouldShow != summaryShown)
        {
            summaryShown = shouldShow;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const bool lit = isMouseOver (true), open = owner.isAdvancedOpen();
        auto arrow = b.removeFromLeft (16.0f);
        const float cx = arrow.getCentreX() - 3.0f, cy = arrow.getCentreY();
        juce::Path chevron;
        if (open)
        {
            chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
            chevron.lineTo (cx, cy + 2.0f);
            chevron.lineTo (cx + 4.0f, cy - 2.0f);
        }
        else
        {
            chevron.startNewSubPath (cx - 2.0f, cy - 4.0f);
            chevron.lineTo (cx + 2.0f, cy);
            chevron.lineTo (cx - 2.0f, cy + 4.0f);
        }
        g.setColour (lit ? tone::text : tone::muted);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setFont (font (Weight::medium, 14.0f));
        g.drawText ("Advanced", b.removeFromLeft (68.0f), juce::Justification::centredLeft, false);

        if (summary.isNotEmpty() && ! open && summaryShown)
        {
            g.setColour (tone::text);
            g.setFont (font (Weight::regular, 13.0f));
            g.drawText (summary, b.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }
    void mouseDown (const juce::MouseEvent&) override   { owner.setAdvancedOpen (! owner.isAdvancedOpen()); }

private:
    TapeColorEditor& owner;
    juce::String summary;
    bool summaryShown = true;
};

// ---- the editor ---------------------------------------------------------------------------------------------------

TapeColorEditor::TapeColorEditor (PluginProcessor& p)
    : juce::AudioProcessorEditor (p),
      processorRef (p),
      ribbon (p.apvts, "age", "One sweep from a fresh reel to a worn cassette: less top end, less headroom, more hiss, more wobble."),
      presets (p),
      autoLevel (p.apvts, "autolevel", "Auto level", "Holds the level steady as you turn up Drive, so you hear the tone and not just louder."),
      bypass (p.apvts, "bypass", "Bypass", "Turns the tape off. The delay stays the same, so nothing shifts in time."),
      drive (p.apvts, "drive", "Drive", "How hard the sound hits the tape. More is warmer and thicker.", [] (float v) { return juce::String (v, 1) + " dB"; }),
      bias (p.apvts, "bias", "Bias", "0 is set up right. Up adds warmth and takes some top end. Down starves the tape, so quiet parts break up.", signedPercent),
      noise (p.apvts, "noise", "Noise", "Tape hiss. Age adds its own on top of this.", percent),
      wow (p.apvts, "wow", "Wow", "A slow swing in pitch, about once a second, like a warped tape.", percent),
      flutter (p.apvts, "flutter", "Flutter", "A fast shake in pitch, about 9 times a second, like a worn motor.", percent),
      mix (p.apvts, "mix", "Mix", "How much tape against the clean sound.", percent),
      output (p.apvts, "output", "Output", "The level leaving the plug-in.", decibels),
      speed (p.apvts, "speed", "Tape speed, ips", "Inches a second. Slower tape has a lower, bigger low end bump and loses top end sooner.", { "3.75", "7.5", "15", "30" }),
      hardness (p.apvts, "hardness", "Hardness", "How soon the tape flattens the sound. Soft takes 6 dB more level, hard 6 dB less.", percent),
      texture (p.apvts, "texture", "Texture", "Grain on the tape's surface that follows the sound. Silence stays silent.", percent),
      dropDepth (p.apvts, "dropdepth", "Dropouts", "Short dips where the coating has worn thin. This sets how deep.", percent),
      dropRate (p.apvts, "droprate", "Dropout rate", "How many dips a second, on average. Only does anything with Dropouts up.", [] (float v) { return juce::String (v, v < 10.0f ? 2 : 1) + "/s"; }),
      compander (p.apvts, "compander", "Type", "Noise reduction: boosts the top end on the way on to the tape and takes it back off afterwards, with the hiss.", { "Off", "Fixed", "Soft", "Strong" }),
      encode (p.apvts, "encode", "Encode", "The boost going on. Encode without Decode adds air and bite.", percent),
      decode (p.apvts, "decode", "Decode", "The cut coming off. Decode without Encode goes dark and pumps.", percent),
      lowCut (p.apvts, "lowcut", "Low cut", "Takes out the lows below this. All the way down is off.", [] (float v) { return v <= 20.01f ? juce::String ("Off") : hertz (v); }),
      highCut (p.apvts, "highcut", "High cut", "Takes out the highs above this. All the way up is off.", [] (float v) { return v >= 19999.5f ? juce::String ("Off") : hertz (v); }),
      spacing (p.apvts, "spacing", "Head gap", "A gap between head and tape. Loses more the higher the sound and the slower the tape.", percent),
      azimuth (p.apvts, "azimuth", "Head tilt", "A tilted head reads one side early. Adds width, loses top end when summed to mono.", signedPercent),
      device (p.apvts, "device", "Type", "The shape of small old equipment, after the tape. My own shapes, not captures of real hardware."),
      deviceMix (p.apvts, "devicemix", "Device mix", "How much of the device against the tape without it.", percent),
      randomDrift (p.apvts, "random", "Random", "Irregular slow movement in pitch.", percent),
      stereoDrift (p.apvts, "stereodrift", "Stereo", "Lets the right side drift on its own. 0 is safe in mono.", percent)
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (false);
    setOpaque (true);
    drawerShown = p.advancedOpen.load (std::memory_order_relaxed);

    lowCut.offWord = highCut.offWord = "off";
    lowCut.acceptsKilo = highCut.acceptsKilo = true;

    for (auto* t : { &hardness, &texture, &dropDepth, &dropRate, &encode, &decode, &lowCut, &highCut, &spacing, &azimuth, &deviceMix, &randomDrift, &stereoDrift })
        t->setCompact (true);

    presets.nameNow = [this] { return presetNameNow(); };
    presets.indexNow = [this]
    {
        const int matching = processorRef.programMatchingSettings();
        return matching >= 0 ? matching : processorRef.getCurrentProgram();
    };

    ageValue = std::make_unique<AgeValue> (*p.apvts.getParameter ("age"), ribbon);
    advancedHeader = std::make_unique<AdvancedHeader> (*this);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &response, &curve, &reel, &ribbon, ageValue.get(), &presets, &autoLevel, &bypass,
                                                                        &drive, &bias, &speed, &noise, &wow, &flutter, &mix, &output, advancedHeader.get() })
        addAndMakeVisible (c);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &hardness, &texture, &dropDepth, &dropRate, &compander, &encode, &decode, &lowCut, &highCut,
                                                                        &spacing, &azimuth, &device, &deviceMix, &randomDrift, &stereoDrift })
        addChildComponent (c);

    for (const char* id : watchedIds)
        values.push_back (p.apvts.getRawParameterValue (id));
    lastValues.assign (values.size(), std::numeric_limits<float>::quiet_NaN());

    fftIn.resize ((size_t) fftSize);
    fftOut.resize ((size_t) fftSize);
    hann.resize ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        hann[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
    inDb.fill (-200.0f);
    outDb.fill (-200.0f);

    hintText = restingHint;
    addMouseListener (this, true);

    setResizable (true, true);
    const bool open = isAdvancedOpen();
    using P = PluginProcessor;
    const int extra = open ? drawerHeight : 0;
    setResizeLimits (P::minWindowWidth, P::minWindowHeight + extra, P::maxWindowWidth, P::maxWindowHeight);
    setSize (juce::jlimit (P::minWindowWidth, P::maxWindowWidth, p.windowWidth.load (std::memory_order_relaxed)),
             juce::jlimit (P::minWindowHeight + extra, P::maxWindowHeight, p.windowHeight.load (std::memory_order_relaxed)));
    constructed = true;
    resized();

    // Nothing from the last time a window was open: the peak the audio left after that window's last frame, and the old sound in the tap.
    p.tapPeak.store (0.0f, std::memory_order_relaxed);
    lastWritten = openedAt = p.tapWritten.load (std::memory_order_acquire);
    p.tapWanted.store (true, std::memory_order_relaxed);

    lastTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    tick (1.0 / fastRate);
    startTimerHz ((int) slowRate);
}

TapeColorEditor::~TapeColorEditor()
{
    stopTimer();
    processorRef.tapWanted.store (false, std::memory_order_relaxed);
    removeMouseListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    setLookAndFeel (nullptr);
}

bool TapeColorEditor::isAdvancedOpen() const noexcept
{
    return drawerShown;
}

void TapeColorEditor::setAdvancedOpen (bool shouldOpen)
{
    processorRef.advancedOpen.store (shouldOpen, std::memory_order_relaxed);
    if (shouldOpen != drawerShown)
        showDrawer (shouldOpen);
}

void TapeColorEditor::showDrawer (bool open)
{
    drawerShown = open;

    using P = PluginProcessor;
    const int extra = open ? drawerHeight : 0;
    int wanted = 0;
    if (open)
    {
        wanted = juce::jlimit (P::minWindowHeight + extra, P::maxWindowHeight, getHeight() + drawerHeight);

        // On a small screen the picture gives up room rather than the window running off the bottom.
        if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
            wanted = juce::jlimit (P::minWindowHeight + extra, juce::jmax (P::minWindowHeight + extra, display->userArea.getHeight() - 60), wanted);
        drawerAdded = wanted - getHeight();
    }
    else
    {
        // Shutting it takes off what opening it added, so open then shut is the height it was. Up to 0.4.3 it took off the whole drawer
        // even when the top of the window or the screen had cut the opening short.
        wanted = juce::jlimit (P::minWindowHeight, P::maxWindowHeight, getHeight() - drawerAdded);
        drawerAdded = drawerHeight;
    }

    setResizeLimits (P::minWindowWidth, P::minWindowHeight + extra, P::maxWindowWidth, P::maxWindowHeight);
    setSize (getWidth(), wanted);
    resized();
    repaint();
    advancedHeader->repaint();
    updateSummary();
}

// ---- reading the settings -----------------------------------------------------------------------------------------

juce::String TapeColorEditor::presetNameNow() const
{
    const int matching = processorRef.programMatchingSettings();
    if (matching >= 0)
        return processorRef.getProgramName (matching);
    const int current = processorRef.getCurrentProgram();
    return current >= 0 ? processorRef.getProgramName (current) + ", edited" : PresetBox::noneName();
}

// Both of these read the parameters themselves. The blocks' own copies catch up with automation from the host's thread a moment later, and
// up to 0.4.3 these read those copies, so they could be built from the values before and stay that way until something else moved.
void TapeColorEditor::updateRestingTiles()
{
    auto& a = processorRef.apvts;
    const bool companderOn = juce::roundToInt (a.getRawParameterValue ("compander")->load()) != 0;
    encode.setResting (! companderOn);
    decode.setResting (! companderOn);
    deviceMix.setResting (juce::roundToInt (a.getRawParameterValue ("device")->load()) == 0);
    dropRate.setResting (a.getRawParameterValue ("dropdepth")->load() <= 0.0f);
}

void TapeColorEditor::updateSummary()
{
    juce::StringArray changed;
    auto& a = processorRef.apvts;
    const auto add = [&changed, &a] (const char* name, const char* id, const ui::Tile& tile)
    {
        const auto& param = *a.getParameter (id);
        if (std::abs (param.getValue() - param.getDefaultValue()) > 1.0e-4f)
            changed.add (juce::String (name) + " " + tile.textFor (a.getRawParameterValue (id)->load()));
    };
    add ("Hardness", "hardness", hardness);
    add ("Texture", "texture", texture);
    add ("Dropouts", "dropdepth", dropDepth);
    if (juce::roundToInt (a.getRawParameterValue ("compander")->load()) != 0)
        changed.add (juce::String ("Compander ") + a.getParameter ("compander")->getCurrentValueAsText());
    add ("Low cut", "lowcut", lowCut);
    add ("High cut", "highcut", highCut);
    add ("Head gap", "spacing", spacing);
    add ("Head tilt", "azimuth", azimuth);
    if (juce::roundToInt (a.getRawParameterValue ("device")->load()) != 0)
        changed.add (a.getParameter ("device")->getCurrentValueAsText());
    add ("Random", "random", randomDrift);
    add ("Stereo", "stereodrift", stereoDrift);
    advancedHeader->setSummary (changed.joinIntoString (", "));
}

void TapeColorEditor::updateSettings()
{
    bool changed = false;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const float v = values[i]->load (std::memory_order_relaxed);
        if (v != lastValues[i])
        {
            lastValues[i] = v;
            changed = true;
        }
    }

    // Not parameters, so watched here: the session's rate (a host can change it with the window open, or open the window before it says)
    // and the response a loaded state or automation has set.
    const double rate = processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 48000.0;
    const bool old = processorRef.oldResponse.load (std::memory_order_relaxed);
    if (rate != lastRate || old != lastOld)
    {
        lastRate = rate;
        lastOld = old;
        changed = true;
    }

    using namespace tapecolor::display;
    const int companderMode = (int) std::round (lastValues[12]);
    const double encodeAmount = (double) lastValues[13] * 0.01, decodeAmount = (double) lastValues[14] * 0.01;

    // A mismatched dynamic compander really does move the tone with the sound, so the line follows, a few times a second.
    const float boostNow = processorRef.readingCompanderDb.load (std::memory_order_relaxed);
    const bool boostMatters = companderMode != 0 && std::abs (encodeAmount - decodeAmount) > 0.005;
    const bool boostMoved = boostMatters && std::abs (boostNow - usedBoost) >= 1.0f && sinceBoostUpdate >= 0.12;

    if (! changed && ! boostMoved)
        return;

    sinceBoostUpdate = 0.0;
    usedBoost = boostNow;

    Settings s;
    s.driveDb = (double) lastValues[0];
    s.bias = (double) lastValues[1] * 0.01;
    s.speedPos = (double) lastValues[2];
    s.age = (double) lastValues[3] * 0.01;
    s.hardness = (double) lastValues[4] * 0.01;
    s.spacing = (double) lastValues[5] * 0.01;
    s.wow = (double) lastValues[6] * 0.01;
    s.flutter = (double) lastValues[7] * 0.01;
    s.noisePercent = (double) lastValues[8];
    s.lowCut = (double) lastValues[9];
    s.highCut = (double) lastValues[10];
    s.mix = (double) lastValues[11] * 0.01;
    s.oldResponse = processorRef.oldResponse.load (std::memory_order_relaxed);
    s.compander = companderMode;
    s.encode = encodeAmount;
    s.decode = decodeAmount;
    s.companderBoostDb = (double) (companderMode == 0 ? 0.0f : boostNow);
    s.device = (int) std::round (lastValues[15]);
    s.deviceMix = (double) lastValues[16] * 0.01;
    s.sampleRate = rate;
    s.outputDb = (double) lastValues[18];
    s.autoLevel = lastValues[19] >= 0.5f;

    // Each picture is drawn again only when something it shows has changed. Up to 0.4.3 any of the 26 controls redrew both, so an Output or
    // Texture ride rebuilt them on every frame.
    const bool bypassed = lastValues[17] >= 0.5f;
    const bool responseMoved = ! drawnOnce || bypassed != lastBypassed || ! sameResponse (s, settings);
    const bool curveMoved = ! drawnOnce || bypassed != lastBypassed || ! sameCurve (s, settings);
    settings = s;
    lastBypassed = bypassed;
    drawnOnce = true;
    if (responseMoved)
        response.setSettings (s, bypassed);
    if (curveMoved)
        curve.setSettings (s, bypassed);
    if (responseMoved || curveMoved)
        sinceMoved = 0.0;
    reel.setAge ((float) s.age);
    ageValue->setValue (lastValues[3]);

    const double top = topEndHz (s), hiss = hissDb (s), cents = wobbleCents (s);
    const auto hissNow = hiss < -200.0 ? juce::String ("None") : minusSign() + juce::String (std::abs (juce::roundToInt (hiss))) + " dB";
    const auto wobbleNow = cents < 0.05 ? juce::String ("None") : juce::String (cents, cents < 10.0 ? 1 : 0) + " cents";
    const auto topNow = juce::String (top * 0.001, 1) + " kHz";

    if (hissNow != hissText || wobbleNow != wobbleText || topNow != topEndText)
    {
        hissText = hissNow;
        wobbleText = wobbleNow;
        topEndText = topNow;
        readoutPicture = {};
        repaint (readoutArea);
    }

    updateRestingTiles();
    updateSummary();
    presets.refresh();
}

// ---- reading the sound --------------------------------------------------------------------------------------------

void TapeColorEditor::readAudio (double seconds)
{
    const double rate = processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 48000.0;

    if (rate != columnsRate)
    {
        columnsRate = rate;
        const float binHz = (float) rate / (float) fftSize;
        for (int i = 0; i < ResponseView::columns; ++i)
        {
            const float centre = ResponseView::columnHz (i);
            const float spread = std::pow (1000.0f, 0.5f / (float) ResponseView::columns);
            binFrom[(size_t) i] = juce::jlimit (1, fftSize / 2 - 1, (int) std::ceil (centre / spread / binHz));
            binTo[(size_t) i] = juce::jlimit (1, fftSize / 2 - 1, (int) std::floor (centre * spread / binHz));
            binBlend[(size_t) i] = centre / binHz;
            tilt[(size_t) i] = 3.0f * std::log2 (centre / 1000.0f);   // sound has less energy up high, so a scale tilted like this reads level with the ear
        }
    }

    const std::uint32_t written = processorRef.tapWritten.load (std::memory_order_acquire);
    const std::uint32_t fresh = written - lastWritten;
    lastWritten = written;

    constexpr std::uint32_t mask = PluginProcessor::tapSize - 1;
    const float peak = processorRef.tapPeak.exchange (0.0f, std::memory_order_relaxed);

    if (fresh > 0)
        sinceAudio = 0.0;
    else
        sinceAudio += seconds;

    // Quick up, slow down, the way a level meter reads.
    heldPeak = juce::jmax (peak, heldPeak * (float) std::exp (-seconds / 0.18));
    if (heldPeak < 1.0e-5f)
        heldPeak = 0.0f;
    signalPresent = heldPeak > 3.0e-4f && sinceAudio < 0.25;

    if (curve.setPeak (heldPeak))
        sinceMoved = 0.0;

    if (fresh == 0 && sinceAudio >= 0.25)
    {
        // The host stopped calling: nothing is coming off the tape. Let the picture fall away.
        bool anything = false;
        const float fall = (float) seconds * 60.0f;
        for (int i = 0; i < ResponseView::columns; ++i)
        {
            inDb[(size_t) i] = juce::jmax (-200.0f, inDb[(size_t) i] - fall);
            outDb[(size_t) i] = juce::jmax (-200.0f, outDb[(size_t) i] - fall);
            anything = anything || inDb[(size_t) i] > -125.0f || outDb[(size_t) i] > -125.0f;
        }
        if (anything && response.setSpectra (inDb, outDb))
            sinceMoved = 0.0;
        return;
    }

    if (fresh == 0)
        return;

    // Until a whole transform's worth has come in since the window opened, the older part of the tap is from the last time it was open,
    // perhaps at another rate. That part reads as silence.
    if (! tapFilled && written - openedAt >= (std::uint32_t) fftSize)
        tapFilled = true;
    const int stale = tapFilled ? 0 : fftSize - (int) (written - openedAt);

    // Both signals through one complex transform, then pulled apart. That is half the work of two.
    for (int i = 0; i < fftSize; ++i)
    {
        const std::uint32_t at = (written - (std::uint32_t) fftSize + (std::uint32_t) i) & mask;
        const float w = i < stale ? 0.0f : hann[(size_t) i];
        fftIn[(size_t) i] = { processorRef.tapIn[at].load (std::memory_order_relaxed) * w, processorRef.tapOut[at].load (std::memory_order_relaxed) * w };
    }
    fft.perform (fftIn.data(), fftOut.data(), false);

    const float norm = 4.0f / (float) fftSize;
    const auto powers = [&] (int k, float& pIn, float& pOut)
    {
        const auto a = fftOut[(size_t) k], b = fftOut[(size_t) (fftSize - k)];
        const float inRe = 0.5f * (a.real() + b.real()), inIm = 0.5f * (a.imag() - b.imag());
        const float outRe = 0.5f * (a.imag() + b.imag()), outIm = 0.5f * (b.real() - a.real());
        pIn = (inRe * inRe + inIm * inIm) * norm * norm;
        pOut = (outRe * outRe + outIm * outIm) * norm * norm;
    };

    const float attack = 1.0f - (float) std::exp (-seconds / 0.025), release = 1.0f - (float) std::exp (-seconds / 0.16);

    for (int i = 0; i < ResponseView::columns; ++i)
    {
        float sumIn = 0.0f, sumOut = 0.0f;
        const int from = binFrom[(size_t) i], to = binTo[(size_t) i];

        if (to >= from)
        {
            for (int k = from; k <= to; ++k)
            {
                float a = 0.0f, b = 0.0f;
                powers (k, a, b);
                sumIn += a;
                sumOut += b;
            }
            sumIn /= (float) (to - from + 1);
            sumOut /= (float) (to - from + 1);
        }
        else
        {
            // Fewer bins than columns down low: read between the two nearest.
            const float place = juce::jlimit (1.0f, (float) (fftSize / 2 - 2), binBlend[(size_t) i]);
            const int k = (int) place;
            const float t = place - (float) k;
            float a0 = 0.0f, b0 = 0.0f, a1 = 0.0f, b1 = 0.0f;
            powers (k, a0, b0);
            powers (k + 1, a1, b1);
            sumIn = a0 + t * (a1 - a0);
            sumOut = b0 + t * (b1 - b0);
        }

        const float dbIn = 10.0f * std::log10 (sumIn + 1.0e-14f) + tilt[(size_t) i];
        const float dbOut = 10.0f * std::log10 (sumOut + 1.0e-14f) + tilt[(size_t) i];
        auto& shownIn = inDb[(size_t) i];
        auto& shownOut = outDb[(size_t) i];
        shownIn += (dbIn - shownIn) * (dbIn > shownIn ? attack : release);
        shownOut += (dbOut - shownOut) * (dbOut > shownOut ? attack : release);
    }

    if (response.setSpectra (inDb, outDb))
        sinceMoved = 0.0;
}

bool TapeColorEditor::tick (double seconds)
{
    sinceMoved += seconds;
    sinceBoostUpdate += seconds;
    textAge += seconds;

    // A state loaded while the window is open can open or shut Advanced.
    if (const bool flag = processorRef.advancedOpen.load (std::memory_order_relaxed); flag != drawerShown)
        showDrawer (flag);

    updateSettings();
    readAudio (seconds);

    const bool running = signalPresent && lastValues[17] < 0.5f;
    constexpr double turns[] = { 0.22, 0.44, 0.88, 1.76 };
    const int speedIndex = juce::jlimit (0, 3, (int) std::round (lastValues[2]));
    if (reel.advance (seconds, turns[speedIndex], (double) processorRef.readingHeadMs.load (std::memory_order_relaxed), running))
        sinceMoved = 0.0;

    // Numbers change about 8 times a second: faster can't be read.
    if (textAge >= 0.12)
    {
        textAge = 0.0;
        if (curve.updateReading())
            sinceMoved = 0.0;
        const float boost = processorRef.readingCompanderDb.load (std::memory_order_relaxed);
        const bool on = compander.getSelected() != 0;
        if (on && std::abs (boost - shownBoost) >= 0.1f)
        {
            shownBoost = boost;
            compander.setReading ("Boost " + juce::String (boost, 1) + " dB");
        }
        else if (! on && shownBoost > -50.0f)
        {
            shownBoost = -100.0f;
            compander.setReading ({});
        }
    }

    return sinceMoved < 0.6;
}

void TapeColorEditor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double seconds = juce::jlimit (0.001, 0.25, now - lastTime);
    lastTime = now;

    const bool moving = tick (seconds);

    // Fast only while something on screen is moving. A stopped session polls, and draws nothing.
    if (moving != fast)
    {
        fast = moving;
        startTimerHz ((int) (fast ? fastRate : slowRate));
    }
}

// ---- hints --------------------------------------------------------------------------------------------------------

void TapeColorEditor::mouseEnter (const juce::MouseEvent& e)  { showHintFor (e.eventComponent); }
void TapeColorEditor::mouseMove (const juce::MouseEvent& e)   { showHintFor (e.eventComponent); }
void TapeColorEditor::mouseExit (const juce::MouseEvent&)     { setHint (restingHint); }

void TapeColorEditor::showHintFor (juce::Component* c)
{
    for (; c != nullptr && c != this; c = c->getParentComponent())
        if (auto* hinted = dynamic_cast<Hinted*> (c))
        {
            setHint (hinted->hint);
            return;
        }
    setHint (restingHint);
}

void TapeColorEditor::setHint (const juce::String& text)
{
    if (text != hintText)
    {
        hintText = text;
        advancedHeader->setSummaryShown (text == restingHint);
        repaint (hintArea);
    }
}

void TapeColorEditor::previewHint (const juce::String& paramId)
{
    for (int i = 0; i < getNumChildComponents(); ++i)
        if (getChildComponent (i)->getComponentID() == paramId)
            showHintFor (getChildComponent (i));
}

void TapeColorEditor::hoverTile (const juce::String& paramId, bool shouldShow)
{
    for (auto* t : { &drive, &bias, &noise, &wow, &flutter, &mix, &output })
        if (t->getComponentID() == paramId)
            t->setForcedHover (shouldShow);
}

void TapeColorEditor::startTypingTile (const juce::String& paramId)
{
    for (auto* t : { &drive, &bias, &noise, &wow, &flutter, &mix, &output })
        if (t->getComponentID() == paramId)
            t->startTyping();
}

void TapeColorEditor::startTypingAge()
{
    ageValue->startTyping();
}

juce::String TapeColorEditor::getAgeText() const    { return ageValue->shownText(); }
void TapeColorEditor::typeAge (const juce::String& text)  { ageValue->typeText (text); }
juce::String TapeColorEditor::getSummary() const    { return advancedHeader->getSummary(); }

// ---- drawing and layout -------------------------------------------------------------------------------------------

void TapeColorEditor::paint (juce::Graphics& g)
{
    g.fillAll (tone::base);

    if (g.clipRegionIntersects (titleArea))
    {
        g.setColour (tone::text);
        g.setFont (font (Weight::medium, 24.0f));
        g.drawText ("Tape Color", titleArea, juce::Justification::centredLeft, false);
    }

    const auto sectionName = [&g] (const char* name, juce::Rectangle<int> area)
    {
        if (g.clipRegionIntersects (area))
        {
            g.setColour (tone::muted);
            g.setFont (font (Weight::medium, 13.0f));
            g.drawText (name, area, juce::Justification::centredLeft, false);
        }
    };

    sectionName ("Tape", tapeLabel);
    sectionName ("Motion", motionLabel);
    sectionName ("Finish", outLabel);

    if (isAdvancedOpen())
    {
        sectionName ("Surface", surfaceLabel);
        sectionName ("Compander", companderLabel);
        sectionName ("Playback", playbackLabel);
        sectionName ("Device", deviceLabel);
        sectionName ("Drift", driftLabel);
    }

    // The reel's repaints can stretch the area being drawn over these, so they are drawn once and kept.
    if (g.clipRegionIntersects (readoutArea) || g.clipRegionIntersects (ribbonEnds))
    {
        const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        const auto strip = readoutArea.getUnion (ribbonEnds);
        const int w = juce::jmax (1, juce::roundToInt ((float) strip.getWidth() * scale)), h = juce::jmax (1, juce::roundToInt ((float) strip.getHeight() * scale));

        if (! readoutPicture.isValid() || readoutPicture.getWidth() != w || readoutPicture.getHeight() != h)
        {
            readoutPicture = juce::Image (juce::Image::ARGB, w, h, true);
            juce::Graphics p (readoutPicture);
            p.addTransform (juce::AffineTransform::scale (scale).translated (-(float) strip.getX() * scale, -(float) strip.getY() * scale));

            const auto readout = [&] (juce::Rectangle<int> area, const char* name, const juce::String& value)
            {
                p.setColour (tone::muted);
                p.setFont (font (Weight::regular, 12.5f));
                p.drawText (name, area.removeFromTop (17), juce::Justification::centredRight, false);
                p.setColour (tone::text);
                p.setFont (font (Weight::medium, 17.0f, true));
                p.drawText (value, area, juce::Justification::centredRight, false);
            };
            auto r = readoutArea;
            const int third = r.getWidth() / 3;
            readout (r.removeFromLeft (third), "Top end", topEndText);
            readout (r.removeFromLeft (third), "Hiss", hissText);
            readout (r, "Wobble", wobbleText);

            p.setColour (tone::faint);
            p.setFont (font (Weight::regular, 12.5f));
            p.drawText ("Fresh reel", ribbonEnds, juce::Justification::centredLeft, false);
            p.drawText ("Worn cassette", ribbonEnds, juce::Justification::centredRight, false);
        }

        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.setColour (juce::Colours::black);
        g.drawImageTransformed (readoutPicture, juce::AffineTransform::scale (1.0f / scale).translated ((float) strip.getX(), (float) strip.getY()));
    }

    if (g.clipRegionIntersects (hintArea))
    {
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (hintText, hintArea, juce::Justification::centredRight, true);
    }
}

void TapeColorEditor::resized()
{
    if (! constructed)
        return;

    processorRef.windowWidth.store (getWidth(), std::memory_order_relaxed);
    processorRef.windowHeight.store (getHeight(), std::memory_order_relaxed);
    readoutPicture = {};

    auto r = getLocalBounds().withTrimmedLeft (margin).withTrimmedRight (margin);

    // Top bar.
    auto bar = r.removeFromTop (headerHeight).withTrimmedTop (12).withTrimmedBottom (10);
    titleArea = bar.removeFromLeft (138);
    presets.setBounds (bar.removeFromLeft (232));
    bypass.setBounds (bar.removeFromRight (84));
    bar.removeFromRight (6);
    autoLevel.setBounds (bar.removeFromRight (104));

    // Bottom up. The picture takes whatever is left.
    auto foot = r.removeFromBottom (footerHeight).withTrimmedBottom (8);
    advancedHeader->setBounds (foot.withWidth (foot.getWidth() * 3 / 5));
    hintArea = foot.withTrimmedLeft (100);

    const bool open = isAdvancedOpen();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &hardness, &texture, &dropDepth, &dropRate, &compander, &encode, &decode, &lowCut, &highCut,
                                                                        &spacing, &azimuth, &device, &deviceMix, &randomDrift, &stereoDrift })
        c->setVisible (open);

    if (open)
    {
        auto drawer = r.removeFromBottom (drawerHeight - 8);
        r.removeFromBottom (8);
        const int total = drawer.getWidth();
        const int gap = 22;
        const float unit = (float) (total - 4 * gap) / 870.0f;
        const auto column = [&] (float weight, juce::Rectangle<int>& label)
        {
            auto c = drawer.removeFromLeft (juce::roundToInt (weight * unit));
            drawer.removeFromLeft (gap);
            label = c.removeFromTop (rowLabel);
            return c;
        };
        constexpr int small = 66, inner = 6;
        const auto twoRows = [&] (juce::Rectangle<int> c, juce::Component& a, juce::Component& b, juce::Component& d, juce::Component& e)
        {
            auto top = c.removeFromTop (small);
            c.removeFromTop (inner);
            auto bottom = c.removeFromTop (small);
            const int half = (top.getWidth() - inner) / 2;
            a.setBounds (top.removeFromLeft (half));
            b.setBounds (top.withTrimmedLeft (inner));
            d.setBounds (bottom.removeFromLeft (half));
            e.setBounds (bottom.withTrimmedLeft (inner));
        };

        // Weights out of 870. Up to 0.4.3 Surface 190, Playback 200, Device 160 and Drift 110, which cut "Dictation Machine" and
        // "Dropout rate" at the smallest window.
        twoRows (column (200.0f, surfaceLabel), hardness, texture, dropDepth, dropRate);

        {
            auto c = column (210.0f, companderLabel);
            compander.setBounds (c.removeFromTop (small));
            c.removeFromTop (inner);
            auto bottom = c.removeFromTop (small);
            const int half = (bottom.getWidth() - inner) / 2;
            encode.setBounds (bottom.removeFromLeft (half));
            decode.setBounds (bottom.withTrimmedLeft (inner));
        }

        twoRows (column (194.0f, playbackLabel), lowCut, highCut, spacing, azimuth);

        {
            auto c = column (176.0f, deviceLabel);
            device.setBounds (c.removeFromTop (small));
            c.removeFromTop (inner);
            deviceMix.setBounds (c.removeFromTop (small));
        }

        {
            auto c = column (90.0f, driftLabel);
            randomDrift.setBounds (c.removeFromTop (small));
            c.removeFromTop (inner);
            stereoDrift.setBounds (c.removeFromTop (small));
        }
    }

    // The main row, in three groups.
    {
        auto row = r.removeFromBottom (rowLabel + tileHeight);
        r.removeFromBottom (sectionGap);
        auto labels = row.removeFromTop (rowLabel);
        const int inner = 6, groupGap = 22;
        const float unit = (float) (row.getWidth() - 5 * inner - 2 * groupGap) / 910.0f;
        int x = row.getX();
        const auto place = [&] (juce::Component& c, float weight, int gapAfter)
        {
            const int w = juce::roundToInt (weight * unit);
            c.setBounds (x, row.getY(), w, row.getHeight());
            x += w + gapAfter;
        };

        const int tapeX = x;
        place (drive, 148.0f, inner);
        place (bias, 104.0f, inner);
        place (speed, 188.0f, inner);   // 214 up to 0.4.3, when Output had no room for "+12.0 dB"
        place (noise, 88.0f, groupGap);
        const int motionX = x;
        place (wow, 88.0f, inner);
        place (flutter, 88.0f, groupGap);
        const int outX = x;
        place (mix, 84.0f, inner);
        output.setBounds (x, row.getY(), row.getRight() - x, row.getHeight());

        tapeLabel = labels.withX (tapeX).withWidth (motionX - tapeX - groupGap);
        motionLabel = labels.withX (motionX).withWidth (outX - motionX - groupGap);
        outLabel = labels.withX (outX).withRight (row.getRight());
    }

    // Age: the big idea, right across the window.
    {
        auto band = r.removeFromBottom (ageHeight);
        r.removeFromBottom (sectionGap);
        reel.setBounds (band.removeFromLeft (ageHeight));
        band.removeFromLeft (18);
        auto top = band.removeFromTop (54);
        readoutArea = top.removeFromRight (306).withTrimmedTop (8).withTrimmedBottom (4);
        ageValue->setBounds (top.withTrimmedRight (12).withTrimmedBottom (4));
        ribbonEnds = band.removeFromBottom (18);
        ribbon.setBounds (band);
    }

    // The picture: what the tape does to the tone, and how hard it is working.
    {
        const int curveWidth = juce::jlimit (210, 330, r.getHeight() - 8);
        curve.setBounds (r.removeFromRight (curveWidth));
        r.removeFromRight (8);
        response.setBounds (r);
    }
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new TapeColorEditor (*this);
}
