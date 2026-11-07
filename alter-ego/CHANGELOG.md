# Changelog

## 1.1.1, 2026-11-07

Renamed to Alter Ego, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

Custom window, called Alter Ego inside. A large Pitch and Formant pad where the dot is the voice, Character chips, Vibrato and Whisper, and a live mirror: your voice and what comes out, back to back.
No DSP change. Adds a read of the voice going in and coming out, written only while a window is open. Output through the installed AU is bit identical to 1.0.0 at 8 settings (`tests/au_null.sh`), and audio with the window open and painting nulls to exact zero against it closed. Adds `tests/RenderWindow.cpp`, `tests/AuRender.cpp`, pictures in `docs/window/` and window tests in `tests/PluginTest.cpp`. The plug-in's build name stays Smooth Morph until the rename.

## 1.0.0

First version. Real time voice changer: Pitch, Formant Shift, Character (Natural, Deep, Child, Robot, Monster) with Character Amount and Robot Note, Vibrato Rate and Depth, Whisper, Range Match with Range Centre, Mix, Output, Bypass.
Readings: Detected Pitch, Output Pitch, Range Offset. Fourteen factory presets. Latency about 24 ms at every rate, dry path matched.
