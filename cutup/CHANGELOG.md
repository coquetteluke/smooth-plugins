# Changelog

## 1.3.0, 2026-11-17

- Switchboard can now see and set every control through the link. Sound unchanged. `src/SmoothLink.h` is a byte copy of Switchboard's: the plug-in keeps a card of every control in the shared link folder and moves its own parameter, with a gesture as a mouse drag does, when Switchboard sends a set; the card carries the track name Logic gives it. Main controls: Slice Mode, Sensitivity, Slice Start, Selected Slice, Play Mode, Output. New test `PluginTest link` (also in the full run): the card lists all 218 parameters by ID, "75 %" on Sensitivity lands and is confirmed, closing removes the card. No DSP, parameter, ID, range, default, preset or AU code changed; the render sums of the default and all 14 presets, from the same saved sample, equal 1.2.2's.

## 1.2.2, 2026-11-13

- Slice Mode set to Manual by a host or automation now gets the slices the processor holds (the default transient slices until a marker is moved), the same with the window open as closed. 1.2.1 gave the slices on show when the window was open, so an open window changed what a host set. Clicking Manual in the window still keeps the markers on show. New test in `tests/Phase2Tests.h`: Manual set through the parameter with the window open and closed must give the same slices and save the same bytes; on 1.2.1 8 slice starts differed.
- Changed test: the 1.0.0 test "picking Manual keeps the markers on show" picked Manual through the parameter, the way a host sets it, so it required the old behaviour. It now clicks Manual in the window, as a person picks it, with the same check plus that the click turned Slice Mode to Manual; the old lines are kept in a comment above it. Changed with Luke's yes on 2026-11-13: "u can do all of these".
- Nothing else changed: every slice is cut where 1.2.1 cuts it, the 17 sessions saved by 1.2.0 play bit for bit and save back byte for byte, and all 14 presets render the same.

## 1.2.1, 2026-11-13

Fixes for the bugs the Phase 2 hunt kept, each with a test in `tests/Phase2Tests.h` that failed on 1.2.0. Every slice is cut where 1.2.0 cuts it and, up to 8 times the file's speed, every voice plays exactly as in 1.2.0. 17 sessions saved by the installed 1.2.0 (the default, all 14 presets, Manual and Grid) play bit for bit, keep their sample, cuts and every slot, and save back byte for byte.

- Sound: pitching faster than 8 times the file's speed (above +36 st, or less on a file at a higher rate than the session) no longer folds back. The resampler has bands up to 64 times, with longer kernels past 8 times; a tone pitched above Nyquist now comes out more than 100 dB down instead of 2 dB. 16 voices looping at +40 st take 23% of a core instead of 10%.
- A save while Slice Start, Sensitivity or another slicing control is automated no longer brings the session back in Manual: the saved slices are cut by the values saved with them.
- A session restored with the window open keeps its Manual markers.
- The host's Reset stops every voice and puts the wheel and pedal back, except the Reset JUCE sends as the host's bypass turns on, so the sound still fades. Reset All Controllers puts the wheel and pedal back. Bend Range now applies to a wheel already held.
- Root Note reads flat names (Eb3, Bb2) and numbers like 51.0. In the window, text that is no note is refused; from a host it reads as C3, as before, and empty text no longer reads as C-2.
- A sample that finishes loading in the middle of a marker edit keeps its own markers, so its session no longer loads empty. A load cancelled by a restored session no longer leaves Loading on show.
- Window: Zoom slice holds still while a marker or the Start tab is dragged; the keys settle after a Root drag; the Advanced drawer follows a restored session; a typed slot value goes to the slice it was typed for; Play Mode, Grid and Grid Note wait out a double-click; in Chromatic the stepper names Root and a clicked key plays at its own pitch and lights; Grid Tempo, slot values and the Advanced summary fit their room; the time scale steps up to 10 minutes; Sample Length reads up to 2400 s.
- The chosen preset is saved with the session. A session saved before 1.2.1, or where no preset was chosen, shows Choose a preset; its arrows go to the first or last.
- A session whose saved sample was damaged names the sample in the window. A sample file moved or deleted since never mattered: the audio is in the session.
- Not fixed: with sound before the first onset taking a slot of its own, the latest of the 32 strongest onsets still goes rather than the weakest. Keeping the strongest changed where Luke's long takes are cut, and Vowel Hold then clicked in two places on the real vocal check (takes 10 and 14) where 1.2.0 is clean.
- Not fixed: Slice Mode set to Manual by a host while the window is open still starts from the slices on show, because the 1.0.0 test "picking Manual keeps the markers on show" expects exactly that. The README now says so.

## 1.2.0, 2026-11-11

Transient cuts start where the sound starts, on real sung takes. Measured on Luke's Rode takes (11 songs, 61 phrases of 20 s, 1883 cuts at the default settings), 1.1.1 cut 363 slices late, after the S, F, H or breath that leads into the vowel (worst 156 ms), put 2 cuts in the middle of an S, and 5 where nothing rises. The slicer now holds every cut against its own slice: a cut that leaves a weaker lead-in off moves back to where the lead-in comes up, a cut inside an S moves to where the S starts, a cut on dead air moves up to the sound, and a cut where nothing rises is left out. In a real room "quiet" is the room, not zero. Saved sessions keep the cuts they were saved with; only a newly loaded sample is cut the new way. A new test in the suite, sung phrases in a room, judges every cut the way the real vocal check does, and failed on 1.1.1 (86 late, 2 on dead air, 26 inside an S, 5 without a rise).

## 1.1.1, 2026-11-07

Renamed to Cutup, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

The custom window, called Cutup. The waveform runs edge to edge, cut into warm coloured slices, and each slice flows down a ribbon to its key in the same colour. What a voice has played so far lights up as it plays. Amp and Pitch glide graphs with dots to drag, a Start tab on the waveform, a Root tab on the keyboard, Zoom slice, and a slot stepper that takes every per slice control to any of the 32 slots. The sound is bit identical to 1.0.0 on all 14 presets. The window adds only its size and whether Advanced is open to the saved state.

## 1.0.0

First version. Loads a vocal sample on a background thread and keeps the audio in the saved state. Slices by transients, by equal parts, by note length or by hand. 32 slots, each with Pitch, Reverse, Attack, Release, Level and Pan as real parameters. Gate, One Shot, Loop, To End and Boomerang play modes. Slices or Chromatic key modes. Polyphony to 16 with click free stealing. Waveform window with markers, click to audition, Load and drag and drop. 14 factory presets that set behaviours, not samples.
