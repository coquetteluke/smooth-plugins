# Changelog

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
