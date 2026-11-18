#pragma once
// The controls Switchboard draws for another plug-in's parameters: knobs, switches, two-way switches and menus on a plug-in's line,
// and slimmer ones in the All controls list. Each one shows the card's value, follows your hand at once while you set it, and goes
// back to the card once the plug-in has answered the last command (or failed to within 3 s).
#include <juce_gui_basics/juce_gui_basics.h>
#include "Commands.h"
#include "Look.h"

namespace hub
{
class ControlView;

// What a control talks to: the window.
struct Wire
{
    virtual ~Wire() = default;
    virtual double now() const = 0;                       // unix seconds
    virtual juce::String send (const juce::String& instance, const juce::String& param, double value, Gesture) = 0;
    virtual juce::String sendText (const juce::String& instance, const juce::String& param, const juce::String& text) = 0;
    virtual void dragging (ControlView&, bool on) = 0;    // on: the window ticks this control at 30 Hz until it says off
};

class ControlView : public juce::Component
{
public:
    // compact: the slim version for a row of the All controls list.
    static std::unique_ptr<ControlView> make (Wire&, const juce::String& instance, const Param&, juce::Colour cord, bool compact);

    // The width a control takes on a plug-in's line (its height is lineHeight).
    static int lineWidth (const Param&);
    static constexpr int lineHeight = 100;

    ~ControlView() override;

    Kind kind() const { return k; }
    const juce::String& instance() const { return inst; }
    const Param& param() const { return p; }
    double shownValue() const { return follow ? local : p.value; }
    juce::String shownText() const { return p.text; }

    // The control's values from a newer card. Repaints only when what it shows changed.
    void update (const Param&);
    // One of the window's commands was answered or failed. Once the last one this control sent is, it shows the card again.
    void settled (const juce::String& commandName);
    // From the window's timer while this control drags: sends what waited for its turn, and paints at most 30 times a second.
    void tick (double now);

protected:
    ControlView (Wire&, juce::String instance, const Param&, juce::Colour cord, bool compact);

    void sendValue (double v);     // a tap; shows v at once
    void sendDefault();            // text "default"; shows the default at once
    void dragStart();
    void dragBy (float pixels, bool fine, float pixelsForFullRange);
    void dragEnd();

    // Value colours dim when the plug-in is not responding (the control is then disabled).
    juce::Colour tint (juce::Colour c) const { return isEnabled() ? c : c.withMultipliedAlpha (0.45f); }
    void drawLabel (juce::Graphics&, juce::Rectangle<int>) const;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void enablementChanged() override { repaint(); }

    Wire& wire;
    const juce::String inst;
    Param p;
    const juce::Colour cord;
    const Kind k;
    const bool compact;
    bool hover = false;

private:
    void showLocal (double v, const juce::String& commandName);
    void afterSend();

    bool follow = false, needsPaint = false, dragTold = false;
    double local = 0.0, raw = 0.0;
    juce::String waitFor, nameBeforeDrag;
    DragThrottle throttle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlView)
};
} // namespace hub
