#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "window/Theme.h"
#include "window/Controls.h"
#include "window/PitchStrip.h"
#include "window/ModuleRow.h"
#include "window/ModuleGraph.h"

/** The window: tuning and the live pitch strip on top, the bold row of module blocks across the middle, and one panel underneath that belongs to the selected block.
    Laid out once at design size and scaled as a whole, except the strip, which sits at whole pixels so its scrolling image never needs resampling.
    All of it reads the processor through atomics and writes only through real parameters, so nothing here can block audio. */
class TuneFxEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    static constexpr int designWidth = 1120, designHeight = 720;

    explicit TuneFxEditor (PluginProcessor&);
    ~TuneFxEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** One display frame. The screen's refresh drives this normally. The offline renders call it directly. */
    void tick (double dtSeconds);

    /** Which module's controls are showing. */
    void selectModule (int);
    int getSelectedModule() const noexcept  { return selected; }

    /** Opens or closes Advanced. Without animation it is there straight away. */
    void setAdvancedOpen (bool shouldBeOpen, bool animate);
    bool isAdvancedOpen() const noexcept  { return processor.advancedOpen.load (std::memory_order_relaxed); }

    /** For the offline renders, which have no mouse. Lights a control and shows its hint. */
    void previewHover (const juce::String& paramId);

    // For tests.
    juce::String readingText() const;
    PitchStrip& getStrip() noexcept        { return strip; }
    ModuleRow& getRow() noexcept;
    ModuleGraph& getGraph() noexcept;
    juce::Component* findControl (const juce::String& paramId);
    void clickCustomKey (int key);
    juce::String headerTitle() const;          // the name in the header
    juce::String advancedLabel() const;        // the words on the Advanced button, and the room they have beside its arrow
    int advancedLabelRoom() const;
    int correctionBarPixels() const;           // how far the bar under the note reaches from the middle, in design pixels

    /** Whether frames are being driven by the screen right now. Off when nothing has moved for a second. */
    bool isRunningFrames() const noexcept { return vblank != nullptr; }

private:
    class Reading;
    class AdvancedButton;
    class CustomKeys;
    struct Panel;
    struct ModulePage;
    struct AdvancedPage;

    void timerCallback() override;
    void startFrames();
    void placeStrip();
    void showModule (int);
    void showHint (const juce::String&);
    void hintFromComponent (juce::Component*);

    static constexpr double maxFramesPerSecond = 30.0;

    PluginProcessor& processor;
    juce::SharedResourcePointer<fxui::Assets> assets;   // held for the window's life, so the fonts are loaded once
    fxui::LookAndFeel lookAndFeel;
    PitchStrip strip;
    std::unique_ptr<Panel> panel;
    int selected = 0;
    float advancedAmount = 0.0f;
    double lastFrameTime = 0.0, frameBudget = 0.0, sinceDrawn = 0.0, stillFor = 0.0;
    int slowCountdown = 0;
    bool constructed = false, advancedShown = false;
    std::unique_ptr<juce::VBlankAttachment> vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneFxEditor)
};
