# Changelog

## 0.5.0, 2026-11-11

Steady Notes now follows the note, Punch keeps a held note still, and there are six presets for a lead vocal. Off, nothing changes.

Why: 0.4.0's Steady Notes kept held vowels still but could not meet all of its own limits on Luke's real takes. Breaths, mouth sounds and the gain after a phrase ends pulled against each other, because it read level only, and by level alone the end of a word, the breath after it and a sag inside a held note look the same.

What Steady Notes does now (all of it only while the switch is on):
- It hears pitch, the way a pitch tracker does. A note is a sound that has kept a pitch for 80 ms. While it has its pitch it may sag 6 dB before Sustain acts.
- The tail is what comes right after the pitch stops. A boost stays on it for about 70 ms and is gone by 105 ms. A cut stays for 110 ms and is gone by 130 ms. The breath, the mouth sound and the next phrase after that are left alone. Only a boost spares an S.
- Punch reads a held note by its level instead of every peak, and its level match is learned on the voice and then held, so a held note keeps one gain. The room and far-off breaths are no longer lifted with the voice.
- Attack no longer takes a jump of the peaks inside a held note for a start.

On the 11 working set takes with Steady Notes on, every limit is met: after a phrase ends +5.6 to +7.8 and -6.3 to -9.0 dB of 9, held vowels 0.00 dB, breaths at most 1.3 dB against the voice at Sustain +6 (limit 3), mouth sounds and S not raised, Punch 50 and 100 add at most 0.56 and 0.64 dB of swing to held vowels (limit 1.5; 1.68 and 4.10 without).

New presets, added at the end of the list so every older preset keeps its number and its sound: Lead Gentle, Lead Upfront, Lead Rap, Lead Soft Consonants, Lead Long Tails, Lead Dry And Close. All have Steady Notes on, and each meets every limit on every working set take, sung and rap. The README says which of the 14 older presets suit a voice.

Off stays off: with Steady Notes off the detector is 0.4.0's sample for sample (a new test runs both side by side at all 14 older presets), and the real plug-in nulls against the installed 0.4.0 on every working set take at the default and three older presets. The default is still Neutral. Latency, AU codes and every parameter are unchanged; no parameter was added.

The window: the preset box is wider, so the longest name fits with ", edited" after it. The Steady switch's hint says what it does now.

Tests: 410 pass, 0 fail (0.4.0 had 347). New ones use a made-up sung line with a held note, what is left after it, a breath, a mouth sound, a quieter second note, two syllables on one breath, a breath straight after a note and a humming room.

## 0.4.0, 2026-11-11

Steady Notes: a new control, off by default, that makes Sustain fit a voice. Found on Luke's real takes (Rode session, 17 songs): Sustain moved held vowels in and out by its full amount, because every small sag of a held note read as a tail; it gave a quiet phrase start the tail gain of the louder phrase before it; and it lifted every S. With Steady Notes on, a sound that has kept its level for 60 ms may sag by 4 dB before Sustain acts, a sharp start counts as a new sound, and bright sounds (an S) are left alone. The tail of a note still gets the full amount.
Off, nothing changes: every preset and saved session sounds exactly as before, held to the numbers recorded from 0.1.0. No preset turns it on yet: which presets should is Luke's call after listening. The control is a real AU parameter (Steady Notes) with a switch at the end of Sustain's bar in the window. Attack, Punch, Impact, the Ceiling, the default and latency are unchanged.
New tests: a held note swinging 3 dB at 5 Hz moves by 7.3 dB under Sustain 9 without Steady Notes and by 0.0 dB with it; a quieter note after a louder one; a hiss after a note; the control's default, save and recall, and that every preset leaves it off.

## 0.3.1, 2026-11-07

Renamed to Punch, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 0.3.0, 2026-11-07

The custom window replaces the generic one. Black and hot orange. The last hit drawn large, before and after, on a time scale that opens up its first moments, with the last few seconds as bars above it. Attack sits under the start of the hit and Sustain under its tail. In and Out meters share the plot's level scale, and Impact and Ceiling reductions hang beside them with large numbers. The Ceiling can also be dragged as a line on the plot. Every control is the same AU parameter under the same ID. The only new saved items are the window size and whether Advanced is open, kept in range when loaded.
No DSP change. Adds a lock-free read of the level before and after for the display. Output is bit identical to 0.2.0 through the installed AU, on real speech under four settings. Adds `tests/RenderWindow.cpp`, `tests/AuRender.cpp`, `tests/null/`, pictures in `docs/window/` and window tests in `tests/PluginTest.cpp`.

## 0.2.0

Second pass, checked against what the plug-in this replaces offers. Adds Gain, Impact (level-riding compression that also raises the level), Ceiling and Ceiling On, plus read-only Input Level, Output Level, Impact Reduction and Ceiling Reduction. Seven new presets. Latency stays 2 ms. Every first version control, preset and saved session sounds the same, measured against numbers recorded from 0.1.0.

## 0.1.0

First version. Transient shaper and fast compressor with Punch, Attack, Sustain, Mix and Bypass. 2 ms latency, dry path delay-matched. Seven presets. Damaged saved state and bad input samples are cleaned before they reach the audio.
