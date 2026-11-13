#pragma once
#include "Controls.h"
#include "../PluginProcessor.h"
#include <array>

// The hero, as two pieces that line up edge to edge: the waveform cut into coloured slices, and under it the ribbons that carry
// each slice down to its key. Everything still is cached in images and only rebuilt when a parameter or the sample changes.
// A frame only copies parts of a brighter copy of the same image over the top, as far as each playing voice has got.

class SliceWaveform final : public juce::Component,
                            public ui::Hinted
{
public:
    explicit SliceWaveform (PluginProcessor&);

    /** Called on the window's timer. Marks the layers stale if anything that shows has changed. */
    void update (const slicecore::SliceTable&, int selected, bool zoomed);
    /** Reads the voices and repaints only what moved. Returns true while any voice is sounding. */
    bool updateVoices();

    void setDropActive (bool);
    std::function<void()> onEmptyClick;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    // Public so a test can drive the window with no message loop running.
    int frameAtX (float x) const;
    int sliceAtFrame (int frame) const;
    int markerNearX (float x) const;
    float xForFrame (int frame) const;
    juce::Point<float> startHandlePosition() const;
    double timeStepSeconds() const;   // the time scale's step at the current view and width
    bool isDraggingSomething() const noexcept  { return dragging >= 0 || draggingStart; }
    int getRebuildCount() const noexcept  { return rebuilds; }

    int getViewFrom() const noexcept  { return viewFrom; }
    int getViewTo() const noexcept    { return viewTo; }

    // For the CPU tool.
    int getPaintCount() const noexcept  { return paints; }
    double getPaintSeconds() const noexcept  { return paintSeconds; }
    double getPaintedArea() const noexcept  { return paintedArea; }
    int getVoiceCount() const noexcept;

    static constexpr int stripHeight = 22, axisHeight = 20;

private:
    struct Wipe
    {
        bool on = false;
        int left = 0, right = 0;
        float head = 0.0f, level = 0.0f;
        int slot = 0;
    };

    struct Look
    {
        slicecore::SliceTable table;
        int selected = 0, mode = 0, width = 0, height = 0, viewFrom = 0, viewTo = 0;
        float startMs = 0.0f;
        const LoadedSample* sample = nullptr;
        bool bypassed = false, zoomed = false;
        std::array<float, PluginProcessor::kSlots> level {}, pitch {};
        std::array<bool, PluginProcessor::kSlots> reverse {};
        bool operator== (const Look&) const;
    };

    void rebuildLayers (float scale);
    void drawLayer (juce::Graphics&, float scale, bool lit);
    void drawEmpty (juce::Graphics&);
    void measureColumns (int columns);
    float startFrame() const;
    juce::Rectangle<float> startTab() const;
    bool startTabShows() const;

    PluginProcessor& processor;
    LoadedSample::Ptr sample;
    slicecore::SliceTable table;
    Look look;
    int selected = 0, dragging = -1, hoverMarker = -1, viewFrom = 0, viewTo = 0;
    bool draggingStart = false, dropActive = false, stale = true;

    juce::Image still, litLayer;
    float layerScale = 0.0f;
    int rebuilds = 0;

    // One column of the waveform per physical pixel, so the level and reverse can move without reading the audio again.
    std::vector<float> columnLow, columnHigh;
    const LoadedSample* columnSample = nullptr;
    int columnFrom = -1, columnTo = -1, columnCount = 0;
    float largest = 0.01f;

    std::array<Wipe, PluginProcessor::kPlayheads> wipes;
    std::unique_ptr<juce::ParameterAttachment> startAttachment;

    int paints = 0;
    double paintSeconds = 0.0, paintedArea = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceWaveform)
};

/** The ribbons and the keyboard. Each slice flows down to its key, in its colour. */
class KeyMap final : public juce::Component,
                     public ui::Hinted
{
public:
    KeyMap (PluginProcessor&, SliceWaveform&);

    void update (const slicecore::SliceTable&, int selected);
    void updateVoices();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // Public so a test can drive the window with no message loop running.
    int noteAt (juce::Point<float>) const;
    juce::Point<float> rootTabCentre() const;
    juce::Point<float> keyCentre (int note) const;
    int getFirstKey() const noexcept  { return firstKey; }
    int getLastKey() const noexcept   { return lastKey; }
    bool isDraggingRoot() const noexcept  { return draggingRoot; }
    bool isLit (int note) const noexcept  { return juce::isPositiveAndBelow (note, 128) && lit[(size_t) note] > 0; }

    static constexpr int ribbonHeight = 38, keysHeight = 66;

private:
    struct Key
    {
        bool black = false;
        juce::Rectangle<float> face;     // the whole shape, for hit testing and ribbons
        juce::Rectangle<int> upper, lower;   // the parts that show, in whole pixels, to light it
    };

    void chooseRange (int root, int count, bool chromatic);
    void placeKeys();
    void rebuildLayers (float scale);
    void drawLayer (juce::Graphics&, float scale, bool litVersion);
    void drawRootTab (juce::Graphics&);
    int slotForNote (int note) const noexcept;
    juce::Rectangle<float> rootTab() const;
    juce::Colour keyColour (int note, bool litVersion) const;

    PluginProcessor& processor;
    SliceWaveform& waveform;
    slicecore::SliceTable table;
    int selected = 0, root = 60, firstKey = 48, lastKey = 84, viewFrom = 0, viewTo = 0;
    bool chromatic = false, hasSample = false, bypassed = false, draggingRoot = false, stale = true, hoverRoot = false, needsRange = false;
    std::array<Key, 128> keys;
    std::array<juce::Path, PluginProcessor::kSlots> ribbons;
    std::array<std::uint8_t, 128> lit {};     // 0 off, then how loud in a few steps, so a fade repaints a few times and not every frame
    std::array<std::uint8_t, PluginProcessor::kSlots> litSlots {};

    juce::Image still, litLayer;
    float layerScale = 0.0f;
    std::unique_ptr<juce::ParameterAttachment> rootAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyMap)
};

/** Base for the two envelope graphs: a few handles that drag real parameters, and nothing moves unless a parameter does. */
class EnvelopeGraph : public juce::Component,
                      public ui::Hinted
{
public:
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    bool isDraggingHandle() const noexcept  { return grabbed >= 0; }
    virtual int numHandles() const = 0;
    virtual juce::Point<float> handlePosition (int) const = 0;

    void setAccent (juce::Colour);

protected:
    struct Link
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attachment;
        float norm() const  { return param != nullptr ? param->getValue() : 0.0f; }
        float plain() const { return param != nullptr ? param->convertFrom0to1 (param->getValue()) : 0.0f; }
    };

    void bindLink (Link&, juce::RangedAudioParameter&);
    void setPlain (Link&, float plainValue);
    void setNorm (Link&, float normalised);
    void reset (Link&);

    /** Which parameters this handle moves. */
    virtual void linksFor (int handle, Link*& first, Link*& second) = 0;
    virtual void dragHandle (int handle, juce::Point<float>) = 0;

    juce::Rectangle<float> plot() const  { return getLocalBounds().toFloat().reduced (1.0f, 7.0f); }
    void drawHandles (juce::Graphics&);

    juce::Colour accent = ui::tone::text;
    int grabbed = -1, hovered = -1;
};

class AmpGraph final : public EnvelopeGraph
{
public:
    explicit AmpGraph (juce::AudioProcessorValueTreeState&);
    void bindSlot (int slot);
    void paint (juce::Graphics&) override;

    int numHandles() const override  { return 3; }
    juce::Point<float> handlePosition (int) const override;

private:
    void linksFor (int handle, Link*& first, Link*& second) override;
    void dragHandle (int handle, juce::Point<float>) override;
    float attackEnd() const;
    float decayEnd() const;
    float releaseStart() const;

    juce::AudioProcessorValueTreeState& state;
    Link attack, decay, sustain, release;
};

class PitchGraph final : public EnvelopeGraph
{
public:
    explicit PitchGraph (juce::AudioProcessorValueTreeState&);
    void paint (juce::Graphics&) override;

    int numHandles() const override  { return 2; }
    juce::Point<float> handlePosition (int) const override;

private:
    void linksFor (int handle, Link*& first, Link*& second) override;
    void dragHandle (int handle, juce::Point<float>) override;
    float yFor (float semis) const;

    Link amount, time;
};
