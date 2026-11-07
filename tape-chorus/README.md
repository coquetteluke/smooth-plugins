# Smooth Tape Chorus

A tape style chorus. Your signal goes through a soft tape saturation, then 1 to 8 moving read heads pull separate copies off a delay line, with slow, fast and random tape speed wobble on top. Spread places the copies across the stereo field. Feedback can send them round again. Mix blends the result with the untouched dry signal.

The control set was checked against the public manual of the chorus in UVI Tape Suite. All code is written from scratch.

## How to load it

Audio Units > Luke Garito > Smooth Tape Chorus. AU code: `aufx STch Luke`. Works mono in to mono out, mono in to stereo out, and stereo in to stereo out. No licence system.

## Controls

| Control | Range | What it does |
|---|---|---|
| Voices | 1 to 8 | Number of separate moving copies. Copies fade in and out in about 30 ms, and level stays about the same. |
| Rate | 0.05 to 10 Hz | Speed of the sweep. |
| Depth | 0 to 8 ms | How far each copy's delay swings either way from its centre. More depth, more pitch movement. Pitch Swing shows the result in cents. |
| Delay Time | 3 to 40 ms | Centre delay. With several voices, the copies sit around it. |
| Feedback | 0 to 90 % | Sends the voices back onto the tape. Each pass falls by this amount. Level is held steady as it rises, and the loop cannot run away. |
| Wow | 0 to 100 % | Slow, irregular tape drift. Up to 2.5 ms swing. |
| Wow Rate | 0.1 to 4 Hz | Speed of Wow. 0.53 Hz is the 0.1.0 speed. |
| Flutter | 0 to 100 % | Fast, irregular tape shake. Up to 0.25 ms swing. |
| Flutter Rate | 3 to 30 Hz | Speed of Flutter. 7.1 Hz is the 0.1.0 speed. |
| Random | 0 to 100 % | Wandering delay with no pattern, separate for each voice. Up to 2 ms swing. Takes away the regular sound of a chorus. |
| Random Rate | 0.1 to 20 Hz | How fast Random wanders. The swing does not change with speed. |
| Unsync | off, on | Off: left and right wander together. On: each side wanders on its own, which widens the sound without Spread. |
| Saturation | 0 to 100 % | Soft tape clipping on the chorus voices only. A -12 dBFS signal stays at about the same level. The dry signal is never saturated. |
| Degrade | 0 to 100 % | Random level dips in each voice, like worn tape. Dips reach -20 dB at most, and only at 100. |
| Degrade Rate | 0.2 to 20 Hz | How fast the dips come and go. |
| Noise | 0 to 100 % | Tape hiss on the chorus voices. 100 is -40 dBFS, 50 is -52, 25 is -64. 0 adds nothing. |
| Noise Gate | off, on | On: hiss fades out when the input goes quiet, in under 0.5 s, and comes back when it plays. |
| Low Cut | Off, 20 to 1000 Hz | 12 dB per octave on the chorus voices, and inside the feedback loop. The dry signal keeps its low end. |
| High Cut | 1 to 20 kHz, Off | 12 dB per octave on the chorus voices, and inside the feedback loop, so repeats get darker. |
| Tone | -12 to +6 dB | Top end of the chorus voices. A gentle shelf from about 2.5 kHz up. 0 is flat. Outside the feedback loop. |
| Spread | 0 to 100 % | 0 keeps left and right identical. Higher moves the two sides apart in time and sweep, and places the copies left and right. |
| Mix | 0 to 100 % | Equal power blend of dry and chorus. 0 gives back the input exactly. |
| Input Gain | -24 to +24 dB | Before everything, dry included. Not made up afterwards, so it hits Saturation harder and comes out louder. |
| Output Gain | -24 to +24 dB | After everything. |
| Bypass | on or off | Fades to the exact dry signal in 20 ms, ignoring both gains. Latency stays 0. |

Readings, which cannot be set and write no automation: **Input Level** (after Input Gain), **Output Level**, **Pitch Swing** (largest pitch movement from Rate and Depth, in cents).

If Delay Time is too short to hold the modulation you asked for, Depth, Wow, Flutter, Random and the voice offsets all shrink together, so the delay never falls below about 1.3 ms.

## The window

Two reels with one line per voice running between them. Each line is that voice's read head, read from the engine about 250 times a second: it sweeps with Rate and Depth, shakes with Wow, Flutter and Random, breaks where Degrade pulls a voice down, and the pale left lines and aqua right lines part as Spread opens. The bars behind are the loudest samples really written to the tape, so Feedback shows as repeats after a phrase. The lines gather toward each reel as they reach it, and the middle is true to the scale shown. The left reel winds out with the input level, the right with the output, and a coral ring means over 0 dB.

The row under it follows the signal from left to right: Input, Voices, Rate, Depth, Delay, Feedback, Spread, Mix, Output. Advanced opens the rest on the same nine columns. Drag any column, double-click to reset, click a number to type. Drag the tape up or down for Depth and sideways for Rate, and drag a reel for its gain. A line at the bottom says what the thing under the mouse does.

With no audio running at all, the window draws what the settings will do. When the tape has been silent for 3 seconds and nothing is changing, it stops drawing. Everything in it is a real parameter with its old ID, name and range, so automation moves the same controls. A session gains only the window size and whether Advanced is open. The type is Work Sans, under the SIL Open Font License, in `assets/fonts/`.

Pictures are in `docs/window/`. To make them again: `build/RenderWindow_artefacts/Release/RenderWindow docs/window`. To measure the window: `RenderWindow --live [runs] [seconds] [normal, silent or background] [preset]`.

## Presets

Same as 0.1.0, sample for sample:

- **Default Tape Chorus**: 2 voices, a general starting point.
- **Soft Vocal Double**: 2 voices, slow and subtle.
- **Wide Four Voices**: 4 voices, full Spread.
- **Worn Cassette**: heavy Wow, Flutter and Saturation with a dark top end.
- **Slow Drift**: 3 voices, a long slow sweep.
- **Fast Shimmer**: quick, shallow and a little bright.
- **Mono Thickener**: 3 voices, no Spread, stays in the centre.

New in 0.2.0:

- **Eight Voice Stack**: 8 voices, full Spread, low end kept out of the chorus. For a stack of backing vocals.
- **Clean Reel Lift**: 4 voices, almost no tape colour, Low Cut at 300 Hz. Width for a lead with no mud.
- **Cassette Backing**: 6 voices, cassette speed wobble, hiss, dropouts, top end cut at 7 kHz, a little Feedback.
- **Lo-Fi Tape Double**: 2 voices, heavy wear, narrow band, hiss.
- **Random Drift Double**: almost no regular sweep, mostly Random and Unsync, so it sounds like a second take.
- **Resonant Tape Swirl**: 3 voices, short delay, 55 % Feedback, darkening repeats.

## Signal flow

Input > Input Gain > dry path straight to Mix.

Input > Input Gain > pre-emphasis > oversampled soft clip (4x, or 2x from 60 kHz up) > de-emphasis > DC block > delay line, with hiss and feedback added here > up to 8 voices, each with a left and right read head (6 point interpolation) and its own Degrade dips > sum > Low Cut > High Cut > Tone > Mix > Output Gain > output.

Feedback leaves after High Cut, is scaled by the sum of the voice gains so that its gain can never pass 1 even if every copy lines up, rolls off below 40 Hz and above 12 kHz, passes a soft limiter that starts at 0.9, and returns to the delay line. The repeats do not go through Saturation again, because the clipper runs on oversampled blocks and that would make the output depend on block size.

Latency is 0 samples at every rate. The oversampling filters take 59.5 samples at 48 kHz, and that amount is taken off the read head, so the delay you set is the delay you get and the dry path needs no delay. The feedback path is held back by the same amount, so one pass round the loop takes the Delay Time.

## What was measured

CPU through the real AU at 48 kHz with 128 frame blocks, percent of one core: default 0.8, worst preset 1.1 (Eight Voice Stack), worst single block 3.

Same sound after the CPU work: `tests/null_results.txt` holds the null of 0.2.1 against 0.2.0 for the default, every preset and random automation. Worst peak -114.4 dBFS, on a signal at 16 times full scale.

Window CPU, on the real window at Retina size with speech running, 30 frames a second, percent of one core, with other builds running on the Mac: default 2.5 to 4.1 over 5 runs, Eight Voice Stack 3.8 to 4.8 over 5 runs. That is 3.1 and 4.0 percent of a fast core in work. Stopped with nothing moving: 0.7. On the slow cores, when the Mac was busy with several builds, the same work read 6.9 to 11 percent. `docs/window/cpu-runs.txt` has the runs. Through `NullTest --cost` the audio thread takes 0.5 percent more instructions than 0.2.1 (96.1 to 96.6 million a second at default), to fill the window's picture.

Same sound after the window: `tests/au_null.sh` renders speech through the real installed AU at 19 settings, and `tests/au_baseline.sha256` holds the checksums from installed 0.2.1. All 19 are bit for bit identical. `tests/NullTest.cpp` also checks 6 settings against 0.2.1 with no window, with the window drawing while audio runs on another thread, and with the window opening and closing over and over during audio. All exact. `tests/au_null_results.txt` and `tests/window_results.txt` have those results, and the window's own checks: every parameter has a place on screen, automation moves the controls, drags have one begin and one end, and the saved state holds only the two new values.

Offline, in `tests/PluginTest.cpp`. Full numbers are in `tests/results.txt`.

- Same as before: all 7 first presets null against a frozen copy of the 0.1.0 engine (`tests/EngineV010.h`), difference -240 dB, at 44.1, 48 and 96 kHz. A 0.1.0 session opens with its own settings and every new control neutral.
- Voices: one pulse in gives 1 to 8 copies out. Same with modulation on, and each copy travels at least 1 ms at Depth 0.8 ms. Noise level stays within 1.5 dB from 1 to 8 voices.
- Feedback: repeats arrive 30 ms apart at Delay 30, within 0.05 ms, at all three rates. Each pass at 1 kHz falls by what you set, within 1 dB at 30, 60 and 90. White noise level stays within 1.9 dB of Feedback 0, darker noise within 0.5 dB. Full scale noise and sine at Feedback 90 peak at no more than 1.81 with 1 voice and 5.94 with 8. The reported tail covers the real one.
- Input Gain and Output Gain: +6 and -12 measure 6.000 and -12.000 dB. Input Gain +12 takes THD from 2.1 to 8.8 % at Saturation 50, Output Gain +12 leaves THD alone.
- Low Cut 200: -3.01 dB at 200 Hz, -12.3 dB at 100 Hz, 5 kHz unmoved. High Cut 5 kHz: -3.01 dB at 5 kHz, -12.8 to -14.8 dB at 10 kHz depending on rate, 200 Hz unmoved.
- Wow Rate and Flutter Rate: over 85 percent of the movement lands in the band each setting calls for. `tests/results.txt` has the shares.
- Random: 100 swings 2.0 ms, 100 against 50 moves 2.0 times as far. Random Rate 5 Hz crosses centre 8.5 times as often as 0.5 Hz, with movement within 2 percent. Unsync: left and right delay correlation over 0.999 off, under 0.5 on.
- Degrade: level movement 0, 0.37, 0.82 and 2.40 dB rms at 0, 25, 50 and 100. Deepest dip -20 dB. Rate 10 Hz crosses the average 6.4 times as often as 1 Hz.
- Noise: -64.9, -52.8 and -40.8 dBFS at 25, 50 and 100 with heads still, -40.1 with heads moving. Noise Gate holds a silent input at nothing, and hiss is at -97 dBFS from 0.5 s after the voice stops.
- Readings: Input Level and Output Level within 0.05 dB on a steady sine, including with both gains. Pitch Swing reads 21.62 cents where the sine shows 21.87. No host notifications from audio.
- Voice changes: sharpest bend in the signal while switching through 1 to 8 is under 2 times steady playback.
- Rate and Depth, tracked from the instantaneous phase of a sine: 0.5, 1, 2 and 5 Hz measure on target, and swing is within 0.5 percent, at 44.1, 48 and 96 kHz.
- Delay Time: pulse arrival within 0.02 ms at 5, 15 and 30 ms, at all three rates. The average of 3 and 4 voices is also on target.
- Wow: 99.9 percent of its movement is below 3 Hz. Flutter: 99.9 percent is above 4 Hz.
- Saturation: THD at 1 kHz, -12 dBFS rises with the control, with 0 under 0.01 percent. `tests/results.txt` has each step.
- Fold back with a 5 kHz sine at Saturation 100 is below -96 dB from 2 to 22 kHz. One line at 23 kHz, above hearing, sits at -53.8 dB. It is the 25 kHz harmonic inside the output filter's transition band.
- Tone: -12 gives about -10.6 dB at 12 kHz, +6 gives about +5.9 dB, and 200 Hz moves under 0.1 dB.
- Spread: correlation of left and right falls steadily from 1.0 at Spread 0 to under 0.3 at Spread 100, with under 0.3 at every voice count from 3 to 8. This uses noise darkened below about 400 Hz, because white noise reads near 0 from Spread 25 up.
- Mix: Mix 0 and Bypass match the input exactly. Mix 50 gives dry and wet each at -3.01 dB, with the dry on time.
- Mono to stereo: Spread 0 keeps both sides identical, Spread 100 wet correlation is near 0, and left and right match within 0.1 dB. A hard left stereo source stays on the left.
- Output is identical at block sizes 512, 37 and 1, and after preparing again, at default settings and with everything on. With everything on there are also no allocations, no denormals, and it recovers from NaN and infinite input.
- Damaged saved state and NaN, infinite or huge input cannot stick in the audio path.

These are synthetic test signals. They do not prove it sounds good on a real voice. That still needs listening.
