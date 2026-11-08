# Tune Pro

Advanced automatic vocal tuning with two engines, MIDI targets, a mode for each note, and a window built around a live piano roll of your pitch above a clickable keyboard.

**Load it:** in an audio effect slot, Audio Units > Luke Garito > Tune Pro. AU code `aumf STpr Luke`. Mono and stereo. It takes MIDI, but default settings tune with no MIDI at all.

Measured on synthetic voices and macOS `say`. That does not prove it sounds good on a real singer.

## Controls

Every control is a real AU parameter that a host can find by name. The window only reads them and writes them.

| Control | What it does |
|---|---|
| Key, Scale | Root and scale: Chromatic, Major, Minor, Harmonic Minor, Melodic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Phrygian, Lydian, Mixolydian, Locrian. |
| Note C to Note B | Each note on top of the key and scale. Tune follows the scale. Remove is never a target. Bypass is a target left uncorrected, even if it is off the scale. Click a key in the window to cycle them. |
| Retune Speed | Time constant of the correction, 0 to 400 ms. 0 reads Instant. |
| Flex Tune | How far a note may drift before correction applies, 0 to 100 cents. Inside that it is left as sung, outside it is brought to the edge. |
| Humanize | Slows correction on the sustained part of a note, up to 400 ms extra. The start of the note stays quick. |
| Natural Vibrato | Scales the vibrato that is left. -100 takes most of it out, +100 doubles it. |
| Vibrato Amount, Rate, Delay | Adds a vibrato: 0 to 100 cents, 2 to 9 Hz, starting 0 to 1500 ms into the note with a 150 ms fade. |
| Throat | Formant shift, 60 to 150%. Above 100 lowers the formants, as a longer throat would. Pitch does not move. Only voiced sound is changed. |
| Formant Preserve | On keeps the formants. Off lets them follow the pitch. |
| Transpose | -24 to +24 semitones, after correction. |
| Input Type | Voice range: Soprano, Alto/Tenor, Low Male, Wide Range. The lowest note sets the latency. |
| Reference A4 | 415 to 466 Hz. |
| Tracking | Which sounds count as pitched. 0 is choosy, 100 tunes rough, breathy sound too. |
| Follow Key | Takes key and scale from `~/Library/Application Support/SmoothPlugins/key.json`, written by Smooth Key. Bad, stale or low confidence data is ignored and the last good key holds. |
| MIDI Targets | Held notes are the targets, and the nearest held note wins. With no notes held, nothing is corrected. The sustain pedal holds notes, All Notes Off lets go. Note modes do not apply here. |
| MIDI Octave | All Octaves: a held note counts in every octave, so the singer stays in their register. As Played: only that exact note. |
| Classic Mode | The plain fast snap. Instant correction whatever Retune Speed, Flex Tune, Humanize and the vibrato controls say, with plain grains and no vocal tract model. |
| Engine | Low Latency or High Quality. See below. |
| Mix | Tuned voice against the input at the same delay. |
| Bypass | Keeps the same delay. |

Readings, which never write automation: Pitch Off Target, Detected Pitch, Output Pitch, Correction Applied, Target Note, Active Key, Latency.

## Engines

Latency at 48 kHz, reported to the host and measured on noise and voices:

| Input Type | Low Latency | High Quality |
|---|---|---|
| Soprano | 9.0 ms | 54.0 ms |
| Alto/Tenor | 13.7 ms | 58.7 ms |
| Low Male, Wide Range | 20.6 ms | 65.6 ms |

- **Low Latency** sits as close to the input as the longest period allows. Use it while tracking.
- **High Quality** adds 45 ms. That buys a vocal tract model that sees both sides of each moment, a first look at whether a sound is voiced before it reaches the output, and grains centred on the pulses of the voice. Use it for mixing.

Changing Engine or Input Type warms the other engine while the first keeps playing, then crossfades over 12 ms, so there is no gap and no click. The host is told the new latency at that point. During those 12 ms the two run at different delays, so level can dip briefly: 7 dB at worst in a 5 ms reading.

## Presets

Natural Pop, Transparent Ballad, Light Touch, Keep The Vibrato, Tight Modern, Hard Tune, Hard Tune Glide, Robot Flat, Classic Snap, Flat Then Sung Vibrato, Final Mix High Quality, Tracking Low Latency. They set how the tuning behaves and leave Key, Scale, Input Type, Follow Key and the note modes alone. Only the last two touch Engine.

## Window

Ink, coral and white. Coral only ever means what the tuning is doing. The font is Inter, SIL OFL, in `assets/fonts/`. Pictures of every state are in `docs/window/`.

- **Header:** presets, Key, Scale, Follow Key, then the note being hit and a bar showing how far the voice was moved.
- **Pitch display:** the last 8 seconds on a lane per note. White is your voice, coral is the corrected voice, the filled gap is the correction, and each note being hit is a block in its lane. Notes outside the scale have the darkest lanes. It follows your range by itself, in whole notes.
- **Keyboard:** click a key to go Tune, Remove, Bypass. Double-click resets. Right-click lists all three. The note being hit lights coral.
- **Controls:** Retune Speed, Flex Tune, Humanize, Natural Vibrato, Mix, then Bypass, Engine and the delay it adds.
- **Advanced:** Throat, Transpose, Input Type, Formant Preserve, Tracking, Reference A4, Classic Mode, the added vibrato and MIDI. While closed it says how many are changed.

Drag any control, shift-drag for fine moves, click a value to type one, double-click to reset. A second click straight after opening a menu closes it and resets it. The line at the bottom says what the control under the mouse does. Drag the corner to resize. The size and whether Advanced is open are saved in the session, and nothing else is added.

Nothing in it can block audio: it reads atomics and writes through parameters with begin and end gestures, so automation and the window move each other.

CPU and how it is kept down: see the Measured section. `build/RenderWindow_artefacts/Release/RenderWindow --live [runs] [seconds]` measures it, and with a folder name it draws the PNGs.

## Signal flow

Input > pitch tracker > corrector > vocal tract model takes the tract out > grains at the new pitch > model puts the tract back > level trim > output, with the dry input at the same delay for Mix and Bypass.

- `src/TuneEngine.h`: one whole path, and the two engines' timing. `src/PluginProcessor.cpp` owns two of these and hands over between them.
- `src/PitchTracker.h`: from Smooth Tune. Marks now slide 5 times slower towards the loudest point, a fix found while building Smooth Harmony.
- `src/PsolaShifter.h`: from Smooth Tune. Grains now carry the whitened voice and centre on its pulses, below 400 Hz. Classic uses the old plain grains.
- `src/EnvelopeFilter.h`: new. Linear prediction lattice filters, computed every 2.5 ms. With nothing shifted the pair cancels exactly, so consonants and uncorrected notes pass unharmed. On high voices the model is drawn through the harmonic peaks, because a direct fit locks on to single harmonics. While shifting it narrows the lowest formants, by how far the harmonics there have moved, up to half a spacing. In High Quality, below 400 Hz, it also remembers each harmonic's level where it was heard for about a fifth of a second, up to 2.5 kHz, so a glide or vibrato fills in the formant peaks that sit between harmonics, and fits that with 48 poles instead of 32. It lets that memory go when the pitch moves faster than 30 semitones a second. For 40 ms after a note starts, the model's shape moves to the new one over 2.5 ms instead of at once, so a note after silence does not burst.
- `src/PitchCorrector.h`: target note, Retune Speed, Flex Tune, Humanize, vibrato, MIDI targets, Classic.
- `src/TuneEditor.cpp` and `src/window/`: the window. Separate from the DSP.

## Measured

Window, real window at Retina size with speech playing, 30 frames a second, method from `~/smooth-resonance/docs/window-cpu.md`, 5 runs: 11.2%, 9.2%, 4.0%, 4.5%, 3.9% of one core. The work is steady at 3.3% to 3.5% of a fast core in every run. The two high runs are the same work landing on the slow cores: 18% and 49% of their time on fast cores, against 89% to 96% in the others. Stopped with no audio: 1.3%. Audio CPU is unchanged, AuBench 2.7% default and 3.0% worst preset before and after. With the window open and painting, output nulls against no window, largest sample difference 0.


All in `tests/results.txt`, on both engines. Limits are in `tests/PassLimits.h`. Run: `build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt`. Before and after numbers for each step are in `CHANGELOG.md`.

| | Smooth Tune | Low Latency | High Quality | Limit |
|---|---|---|---|---|
| First formant flatter than a real voice | 11.6 dB | 9.4 dB | 5.4 dB | 11.6, 7 |
| Formant move, +/-1 to +/-3 semitones | 4.98% | 4.43% | 2.29% | 5, 4 |
| Level change at those shifts | 1.15 dB | 1.45 dB | 0.60 dB | 1.5 |
| Output pitch at instant speed, worst | 3.44 cents | 2.53 cents | 0.89 cents | 5 |
| Non-harmonic energy, 30 cents corrected | -47.5 dB | -46.1 dB | -45.9 dB | -35 |
| Big shifts +/-7, move against a real voice | n/a | 6.3% | 2.5% | 8 |
| Burst at a note start after silence | n/a | 2.1 dB | 1.0 dB | 3 |
| High notes, first formant against its surroundings | 11.8 dB, plain grains | 13.5 dB | **13.1 dB** | 9 |

Bold fails its limit. See below.

- Note starts, share of 10 to 40 ms within 10 cents: Low Latency 88%, High Quality 98%.
- MIDI target: within 0.26 cents, 95th percentile, six notes. No notes held: -91 dB against the delayed input. Letting go: back within one 2 ms reading.
- Note modes: Remove lands on the next note within 0.53 cents, Bypass stays where sung within 0.02 cents, in Chromatic and C Major.
- Classic step, 10% to 90%: 5.5 ms, against 30 to 35 ms at Retune Speed 20. The reading itself is about 9 ms wide at this pitch, so this is near the limit of what it can resolve. Flattens vibrato to 1% with Flex Tune and Humanize at full.
- Transpose -24 to +24: within 0.67 cents.
- Throat 75 to 130%: formants within 2.6% of the setting, pitch within 0.63 cents, an S unchanged at -326 dB.
- Engine switch on a voice, both ways, 7 pitches: biggest step 1.36 times the voice's own, deepest 5 ms fall 7.0 dB.
- Vibrato: 30 cents measures 29.6 to 29.8, 5.5 Hz measures 5.5, Delay 800 ms reaches half depth at 830 to 882 ms.
- Mix 0: -330 dB against the delayed input. Mix 50 on an untouched voice: 0.00 dB change.
- CPU, one core, 48 kHz stereo: about 3.4% while correcting in Low Latency, 4.4% in High Quality, about 8% at 96 kHz.

## What does not pass, and what the numbers do not say

- **High notes still read flat.** Around 440 and 660 Hz the first formant reads 13.1 dB flatter than a real voice, against a 9 dB limit, and the model does not beat plain grains on this one number (11.8 dB). Harmonics there are wider apart than a formant, so on a held note the peak's height is simply not in the input. The memory is off above 400 Hz, because there even vibrato does not sweep a harmonic across the whole peak. What the model does fix there: formant position read against the input goes from 45% to 16.5%, and overall colour from 6.6 to 5.7 dB.
- **Held out voices still fail two limits.** The first set was looked at while tuning 1.3.0 and 1.4.0, so it is no longer clean. Low voices, 98 to 220 Hz, now pass: formant move 1.52% against 4% (1.3.0: 4.24%), level 0.72 dB against 1.5 (1.3.0: 1.69). High voices, 392 to 740 Hz: first formant 15.9 dB flatter than a real voice against 9 dB. Held notes, each harmonic against a real voice: 4.54 dB on average against a 3.5 dB target set for a later version.
- **A second held out set was written for 1.3.0 and run once, at the end.** Women's vowels at 175 to 262 Hz, from the Peterson and Barney table, with their own bandwidths. 1.2.1 read 11.2%, 1.3.0 6.32%, and 1.4.0 2.29% against 4%, but 1.4.0's settings were chosen with this set in view, so that last number is not clean.
- **A third held out set was written for 1.4.0 before any of it was run.** Men at 123 and 185 Hz and women at 233 and 311 Hz, from the Hillenbrand table. Formant move 3.47% against 4%, level 0.80 dB. 1.3.0 on the same voices reads 4.62%, a fail. It was run four times, because two settings changed after its first run, each for a failure on another test, and it read 3.47% every time.
- **Why the formant numbers stop here.** On a held note with no vibrato at all, nothing sweeps the harmonics, so High Quality behaves like 1.2.1: the input's harmonics sit either side of narrow peaks and the model never sees how tall they are. Measured on 2026-11-07 on the vowel u at 165 Hz moved 1 semitone: the harmonic that lands on the second formant came out 4.8 dB weaker than a real voice. Real singing nearly always moves a little, which is why the memory helps, but a dead straight note still needs a model that works out peak heights the harmonics don't show. One unfinished attempt at that, a fitted formant model, read 17.4% formant move and 18.0 dB flat on high notes, so it was not kept. It is saved in `git stash list`.
- **Information floor, measured on 2026-11-08, before trying more settings.** The question: given only the harmonics the input actually has, what is the best any model could read? Measured two ways. First, a standalone program builds the test voices from their exact formula and tries estimates that see the true level at each input harmonic and nothing else: straight lines through the harmonics (what the plug-in draws on high notes), a fitted formant model whose bandwidths come from a prior (the exact bandwidths, the tuning voices' table, or a published trend), and lines through every point heard over the last 0.2 to 1 s at any pitch. It reads its results with the same spectrum and peak measures as the tests. Second, a test-only hook gave the plug-in the true vocal tract in place of its own model, which shows what the grains themselves lose. Neither is in the plug-in; both are in `git stash list`.

  | | Held notes, mean | High notes, flatter | High held out, flatter | 
  |---|---|---|---|
  | Limit | 3.5 dB | 9 dB | 9 dB |
  | 1.4.0 | 4.54 | 13.1 | 15.9 |
  | Plug-in given the true vocal tract | **3.45** | 2.76 | 7.27 |
  | Ideal lines through one moment's harmonics | 3.86 | 9.85 | 10.95 |
  | Ideal formant model, exact bandwidths | 3.89 | 18.6 | 11.7 |
  | Ideal formant model, tuning table bandwidths | 4.21 | 18.2 | 11.1 |
  | Ideal formant model, published bandwidth trend | 3.82 | 15.9 | 11.1 |
  | The same three, also told the exact voice source | 3.15, 3.47, 3.02 | 15.1, 14.9, 14.5 | 11.0, 15.8, 15.8 |
  | Ideal memory of harmonics heard, 0.2 s, any pitch | (no help on a held note) | 10.2 | 8.7 |
  | Ideal memory, 0.5 s | | 6.2 | 8.5 |
  | Ideal memory, 1 s | | 5.2 | 5.4 |

  What it says:
  1. **Held notes cannot pass with the grain shifter as it is.** Even handed the true vocal tract, the plug-in reads 3.45 dB against 3.5: the grains use nearly the whole budget, because they keep the input's pulse shape while a real voice at a new pitch changes it. No estimate from the harmonics comes near zero: the best, a formant model also told the exact voice source, which no plug-in can know, reads 3.02. A bandwidth prior does not help once the source is unknown (3.82 to 4.21 against 3.86 for plain lines), because the source's own ripple from harmonic to harmonic looks like formants to the fit. At 740 Hz there are fewer harmonics below 5 kHz than the model has unknowns, and every formant model is worse than lines (5.5 to 6.7 dB against 4.8).
  2. **High notes and high held out voices cannot pass from one moment's harmonics.** Perfect lines read 9.85 and 10.95 dB. The formant model with a bandwidth prior is worse on the worst case, 11 to 19 dB, because with 4 to 8 harmonics its fit sometimes lands on the wrong peak. This matches the stashed 2026-11-07 attempt (18.0 dB). Fitting with 32, 48 or 96 poles changes little (10.5, 10.8, 9.6).
  3. **Only remembering harmonics over time could reach 9 dB there**, and only at 0.5 to 1 s, at every pitch. In the plug-in that did not carry over. Memory up to 1400 Hz at 0.2, 0.5 and 1 s read 13.7, 13.9 and 11.0 dB on high notes, and at 0.5 and 1 s moved high notes' formants 18.4% against the 16.9% limit, which 1.4.0 passes. At 1 s high held out voices read 12.6 dB (15.9 in 1.4.0) and held notes 4.53. A memory that long would also blend one vowel into the next in real singing, which no test checks yet.
  4. **The worst high note cases are not peak heights at all.** /e/ and /o/ at 660 Hz moved up 3: no harmonic of the new voice ever crosses the first formant, so the reading is the edge of the lowest harmonic's glide and the floor below the fundamental. Between 300 and 450 Hz the output has about 0 to +5 dB where a real voice has -16 to -9; given the true vocal tract it has -13 to -1, so part of it is the model holding its level below the lowest harmonic and part is the grains. Letting the model fall away below the lowest harmonic (12 dB an octave) read 12.5 dB but broke two passing limits (low voices' first formant 9.8 dB against 7, high note formant move 16.94% against 16.89%).

  Nothing beat 1.4.0 without breaking a passing test, so the plug-in is unchanged. A real fix for held notes needs a different shifter, one that keeps each harmonic's own level from the voice source instead of reading it off a grain, and the high note limits need either that or a long memory plus a test for vowel changes.
- **A fourth held out set** (`testHeldOutFourth`, `heldout4`): children's vowels from Peterson and Barney (1952), 277 and 330 Hz, and 349 to 698 Hz, with their own bandwidths. Written and committed before it was run, then run once on 1.4.0, since nothing was changed: speaking pitches, formant move 2.29% against 4% (floor 2.50%), level 0.70 dB against 1.5, both pass. Singing pitches: first formant 12.2 dB flatter where a harmonic crosses it, against 9, a fail; held notes 3.84 dB mean against 3.5, a fail; formant move 27.9% against its floor of 33.0% plus 2%, pass; sharper 2.08 dB against 6, pass. So it agrees with the first set: the same two high voice limits fail on new speakers. Nothing was tuned on it, but it has now been seen once. `tests/results.txt` is still the full 1.4.0 run and does not include this set.
- **A harmonic shifter, tried on 2026-11-08 and not shipped** (branch `harmonic-shifter`, commit c317202; the plug-in on `main` is still 1.4.0). The information floor above said held notes need a shifter that keeps each harmonic's level from the voice. This one does: High Quality reads four input periods at a time at a fixed number of samples per period, so each harmonic sits on one FFT bin, scales each harmonic by how the vocal tract differs between where it was and where it is going, and reads the frame back at the output period, one frame per output period. A harmonic too weak next to the noise between harmonics takes the level of the one now where it is going. The vocal tract change comes from a memory of each harmonic number's level against the pitch it was heard at, during the current voiced stretch only: harmonic k heard at the pitch it is moved to gives the change exactly, and harmonics heard at the same frequency at different pitches tie their own levels together so one can stand in for another. Each new frame is checked against what was stored at its pitch 50 ms or more before, and a mismatch over 6 dB (a new vowel) clears the memory. Where nothing was heard, straight lines through the moment's harmonics. Low Latency and Classic were left as they were.

  Offline first (`tests/prototype/HarmonicPrototype.cpp`, true pitch, dev voices): handed the true vocal tract, the shifter itself reads 0.06 dB on dev held notes and 0.81 dB on dev high notes, so it loses almost nothing. With straight lines it reads 4.15 dB on held notes; with the memory, high notes read 3.5 dB. In the plug-in (`tests/prototype/EngineHarness.cpp` runs the engine without JUCE) it took several fixes for real speech: frames read before the tracker's marks arrive, voicing that flickers for a period, frames overlapping where the input is read a period apart, and the memory carrying old loudness into new notes. Every test group except the held out sets then passed on both engines, and the Low Latency null test still matched 1.4.0. A vowel change test was added with its limit set first (`testVowelChange`, `vowel`).

  | High Quality | 1.4.0 | Harmonic shifter | Limit |
  |---|---|---|---|
  | High notes, first formant against its surroundings (dev) | 13.1 dB | **5.2 dB** | 9 |
  | High notes, formant move against the input (dev) | 16.5% | 12.4% | 16.9 |
  | First formant flatter than a real voice (dev) | 5.4 dB | 4.1 dB | 7 |
  | Level change, +/-1 to +/-3 (dev) | 0.60 dB | 1.21 dB | 1.5 |
  | Big shifts +/-7, move against a real voice (dev) | 2.5% | 6.7% | 8 |
  | Real speech, 99th percentile frame level change | 2.52 dB | 2.88 dB | 3 |
  | Vowel change test, move against the new vowel | 2.1% | 3.1% | 4 |
  | CPU, voice at 180 Hz, 48 kHz stereo | 4.4% | 2.6% | 8 |
  | Latency | 13.7 / 58.7 ms | unchanged | |
  | First held out set, 392 to 740 Hz, first formant where crossed | 15.9 dB | **6.1 dB** | 9 |
  | First held out set, held notes | 4.54 dB | 4.15 dB | 3.5 |
  | First held out set, 98 to 220 Hz, move / level | 1.52% / 0.72 dB | 2.29% / 1.06 dB | 4 / 1.5 |
  | Second held out set, move / level | 2.29% / not recorded | 3.32% / 1.36 dB | 4 / 1.5 |
  | Third held out set, move | 3.47% | **4.77%, fails** | 4 |
  | Third held out set, level | 0.80 dB | **1.62 dB, fails** | 1.5 |
  | Fourth held out set, first formant where crossed | 12.2 dB | **5.0 dB** | 9 |
  | Fourth held out set, held notes | 3.84 dB | **3.37 dB** | 3.5 |
  | New fifth held out set, speaking, move / level | 2.09% / 0.49 dB | 2.50% / 1.08 dB | 4 / 1.5 |
  | New fifth held out set, singing, first formant where crossed | 18.6 dB | 12.0 dB, fails | 9 |
  | New fifth held out set, held notes | 4.02 dB | 4.38 dB, fails | 3.5 |

  Bold marks a change of verdict. The fifth set (`testHeldOutFifth`, `heldout5`) is Catford's published vowel averages (A Practical Introduction to Phonetics, 2001, p. 154, via Wikipedia's Formant article): front rounded and back unrounded vowels no earlier set had, F3 to F5 and bandwidths set by hand from the vowel's class. It was written and committed with the expected result before it was run (c317202), run once on the shifter, then once on 1.4.0 for comparison. Expected: speaking passes, singing first formant passes, held notes fail. Measured: speaking passed, held notes failed as expected, but the singing first formant failed at 12.0 dB, so the high note fix does not carry over to these vowels (open back vowels with F1 and F2 close together, male formants at high pitch), although it is better than 1.4.0's 18.6 dB. Its held notes were worse than 1.4.0's.

  Why it was not shipped: it fixes two of the three named failures (high notes, high held out voices) and both of the fourth set's, and improves held notes on the first and fourth sets, but the third held out set now fails two limits that 1.4.0 passes, and the fifth set's singing voices still fail. Nothing was tuned on the held out sets. On dev voices at the third set's pitches and shifts (123 to 311 Hz, +/-4) 1.4.0 reads 5.9% and 8.2% formant move and the shifter 3.5% and 6.9%, so 1.4.0's pass there was close, but a pass it was.

  Held notes are still the hard part. With true phases and the test voices' own pulse shape built into the model, an all-pole fit through the harmonics finds the vocal tract exactly (0.0 dB on dev held notes), but with the pulse shape a little off it reads 4.85 dB, worse than straight lines (4.10), and assuming only that the pulse's phase is flat above the second harmonic it reads 7.9 to 11 dB. So phase only helps if the model knows the singer's pulse, which would be fitting the test synthesiser rather than a voice, and it was not used. Straight lines and every magnitude only estimate tried read 4.05 to 4.2 dB on dev held notes.

  If this is picked up again: the third set's two failures and the fifth set's singing voices are where to look, on new dev voices like them (back vowels around 300 Hz moved by 4, open back vowels at high pitch), and both sets are now used, so a sixth held out set is needed.
- **Three limits were changed after measuring, each because it could not be read:**
  1. High note formant move was 8% against the input. The input and a real voice with the same vocal tract already read 14.9% apart there, so it is now 2% beyond that floor.
  2. Big shift formant move stays 8%, but reads against a real voice, because the input against a real voice reads 10.5%.
  3. High note and big shift peak height reads against the peak's surroundings. Smooth Tune's version compares it with half the formant's frequency, which on high notes is below the lowest harmonic.
- The first formant numbers lean on a sharpening amount, a level trim and the memory's settings, all set on these test voices. The vowels and pitches in the second formant set were included in that tuning, so only the second held out set is a clean test.
- Colour on big shifts in High Quality is 1.5 dB, down from 3.3 dB in 1.2.1 and 1.7 dB in 1.3.0, because 1.3.0 and later need far less sharpening.
- Real speech: the worst 1% of frames change level by 2.52 dB in High Quality and 2.86 dB in Low Latency, against a 3 dB limit (1.3.0: 2.42 and 2.71). Correction on speech lands 0.1 to 0.5 cents further from the note, median, on the four test voices, for example 2.5 to 3.0 cents.
- Tests changed for honest reasons: a steady voice repeats, so delay on a voice is read only around the reported delay; the onset test moved from 40 to 25 cents sharp, because 40 lands on the far side of the note line.

`build/PluginTest_artefacts/Release/PluginTest demo` writes corrected `say` files for both engines to `build/demo`, to listen to outside any host.
