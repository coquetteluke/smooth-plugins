# Changelog

## 1.2.0, 2026-11-11

From the real vocal re-test on the Rode takes (17 takes of Luke singing and rapping). The default and Clean Slate are untouched: still the delayed input, to -142 dB. Every other setting sounds as it did except at the faults below.

- Fixed: a burst up to 20 dB over the voice for about 30 ms where a voiced sound starts, at any setting that changes the voice (take 01 at 105.67 s: -12.5 dBFS where the voice is at -32.9). At each start the pitch handed to the envelope was gliding in from the last note's pitch, so for a frame or two the envelope was fitted to harmonics that were not there. A voiced stretch now starts on its own pitch, and a jump of more than a semitone is taken at once.
- Fixed: notes 12 to 15 dB too loud for 40 ms, then too quiet, mostly at Pitch +7 (take 01 at 42.28 s, 25 such bursts over four takes). On a soft note with two strong harmonics the fit drew a formant peak between them 21 dB over both, and the moved fundamental landed on it. The fit may now stand no more than 6 dB over the harmonics either side of it.
- Fixed: a one frame burst when a held low note starts to move (take 10 at 15.43 s, 17 to 19 dB over). The gain that goes with each envelope frame jumped to 3 to 6 times for one frame. It may now stand no more than 6 dB over its own average of the last 10 ms.
- New safety net behind those three: the changed voice, over 5 ms, may be no more than 6 dB over the loudest 5 ms of the input within 20 ms either side. Under that it does nothing at all. Over four takes and 14 settings the 20 ms frames that came out 10 dB or more over the voice fell from 149 to 50, and outside Robot and Whisper 100 from 67 to 9.
- New: Peak Guard, in Advanced, off by default, and no preset touches it. Moving pitch down makes fewer, stronger pulses, so peaks come out higher than they went in at the same level (Giant: +7.1 dBFS on a take that peaks at -0.1). On, the output never peaks higher than the input has, times Output. Off it adds nothing.
- Tests: the first hiss test is back as it was (each voice on its own, 3 dB). It read 4.3 dB on 1.1.1 behind a pooled reading of 2.0, and reads 2.0 now because the plug-in changed. New tests for the soft note burst and for Peak Guard.

## 1.1.1, 2026-11-07

Renamed to Alter Ego, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

Custom window, called Alter Ego inside. A large Pitch and Formant pad where the dot is the voice, Character chips, Vibrato and Whisper, and a live mirror: your voice and what comes out, back to back.
No DSP change. Adds a read of the voice going in and coming out, written only while a window is open. Output through the installed AU is bit identical to 1.0.0 at 8 settings (`tests/au_null.sh`), and audio with the window open and painting nulls to exact zero against it closed. Adds `tests/RenderWindow.cpp`, `tests/AuRender.cpp`, pictures in `docs/window/` and window tests in `tests/PluginTest.cpp`. The plug-in's build name stays Smooth Morph until the rename.

## 1.0.0

First version. Real time voice changer: Pitch, Formant Shift, Character (Natural, Deep, Child, Robot, Monster) with Character Amount and Robot Note, Vibrato Rate and Depth, Whisper, Range Match with Range Centre, Mix, Output, Bypass.
Readings: Detected Pitch, Output Pitch, Range Offset. Fourteen factory presets. Latency about 24 ms at every rate, dry path matched.
