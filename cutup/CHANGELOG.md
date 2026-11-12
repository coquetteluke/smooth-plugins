# Changelog

## 1.2.0, 2026-11-11

Transient cuts start where the sound starts, on real sung takes. Measured on Luke's Rode takes (11 songs, 61 phrases of 20 s, 1883 cuts at the default settings), 1.1.1 cut 363 slices late, after the S, F, H or breath that leads into the vowel (worst 156 ms), put 2 cuts in the middle of an S, and 5 where nothing rises. The slicer now holds every cut against its own slice: a cut that leaves a weaker lead-in off moves back to where the lead-in comes up, a cut inside an S moves to where the S starts, a cut on dead air moves up to the sound, and a cut where nothing rises is left out. In a real room "quiet" is the room, not zero. Saved sessions keep the cuts they were saved with; only a newly loaded sample is cut the new way. A new test in the suite, sung phrases in a room, judges every cut the way the real vocal check does, and failed on 1.1.1 (86 late, 2 on dead air, 26 inside an S, 5 without a rise).

## 1.1.1, 2026-11-07

Renamed to Cutup, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

The custom window, called Cutup. The waveform runs edge to edge, cut into warm coloured slices, and each slice flows down a ribbon to its key in the same colour. What a voice has played so far lights up as it plays. Amp and Pitch glide graphs with dots to drag, a Start tab on the waveform, a Root tab on the keyboard, Zoom slice, and a slot stepper that takes every per slice control to any of the 32 slots. The sound is bit identical to 1.0.0 on all 14 presets. The window adds only its size and whether Advanced is open to the saved state.

## 1.0.0

First version. Loads a vocal sample on a background thread and keeps the audio in the saved state. Slices by transients, by equal parts, by note length or by hand. 32 slots, each with Pitch, Reverse, Attack, Release, Level and Pan as real parameters. Gate, One Shot, Loop, To End and Boomerang play modes. Slices or Chromatic key modes. Polyphony to 16 with click free stealing. Waveform window with markers, click to audition, Load and drag and drop. 14 factory presets that set behaviours, not samples.
