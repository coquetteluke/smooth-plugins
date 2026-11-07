# Changelog

## 1.1.1, 2026-11-07

Renamed to Tune, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0, 2026-11-07

The custom window replaces the generic one. Graphite and lime: a huge note, a correction ruler in cents, a short trace on the same scale, In and Out levels, one cell per note, the main controls in view and the rest under Advanced. Every control is the same AU parameter under the same ID. The only new saved items are the window size and whether Advanced is open, kept in range when loaded.
No DSP change. Adds a read of the input and output peak for the meters. Output is bit identical to 1.0.1 through the installed AU. Adds `tests/RenderWindow.cpp`, `tests/AuRender.cpp`, pictures in `docs/window/` and window tests in `tests/PluginTest.cpp`.

## 1.0.1, 2026-11-07

Slowed the tracker's mark pull from 1% to 0.2%, which was bending shifted pitch. Worst output pitch error at instant speed: steady notes 3.44 to 1.01 cents, vibrato with breath 4.36 to 1.52 cents. Worst formant move 4.98% to 4.43%.

## 1.0.0

First version. Real time vocal pitch correction with a time domain tracker, pitch-synchronous grain shifting and formant preservation.
Controls: Key, Scale, Retune Speed, Humanize, Flex Amount, Input Type, Follow Key, Natural Vibrato, Transpose, Reference A4, Tracking, Formant Preserve, Throat Length, Custom notes (Tune, Remove, Bypass), Bypass.
Readings: Pitch Off Target, Detected Pitch, Correction Applied, Target Note, Active Key. Eight factory presets. Generic window plus a live reading strip.
