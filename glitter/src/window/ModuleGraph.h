#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

/** The picture of the selected module, drawn from its real settings, and for most modules a pad: drag a dot and two real parameters move together.
    The picture is one cached image, rebuilt only when one of the module's settings changes. Only the dots and Motion's step or Filter's sweep go on top of it. */
class ModuleGraph final : public juce::Component,
                          public fxui::Hinted
{
public:
    explicit ModuleGraph (PluginProcessor&);

    void setModule (int);

    /** From the editor's frame. Returns true if anything on screen moved. */
    bool refresh();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** How many dots this module has, and for the tests, a drag of one dot to a place in the pad, in 0 to 1 each way. */
    int getNumHandles() const noexcept  { return (int) handles.size(); }
    void dragHandleTo (int handle, juce::Point<float> proportion);

private:
    struct Handle { juce::RangedAudioParameter* x = nullptr, *y = nullptr; int mapping = 0; };

    juce::Rectangle<float> plot() const;
    juce::Point<float> handlePosition (const Handle&) const;
    void moveHandle (const Handle&, juce::Point<float> position);
    int handleAt (juce::Point<float>) const;
    float raw (const char* id) const;
    juce::RangedAudioParameter* param (const char* id) const;
    int voiceNote() const;

    void rebuild (float pixelScale);
    void drawImage (juce::Graphics&, juce::Rectangle<float> area);
    void drawPitch (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawMotion (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawThroat (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawHarmony (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawDoubler (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawVocoder (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawDistortion (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawFilter (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawRing (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawDelay (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    void drawReverb (juce::Graphics&, juce::Rectangle<float>, juce::Colour);

    PluginProcessor& processor;
    int module = 0;
    juce::Image image;
    float imageScale = 0.0f;
    bool stale = true;
    std::vector<std::atomic<float>*> watched;
    std::vector<float> seen;
    std::vector<Handle> handles;
    std::array<juce::Rectangle<float>, 16> motionCells;
    int noteSeen = -1, scaleSeen = -1, stepShown = -1, hoveredHandle = -1, draggedHandle = -1, filterPixel = -1, filterClock = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleGraph)
};
