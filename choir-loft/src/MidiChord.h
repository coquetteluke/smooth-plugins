#pragma once
// Held MIDI notes and which voice sings which one. No allocation, safe on the audio thread.
//
// Assignment rules:
// - More notes than voices: the lowest and the highest always get a voice, because they define the chord, then the newest notes.
// - Fewer notes than voices: the spare voices rest. They do not double anything.
// - The voices take the notes in the way that moves them least in total, so a note that stays held keeps its voice (and its pan and
//   its singer), and voices do not cross on a chord change. On a fresh chord the lowest note goes to the first voice.
#include <algorithm>
#include <array>
#include <cstdlib>

namespace smoothharmony
{
class MidiChord
{
public:
    static constexpr int maxVoices = 4, maxNotes = 32;

    MidiChord() { clearAll(); }

    void clearAll() noexcept
    {
        numHeld = 0;
        pedal = false;
        hold = false;
        order = 0;
        changed = true;
        for (auto& n : lastNote) n = -1;
        for (auto& n : voiceNote) n = -1;
    }

    void setHold (bool on) noexcept
    {
        if (hold == on) return;
        hold = on;
        if (! hold) releaseLatched();
    }

    void noteOn (int channel, int note) noexcept
    {
        // Under Chord Hold the first new note after everything was let go starts a new chord.
        if (hold && ! anyKeyDown())
            dropReleased();
        for (int i = 0; i < numHeld; ++i)
            if (held[(size_t) i].note == note && held[(size_t) i].channel == channel)
            {
                held[(size_t) i].keyDown = true;
                held[(size_t) i].order = ++order;
                changed = true;
                return;
            }
        if (numHeld >= maxNotes)
            removeAt (oldest());
        held[(size_t) numHeld++] = { note, channel, ++order, true };
        changed = true;
    }

    void noteOff (int channel, int note) noexcept
    {
        for (int i = 0; i < numHeld; ++i)
            if (held[(size_t) i].note == note && held[(size_t) i].channel == channel)
            {
                held[(size_t) i].keyDown = false;
                if (! hold && ! pedal)
                {
                    removeAt (i);
                    changed = true;
                }
                return;
            }
    }

    void setPedal (bool down) noexcept
    {
        if (pedal == down) return;
        pedal = down;
        if (! pedal && ! hold) releaseLatched();
    }

    void allNotesOff() noexcept
    {
        numHeld = 0;
        pedal = false;
        changed = true;
    }

    int numNotes() const noexcept { return numHeld; }

    // Works out voiceNote. Only moves anything when the notes or the voice count changed.
    void assignChord (int voices) noexcept
    {
        voices = std::clamp (voices, 1, maxVoices);
        if (! changed && voices == lastVoices && lastAssign == assignChordType) return;
        changed = false;
        lastVoices = voices;
        lastAssign = assignChordType;

        std::array<int, maxVoices> chosen {};
        const int k = chooseNotes (voices, chosen);

        // Every way of giving k notes to the voices. At most 24 of them, and only on a change.
        std::array<int, maxVoices> best {}, trial {};
        best.fill (-1);
        int bestCost = 1 << 30;
        search (chosen, k, voices, 0, 0, 0, trial, best, bestCost);

        for (int v = 0; v < maxVoices; ++v)
        {
            voiceNote[(size_t) v] = best[(size_t) v];
            if (best[(size_t) v] >= 0)
                lastNote[(size_t) v] = best[(size_t) v];
        }
    }

    // Voice n takes the newest note held on MIDI channel n + 1.
    void assignChannels (int voices) noexcept
    {
        voices = std::clamp (voices, 1, maxVoices);
        lastAssign = assignChannelType;
        for (int v = 0; v < maxVoices; ++v)
        {
            int note = -1;
            long long newest = -1;
            if (v < voices)
                for (int i = 0; i < numHeld; ++i)
                    if (held[(size_t) i].channel == v + 1 && held[(size_t) i].order > newest)
                    {
                        newest = held[(size_t) i].order;
                        note = held[(size_t) i].note;
                    }
            voiceNote[(size_t) v] = note;
            if (note >= 0)
                lastNote[(size_t) v] = note;
        }
    }

    int noteFor (int voice) const noexcept { return voiceNote[(size_t) voice]; }

private:
    struct Held { int note = 0, channel = 0; long long order = 0; bool keyDown = false; };
    enum { assignChordType = 1, assignChannelType = 2 };

    bool anyKeyDown() const noexcept
    {
        for (int i = 0; i < numHeld; ++i)
            if (held[(size_t) i].keyDown) return true;
        return false;
    }

    int oldest() const noexcept
    {
        int at = 0;
        for (int i = 1; i < numHeld; ++i)
            if (held[(size_t) i].order < held[(size_t) at].order) at = i;
        return at;
    }

    void removeAt (int i) noexcept
    {
        for (int j = i; j + 1 < numHeld; ++j)
            held[(size_t) j] = held[(size_t) j + 1];
        --numHeld;
    }

    void dropReleased() noexcept
    {
        for (int i = numHeld; --i >= 0;)
            if (! held[(size_t) i].keyDown)
            {
                removeAt (i);
                changed = true;
            }
    }

    void releaseLatched() noexcept { dropReleased(); }

    int chooseNotes (int voices, std::array<int, maxVoices>& chosen) const noexcept
    {
        // Distinct notes, since the same note on two channels is one note here. Newest first.
        std::array<int, maxNotes> notes {};
        std::array<long long, maxNotes> orders {};
        int n = 0;
        for (int i = 0; i < numHeld; ++i)
        {
            int found = -1;
            for (int j = 0; j < n; ++j)
                if (notes[(size_t) j] == held[(size_t) i].note) found = j;
            if (found < 0) { notes[(size_t) n] = held[(size_t) i].note; orders[(size_t) n] = held[(size_t) i].order; ++n; }
            else orders[(size_t) found] = std::max (orders[(size_t) found], held[(size_t) i].order);
        }
        if (n == 0) return 0;

        std::array<bool, maxNotes> used {};
        int k = 0;
        const auto take = [&] (int index) { used[(size_t) index] = true; chosen[(size_t) k++] = notes[(size_t) index]; };
        if (n <= voices)
        {
            for (int i = 0; i < n; ++i) take (i);
        }
        else
        {
            if (voices >= 2)
            {
                int low = 0, high = 0;
                for (int i = 1; i < n; ++i)
                {
                    if (notes[(size_t) i] < notes[(size_t) low]) low = i;
                    if (notes[(size_t) i] > notes[(size_t) high]) high = i;
                }
                take (low);
                take (high);
            }
            while (k < voices)
            {
                int newest = -1;
                for (int i = 0; i < n; ++i)
                    if (! used[(size_t) i] && (newest < 0 || orders[(size_t) i] > orders[(size_t) newest])) newest = i;
                take (newest);
            }
        }
        std::sort (chosen.begin(), chosen.begin() + k);
        return k;
    }

    void search (const std::array<int, maxVoices>& chosen, int k, int voices, int index, int usedVoices, int cost,
                 std::array<int, maxVoices>& trial, std::array<int, maxVoices>& best, int& bestCost) const noexcept
    {
        if (cost >= bestCost) return;
        if (index == k)
        {
            bestCost = cost;
            best.fill (-1);
            for (int v = 0; v < voices; ++v)
                if ((usedVoices >> v) & 1)
                    best[(size_t) v] = trial[(size_t) v];
            return;
        }
        for (int v = 0; v < voices; ++v)
        {
            if ((usedVoices >> v) & 1) continue;
            const int was = lastNote[(size_t) v];
            // A voice that has never sung costs a fixed amount to wake. All equal on a fresh chord, which fills the voices from the first, lowest
            // note first. On a change it is more than a usual step, so voices already singing move rather than a new one taking over.
            const int move = was < 0 ? 12 : std::abs (chosen[(size_t) index] - was);
            trial[(size_t) v] = chosen[(size_t) index];
            search (chosen, k, voices, index + 1, usedVoices | (1 << v), cost + move, trial, best, bestCost);
        }
    }

    std::array<Held, maxNotes> held {};
    int numHeld = 0;
    long long order = 0;
    bool pedal = false, hold = false, changed = true;
    int lastVoices = -1, lastAssign = 0;
    std::array<int, maxVoices> voiceNote {}, lastNote {};
};
} // namespace smoothharmony
