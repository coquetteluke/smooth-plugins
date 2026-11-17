#pragma once
// Switchboard's window: three screens and back buttons.
//
//   Tracks (home)  every track that has one of your plug-ins open, in load order, each row with its plug-ins. A tap opens the track.
//   A track       "< Tracks" at top left, the track name as the title, one line per plug-in with its main controls.
//   Controls      "< <track>" at top left, the plug-in as the title, every control of that plug-in in a searchable list.
//
// The processor keeps the screen (hub::Screen), so a closed and reopened window, a reloaded project or a command through the inbox
// (the hub inbox) all land here; the window follows any change of it within one tick. It reads the link folder only while it is open:
// a 10 Hz timer has the processor scan the cards (hub/Registry.h) and checks the commands it sent (hub/Commands.h); during a drag the
// timer runs at 30 Hz, sends what the drag throttle let wait and paints the moving control, so the window never paints more than 30
// times a second. Only what changed repaints. Nothing here touches the audio thread.
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include "PluginProcessor.h"
#include "Registry.h"
#include "Commands.h"
#include "Controls.h"
#include "Look.h"
#include "Screen.h"

namespace hub
{
class Board;
class TrackList;
class EmptyState;
class AllControlsPanel;
class RailButton;
}

class SwitchboardEditor final : public juce::AudioProcessorEditor, private juce::Timer, private hub::Wire
{
public:
    explicit SwitchboardEditor (SwitchboardProcessor&);
    ~SwitchboardEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // What the window shows, and a tap that changes it (the processor keeps it, the hub file says it at once).
    hub::Screen screen() const { return shown; }
    void show (const hub::Screen&);
    void goBack();                                // the back button: controls to its track, a track to Tracks

    juce::String titleText() const;               // "Switchboard" on Tracks, else the track ("No track name yet") or the plug-in
    juce::String subtitleText() const;            // on Tracks: "on <the track Switchboard is on>", empty until Logic names it
    juce::String backText() const;                // "< Tracks" or "< Lead Vox" (with the single angle quote), empty on Tracks
    juce::Component* backButton() const;
    juce::String shownTrack() const { return knownTrack; }   // the name Logic gave Switchboard's own track, empty until it does

    // The rows of Tracks, top to bottom. track is "" for the plug-ins with no track name; note names the ones not responding.
    struct TrackRow
    {
        juce::String track, title, note;
        juce::StringArray plugins;
    };
    std::vector<TrackRow> trackRows() const;
    juce::Component* trackRow (const juce::String& track) const;          // a row of Tracks, or nullptr
    juce::Component* controlsLink (const juce::String& instance) const;   // "All N controls" on a plug-in's line, or nullptr
    juce::TextEditor* searchBox() const;

    // One tick of the window at a unix time: scan, check commands, follow the screen, update what shows. The timer calls the same.
    void tickNow (double unixNow);
    std::function<double()> clock;   // unix seconds; tests set their own

    // What shows on a track's screen and the controls screen, for tests and the CPU check.
    juce::StringArray shownNames() const;
    juce::String emptyText() const;
    hub::ControlView* findControl (const juce::String& instance, const juce::String& param) const;
    bool isGreyed (const juce::String& instance) const;
    juce::String statusOf (const juce::String& instance) const;
    void openAllControls (const juce::String& instance);   // as "All N controls" on its line does
    void closeAllControls();                               // as the back button on the controls screen does
    int allControlsRows() const;                           // -1 when the controls screen is not showing
    void searchAllControls (const juce::String&);
    hub::ControlView* findAllControlsControl (const juce::String& instance, const juce::String& param) const;
    int paintCount() const { return hub::look::paints; }
    hub::Registry& registry() { return reg; }

private:
    void timerCallback() override;
    void tickAt (double t, bool forceScan);
    void follow();                           // the processor's screen to the window, when it changed
    void navigate (const hub::Screen&);
    void refreshView (double t);
    bool live (const hub::Instance&, double t) const;
    void layoutRail();
    void paintRail (juce::Graphics&);

    // hub::Wire
    double now() const override { return clock ? clock() : smooth::linkdetail::unixNow(); }
    juce::String send (const juce::String& instance, const juce::String& param, double value, hub::Gesture) override;
    juce::String sendText (const juce::String& instance, const juce::String& param, const juce::String& text) override;
    void dragging (hub::ControlView&, bool on) override;

    SwitchboardProcessor& owner;
    hub::look::LookAndFeel lnf;
    hub::Registry& reg;
    hub::Commands cmds;
    hub::Screen shown;
    int seenChanges = -1;
    juce::int64 seenGeneration = -1;
    juce::String knownTrack, status, hint;
    juce::String panelId;                             // the instance the controls screen opened on; another one starts a fresh search
    bool first = true, closing = false, fresh = true;
    double lastScan = -1.0e18;
    std::map<juce::String, juce::int64> unanswered;   // instance -> its seq when a command went unanswered
    juce::Component::SafePointer<hub::ControlView> dragged;

    std::unique_ptr<hub::RailButton> backBtn;
    std::unique_ptr<hub::TrackList> list;
    std::unique_ptr<hub::Board> board;
    juce::Viewport listView, boardView;
    std::unique_ptr<hub::EmptyState> empty;
    std::unique_ptr<hub::AllControlsPanel> panel;
    juce::Rectangle<int> railArea, titleArea, subtitleArea, statusArea, bodyArea, footerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SwitchboardEditor)
};
