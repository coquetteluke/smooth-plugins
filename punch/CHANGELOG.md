# Changelog

## 0.3.0, 2026-11-07

The custom window replaces the generic one. Black and hot orange. The last hit drawn large, before and after, on a time scale that opens up its first moments, with the last few seconds as bars above it. Attack sits under the start of the hit and Sustain under its tail. In and Out meters share the plot's level scale, and Impact and Ceiling reductions hang beside them with large numbers. The Ceiling can also be dragged as a line on the plot. Every control is the same AU parameter under the same ID. The only new saved items are the window size and whether Advanced is open, kept in range when loaded.
No DSP change. Adds a lock-free read of the level before and after for the display. Output is bit identical to 0.2.0 through the installed AU, on real speech under four settings. Adds `tests/RenderWindow.cpp`, `tests/AuRender.cpp`, `tests/null/`, pictures in `docs/window/` and window tests in `tests/PluginTest.cpp`.

## 0.2.0

Second pass, checked against what the plug-in this replaces offers. Adds Gain, Impact (level-riding compression that also raises the level), Ceiling and Ceiling On, plus read-only Input Level, Output Level, Impact Reduction and Ceiling Reduction. Seven new presets. Latency stays 2 ms. Every first version control, preset and saved session sounds the same, measured against numbers recorded from 0.1.0.

## 0.1.0

First version. Transient shaper and fast compressor with Punch, Attack, Sustain, Mix and Bypass. 2 ms latency, dry path delay-matched. Seven presets. Damaged saved state and bad input samples are cleaned before they reach the audio.
