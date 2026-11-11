# Changelog

## 1.2.1, 2026-11-11

Second pass of the real vocal re-test. The default and Clean Slate are untouched (still the delayed input, -142 dB).

- Fixed: switching Range Match off took 1.4 s to bring the voice back to its own pitch (half a second after switching off, the voice was still a third of a semitone away: -3.6 dB against the dry on a real take, limit -40). Switching it on or off, or a Robot taking over, now glides at Pitch's speed; the slow glide stays for the semitone steps it takes as a voice drifts. On the take it reads -150 dB.
- Fixed: the output depended on how loud the voice was recorded. Three fixed thresholds did it: the tracker's gate at -60 dBFS (quiet ends of notes passed unshifted on a quiet take and shifted on a loud one), the envelope's silence test at -110 dBFS and the level match's guard against 0 / 0. Pitch +7 on a real take with the input times 4, scaled back, read -2.9 dB against the plain render; it reads -195 dB, and -201 dB for times a quarter. The gate now sits 40 dB under the loudest the voice has been lately. Renders at the take's own level differ from 1.2.0 about as much as 1.2.0 differed from itself with the input 12 dB louder (7% of frames by over 1 dB).
- Changed: Whisper's breath and Monster's shake are now noise that belongs to the moment of the input, so they are the same whenever the control was moved, and a setting reached by moving a control ends up as exactly the sound of that setting from the start. Before, two renders of Whisper 100 differed by up to 3.5 dB in a 10 ms frame with nothing moved. Same kind of noise, a different draw of it.
- Tests: Range Match switched off (-107 dB, limit -40), input times 4 and times a quarter (-166 dB, limit -60), Whisper switched on later against Whisper from the start (-331 dB, limit -80). Each failed on 1.2.0 first (3.1 dB, -27.4 dB, 3.0 dB).

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
