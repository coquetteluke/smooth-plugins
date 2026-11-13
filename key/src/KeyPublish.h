#pragma once
// What Key shows and publishes, worked out from the analyser's reading and the controls: Manual, Relative Swap and Lock.
// No JUCE and no threads in here, so it can be tested directly. Used only from the background thread.
#include <algorithm>
#include "KeyAnalyser.h"

namespace smoothkey
{
enum class KeySource { heard = 0, manual = 1, locked = 2 };

inline constexpr const char* sourceNames[3] = { "heard", "manual", "locked" };

// The controls that decide what is shown, as read once per tick.
struct ShowControls
{
    bool manual = false;
    int manualKey = 0;
    bool manualMinor = false;
    bool swapRelative = false;
    bool lock = false;
    // Since 1.7.2: the key the window showed when Lock was clicked on there, taken in place of the newest one when Lock goes on.
    // -2 for none (Lock switched by the host or automation), -1 when the window showed no key, so Lock waits for a sure one.
    int offeredKey = -2;
    bool offeredMinor = false;
};

struct Shown
{
    int key = -1;               // 0 to 11 from C, -1 for no key yet
    bool minor = false;
    float confidence = 0.0f;
    int altKey = -1;            // the runner up, -1 for none
    bool altMinor = false;
    float altConfidence = 0.0f;
    float margin = 1.0f;        // 0 a tie, 1 no contest
    bool unsure = false;        // two sets of notes fit about equally
    KeySource source = KeySource::heard;
    bool lockWaiting = false;   // Lock is on and holds nothing yet
    // What it hears right now, after Relative Swap, whatever is on show. For the window while a key is held.
    int heardKey = -1;
    bool heardMinor = false;
    float heardConfidence = 0.0f;
};

class Publisher
{
public:
    // A follower acts on a reading from this confidence up. A waiting Lock takes the first reading that reaches it.
    static constexpr float sureEnough = 0.5f;
    // An unsure reading stays under what followers act on, whatever the analyser sent.
    static constexpr float unsureCeiling = 0.49f;

    // A loaded session: this key is held from now on, as long as Lock is on. key under 0 holds nothing.
    void restore (int key, bool minor) noexcept
    {
        held = key >= 0 ? key % 12 : -1;
        heldMinor = minor;
    }

    // A loaded session, with the Lock it was saved with. Lock is taken as that for a few ticks while the control catches up, and
    // a Lock that comes on with the session is never a click on Lock: it holds the session's key, or waits, and never takes the key
    // that happened to be on show. Since 1.7.2.
    void restoreSession (int key, bool minor, bool lockOn) noexcept
    {
        restore (key, minor);
        wasLocked = lockOn;
        sessionLock = lockOn;
        sessionTicks = 8;
    }

    int heldKey() const noexcept { return held; }
    bool heldIsMinor() const noexcept { return heldMinor; }

    Shown update (const Reading& r, const ShowControls& c) noexcept
    {
        Shown s;

        if (c.manual)
        {
            s.key = std::clamp (c.manualKey, 0, 11);
            s.minor = c.manualMinor;
            s.confidence = 1.0f;
            s.source = KeySource::manual;
        }
        else if (r.key >= 0)
        {
            s.key = r.key % 12;
            s.minor = r.minor;
            s.confidence = std::clamp (r.confidence, 0.0f, 1.0f);

            if (r.altKey >= 0)
            {
                s.altKey = r.altKey % 12;
                s.altMinor = r.altMinor;
                s.altConfidence = std::clamp (r.altConfidence, 0.0f, 1.0f);
                s.margin = std::clamp (r.margin, 0.0f, 1.0f);
            }

            // Relative Swap is a choice between two names for the same notes, so the runner up swaps with the key.
            if (c.swapRelative)
            {
                swap (s.key, s.minor);
                if (s.altKey >= 0) swap (s.altKey, s.altMinor);
            }

            if (s.altKey == s.key && s.altMinor == s.minor)
                s.altKey = -1;

            if (s.altKey < 0)
            {
                s.altMinor = false;
                s.altConfidence = 0.0f;
                s.margin = 1.0f;
            }

            s.unsure = r.unsure && s.altKey >= 0;
            if (s.unsure)
                s.confidence = std::min (s.confidence, unsureCeiling);

            s.heardKey = s.key;
            s.heardMinor = s.minor;
            s.heardConfidence = s.confidence;
        }

        bool lockOn = c.lock;
        if (sessionTicks > 0)
        {
            if (lockOn == sessionLock) sessionTicks = 0;
            else { lockOn = sessionLock; --sessionTicks; }
        }

        if (! lockOn)
        {
            held = -1;
        }
        else
        {
            if (held < 0)
            {
                // Switched on with a key on show: that key, sure or not, because it was chosen by hand. Clicked on in the window,
                // the key the window showed, which may be older than the newest one by a redraw.
                // Switched on with nothing on show: it waits, and takes the first key a follower would have acted on anyway.
                if (! wasLocked && c.offeredKey >= 0)
                {
                    held = c.offeredKey % 12;
                    heldMinor = c.offeredMinor;
                }
                else if (! wasLocked && c.offeredKey == -2 && lastKey >= 0)
                {
                    held = lastKey;
                    heldMinor = lastMinor;
                }
                else if (s.key >= 0 && (c.manual || (s.confidence >= sureEnough && ! s.unsure)))
                {
                    held = s.key;
                    heldMinor = s.minor;
                }
            }

            if (held >= 0)
            {
                s.key = held;
                s.minor = heldMinor;
                s.confidence = 1.0f;
                s.altKey = -1;
                s.altMinor = false;
                s.altConfidence = 0.0f;
                s.margin = 1.0f;
                s.unsure = false;
                s.source = KeySource::locked;
            }
            else
            {
                s.lockWaiting = true;
            }
        }

        wasLocked = lockOn;
        lastKey = s.key;
        lastMinor = s.minor;
        return s;
    }

private:
    static void swap (int& key, bool& minor) noexcept
    {
        key = minor ? (key + 3) % 12 : (key + 9) % 12;
        minor = ! minor;
    }

    int held = -1, lastKey = -1, sessionTicks = 0;
    bool heldMinor = false, lastMinor = false, wasLocked = false, sessionLock = false;
};
} // namespace smoothkey
