#pragma once
// Switchboard's window. A rail on top (the mark, the view menu, All controls), one line per plug-in with its main controls below.
//
// It reads the link folder only while it is open: a 10 Hz timer scans the cards (hub/Registry.h) and checks the commands it sent
// (hub/Commands.h); during a drag the timer runs at 30 Hz, sends what the drag throttle let wait and paints the moving control, so
// the window never paints more than 30 times a second. Only what changed repaints. Nothing here touches the audio thread.
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include "PluginProcessor.h"
#include "Registry.h"
#include "Commands.h"
#include "Controls.h"
#include "Look.h"

namespace hub
{
class Board;
class EmptyState;
class AllControlsPanel;
class RailButton;
}

class SwitchboardEditor final : public juce::AudioProcessorEditor, private juce::Timer, private hub::Wire
{
public:
    enum class ViewKind { thisTrack, allTracks, oneTrack };
    struct View
    {
        ViewKind kind = ViewKind::thisTrack;
        juce::String track;   // oneTrack: the track ("" for the cards with no track name)
    };

    explicit SwitchboardEditor (SwitchboardProcessor&);
    ~SwitchboardEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The view menu: This track (the default), All tracks, or one of the tracks seen.
    void chooseView (View);
    View view() const { return current; }
    juce::String viewLabel() const;
    juce::StringArray viewMenuItems() const;
    juce::String shownTrack() const { return knownTrack; }   // the name Logic gave this track, empty until it does

    // One tick of the window at a unix time: scan, check commands, update what shows. The timer calls the same.
    void tickNow (double unixNow);
    std::function<double()> clock;   // unix seconds; tests set their own

    // What shows, for tests and the CPU check.
    juce::StringArray shownNames() const;
    juce::String emptyText() const;
    bool offersAllTracks() const;
    hub::ControlView* findControl (const juce::String& instance, const juce::String& param) const;
    bool isGreyed (const juce::String& instance) const;
    juce::String statusOf (const juce::String& instance) const;
    void openAllControls (const juce::String& instance);   // empty: every plug-in in the view
    void closeAllControls();
    int allControlsRows() const;                           // -1 when closed
    void searchAllControls (const juce::String&);
    hub::ControlView* findAllControlsControl (const juce::String& instance, const juce::String& param) const;
    int paintCount() const { return hub::look::paints; }
    hub::Registry& registry() { return reg; }

private:
    struct Section
    {
        juce::String track;
        bool header = false;
        std::vector<const hub::Instance*> items;
    };

    void timerCallback() override;
    void tickAt (double t, bool forceScan);
    void refreshView (double t);
    std::vector<Section> sections() const;
    std::vector<const hub::Instance*> inView() const;
    bool live (const hub::Instance&, double t) const;
    void showViewMenu();
    void layoutRail();
    static juce::String trackTitle (const juce::String& track) { return track.isEmpty() ? juce::String ("No track name") : track; }

    // hub::Wire
    double now() const override { return clock ? clock() : smooth::linkdetail::unixNow(); }
    juce::String send (const juce::String& instance, const juce::String& param, double value, hub::Gesture) override;
    juce::String sendText (const juce::String& instance, const juce::String& param, const juce::String& text) override;
    void dragging (hub::ControlView&, bool on) override;

    SwitchboardProcessor& owner;
    hub::look::LookAndFeel lnf;
    hub::Registry reg;
    hub::Commands cmds;
    View current;
    juce::String knownTrack, status;
    bool first = true, closing = false;
    double lastScan = -1.0e18;
    std::map<juce::String, juce::int64> unanswered;   // instance -> its seq when a command went unanswered
    juce::Component::SafePointer<hub::ControlView> dragged;

    std::unique_ptr<hub::RailButton> viewButton, allButton;
    std::unique_ptr<hub::Board> board;
    juce::Viewport viewport;
    std::unique_ptr<hub::EmptyState> empty;
    std::unique_ptr<hub::AllControlsPanel> panel;
    juce::Rectangle<int> railArea, statusArea, bodyArea, footerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SwitchboardEditor)
};
