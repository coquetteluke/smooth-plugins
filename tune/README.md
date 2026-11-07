# Tune

Low latency, real time vocal pitch correction that keeps formants where they are. From hard, stepped tuning to correction you do not notice.

**Load it:** Audio Units > Luke Garito > Tune. AU code `aufx STun Luke`. Mono and stereo. No MIDI.

Synthetic voices and macOS `say` are what it has been measured on. That does not prove it sounds good on a real singer. It has not been compared with Logic's stock pitch correction, which cannot run offline here.

## Controls

Every control is a real AU parameter that a host can find by name.

| Control | What it does |
|---|---|
| Key | Root of the scale. |
| Scale | Chromatic, Major, Minor, Harmonic Minor, Melodic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian, Custom. |
| Retune Speed | Time constant of the correction, 0 to 400 ms. 0 reads Instant and is truly instant: the output sits on the note. |
| Humanize | Slows correction on the sustained part of a note, up to 400 ms extra, while the start of the note stays quick. Vibrato survives on long notes. |
| Flex Amount | How far a note may drift before correction applies, 0 to 100 cents. Inside that, the note is left exactly as sung. Outside, it is brought to the edge. 50 or more does little in Chromatic, because no note is ever more than 50 cents from a chromatic target. |
| Input Type | Voice range. Sets tracking range and latency. Soprano 150 Hz to 1.3 kHz, Alto/Tenor 98 Hz to 1 kHz, Low Male 65 to 600 Hz, Wide Range 65 Hz to 1.3 kHz. |
| Follow Key | Takes key and scale from `~/Library/Application Support/SmoothPlugins/key.json`, written by Smooth Key. Missing, broken, stale (over 10 minutes old, since Smooth Key only writes on a change), future dated or low confidence (under 0.5) data is ignored: the last good key holds, and with none it uses Key and Scale. A Custom scale stays as it is. |
| Natural Vibrato | Scales the vibrato that is left, -100 removes it, +100 doubles it. |
| Transpose | -24 to +24 semitones, after correction. Formants stay. |
| Reference A4 | 415 to 466 Hz. |
| Tracking | Which sounds count as pitched. 0 is choosy and passes rough, breathy sound untouched. 100 tunes it. |
| Formant Preserve | On keeps the formants. Off lets them follow the pitch. |
| Throat Length | 60 to 150%. Above 100 lowers the formants, as a longer throat would. Only voiced sound is changed. |
| Custom C to Custom B | With Scale on Custom: Tune, Remove (never a target) or Bypass (a target, but left uncorrected). |
| Bypass | Keeps the same delay. |

Readings, which never write automation: Pitch Off Target (cents, the meter), Detected Pitch, Correction Applied, Target Note, Active Key. They read through the host by name and value string, and the window shows all of them live.

## The window

Graphite and one colour, lime, which only ever means the corrected voice or the amount of correction. Pictures are in `docs/window/`, rendered from the real window with real `say` material going through the real processor: `build/RenderWindow_artefacts/Release/RenderWindow`.

- **The note** being hit, huge. It turns lime when what comes out is within 8 cents of the note. Sung pitch on the left, correction on the right.
- **The ruler** is the correction meter, in cents away from that note. The white mark is what was sung, the lime mark is what comes out, and the ticks lit between them are the correction. It gives more room to small amounts, so 25, 50 and 100 cents land on the longer ticks. Transpose is left out of it, and shown in words as "out as".
- **The trace** is the last few seconds on the same scale, with the note as the middle line. Hard tuning is a flat lime line with white wandering around it. Soft tuning has the lime line following the white one. The shaded band is Flex Amount. Each note gets its name once it has lasted long enough to read.
- **In and Out** are the two levels at the sides.
- **Notes**: one cell per note. Click to tune to it or remove it, right-click to bypass it. From any scale but Custom, the first click turns the scale showing into a Custom one, with only that note changed. The note being hit lights up.
- **Advanced** holds Transpose, Reference A4, Tracking, Throat Length and Formant Preserve. When closed, it says which of them have moved.

Double-click anything to reset it, click a value to type it. Drag the corner to resize. The size and whether Advanced is open are saved with the session, and are not parameters. All 26 controls are the same AU parameters as before, under the same IDs.

## Presets

Natural Pop, Transparent Ballad, Light Touch, Keep The Vibrato, Tight Modern, Hard Tune, Hard Tune Glide, Robot Flat. Presets only set Retune Speed, Humanize, Flex Amount and Natural Vibrato, so they never fight your Key, Scale, Input Type or Follow Key.

## Signal flow

Input > pitch tracker (on the left and right mixed) > corrector > grain shifter (same grains on both sides, so stereo image holds) > output.

- `src/PitchTracker.h`: time domain. A normalised difference function on a 12 kHz copy finds the period and decides voiced or not, with octave protection. Then full rate pitch marks: each period is found by matching it against the one before. Self contained, for copying into later plug-ins.
- `src/PsolaShifter.h`: pitch-synchronous grains centred on those marks, spaced at the wanted output period. One period of window each side. When nothing needs correcting, or the sound is unvoiced, grains read the input at the reported delay and windows sum to exactly 1, so the output is the delayed input. Consonants and breath therefore pass untouched, with no switch to click. Self contained.
- `src/PitchCorrector.h`: target note, Retune Speed, Flex Amount, Humanize, Natural Vibrato, Transpose, on a 1 ms tick.
- `src/Scales.h`, `src/KeyFollower.h`: masks, and the key file, read on a background thread.
- `src/PluginProcessor.cpp`: parameters, latency, bypass, readings. `getReadings()`, `getPitchPoint()` and `takeLevels()` give the window what it shows without touching audio.
- `src/TuneEditor.cpp`, `src/window/`: the window. No audio code. Inter is embedded, with its license in `assets/fonts/`.

## Latency

Exact and reported, the same bypassed. At 48 kHz: Soprano 9.0 ms, Alto/Tenor 13.7 ms, Low Male and Wide Range 20.6 ms. The lowest note in the range sets it. Changing Input Type fades out, jumps and fades in over 4 ms, then tells the host.

## Measured

All in `tests/results.txt`. Limits are in `tests/PassLimits.h`, committed before any DSP existed. Run: `build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt`.

- Tracking error 80 Hz to 1 kHz: steady 0.02 cents median, 0.12 cents at the 95th percentile. Vibrato 1.2 cents median. Octave errors: 0 per minute across 246 s of synthetic voice, with scoops, leaps and breath.
- Real speech and `say` singing: 16 octave disagreements per minute against a slow two-sided reference, which is not ground truth. 9 of the 14 are on the Organ voice, which is chords.
- Output pitch at instant speed: 3.5 cents worst, at the top near 1 kHz. Mostly under 0.01. On real speech, median distance from a note goes from about 25 cents to 10 or less.
- Retune Speed 25, 50, 100, 200, 400 ms measures 22, 48, 97, 197, 398 ms.
- Formant positions at +/-1 and +/-3 semitones: worst move 4.98% on the voices used for tuning, 4.6% on others, against a 5% limit. Thin margin: see below.
- Level at those shifts: within 1 dB.
- Vibrato kept: Flex 60 cents 99%, Humanize 100 99%, Retune 400 ms 99%. Hard settings leave 1%. Natural Vibrato -100 leaves 20%, +100 gives 191%.
- Hard step between two notes: 5 ms from 10% to 90%.
- Unvoiced sound: at most -315 dB difference from the delayed input. The middle of an S between two corrected vowels is the same.
- Artifacts on a corrected sustained tone: -47.5 dB non-harmonic, the same as the input's own -47.5 dB. Pitch steady to 0.002 cents, level to 0.001 dB.
- Key and Scale masks: all 24 keys, 408 sung notes, all right.
- Follow Key: 10 kinds of bad file ignored, good file used, last good held, off goes back to manual, thread picks it up by itself.
- CPU: about 0.5% of one core in the test program, 0.7% through the real AU.
- The window, real window at the screen's 2x, 27 frames a second, speech playing, share of one core on top of audio. Final code, every run: 3.46, 4.36, 4.40, 3.80, 3.45% started with no other build running, though other builds were starting around them (load 3 to 5), and 6.09, 5.87, 4.75% with two other test programs running flat out. Never measured on a truly idle machine. Stops repainting when nothing moves. Run: `build/RenderWindow_artefacts/Release/RenderWindow --live`.
- Window against audio: the output with the window open, ticking and painting nulls to exact zero against the window closed. Through the installed AU, 1.1.0 is bit identical to 1.0.1 on a sung file at default and a speech file at instant speed (`tests/AuRender.cpp`, then `cmp`). The levels added for the meters only read.

## What was hard to measure, and what the numbers do not say

- Formants are the weak point of any grain shifter when shifts are large and the voice is high. Peak positions hold, but peaks get flatter: the first formant reads up to 11.6 dB flatter than a real voice at that pitch, worst case across +/-1 and +/-3 semitones. At no correction it is identical to the input. Longer grains were tried and were worse.
- Reading formants needed three tries. Picking peaks could not read a real voice to better than 3 to 10%. The final test glides the voice over an octave so harmonics fill in the shape, then matches the shape in bands holding two formants. It reads known moves within 2%, and two real voices with the same vocal tract within 1.9%. A single narrow first formant on its own, as in /i/, cannot be read this way, so it is not claimed separately.
- A small grain reading speed correction in the shifter was tuned on the test voices to bring formants within the limit, hence the thin margin.
- The test for Tracking uses noise at -4 dB, moved from -8 dB after the first run: at -8 dB even Tracking 0 calls the voice pitched, which is correct, so that level did not test anything.

`build/PluginTest_artefacts/Release/PluginTest demo` writes corrected `say` files to `build/demo` for a quick listen outside any host.
