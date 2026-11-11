# Changelog

## 1.6.0, 2026-11-11

Four fixes found on Luke's own takes (17 whole songs, Rode NT1, 2026-11-10). Low Latency and Classic change on purpose, so the Low Latency null test now holds 1.6.0's output (19 of its 24 cases changed; the other 5 are the parameter list and the four latencies). Latency, parameters and AU codes are unchanged. High Quality changes only where listed. Real take numbers are the 11 working takes with Key and Scale set to each song's key, measured by `_shared/realvox`; the 6 blind takes were run once at the end and are in `_vox/rode/work/tune-pro/REPORT.md`.

**1. Alto/Tenor follows notes under its range, down to 70 Hz, at the same 13.7 ms.** Its range starts at 98 Hz and Luke sings down to 77 to 95 Hz; those notes were not followed at all and came out exactly as sung. A second reading now looks at the longer periods over a window that reaches further back in time, not further ahead, and only speaks where the range's own reading says "not voiced" (`PitchTracker::analyseLow`). The range's reading is the same code and never sees the second one: test voices at 99 to 440 Hz come out bit for bit the same with and without it.
- What it costs. In Low Latency a grain for a note under 98 Hz is cut from the newest period already known, up to 1.4 periods back; on Luke's low notes the band above 9 kHz reads 4 dB over the dry voice in the top 5% of frames, against 1 dB in Low Male, which has the delay for a whole period. High Quality has no such cost. After a note under 98 Hz the rest of that phrase is not sample for sample 1.5.1 any more (the marks carry on from the low note), and the level trim, which averages over the corrected voice it has heard, differs by up to 0.3 dB for a few seconds.

**2. Low Latency and Classic: the pitch control is read in line with the audio.** The grains give out the input one whole latency late, 1.33 times the Input Type's longest period, but the pitch the tracker reports is only about two of the voice's own periods old. So on every note above the bottom of the range the correction ran ahead of the sound it belonged to: on a test voice with vibrato 0.8, 3.9 and 6.5 ms early in Alto/Tenor at 147, 196 and 262 Hz, and 7.7, 10.7 and 13.2 ms in Low Male and Wide Range. The control is now read that much further back (`TuneEngine::pitchInLine`). Whether a sound is voiced is decided where it was, so notes are not picked up later.
- What changes in the sound: a correction no longer starts before its note. Measured from the moment the new note arrives in the audio, a flat semitone step reaches its note in 16 ms, where 1.5.1 read 10, because 1.5.1 had started about 4.5 ms early.
- Under about 140 Hz in Alto/Tenor the reading is older than the whole latency, so there the control is still late, by up to 5.8 ms at 98 Hz, as it always was. Only more latency fixes that: Low Male and Wide Range have it.
- High Quality is not changed. Its control runs ahead in the same way, and reading it in line moves the ninth held out set's speaking formant move from 4.62% to 5.00% (limit 4), because its vocal tract settings were tuned with the lead in place. The code is there, switched off (`alignHighQuality`).

**3. A voice with nothing to correct comes out as the delayed input.** With no correction the grains step from mark to mark at the reported delay. Where the next mark was not there (the marks had been started again, or it was not known yet) the grain was cut from the nearest mark, up to 0.6 of a period away, and that offset was then kept for the rest of the uncorrected stretch. It is now a plain read at the reported delay, and the two halves of the window share one raised cosine as long as the spacing, so the output is the input, sample for sample. An offset left by a correction is walked back at 0.1% of the period a grain (1.7 cents, 600 periods at most). This is the grains, so it also holds in High Quality below about 220 Hz; its harmonic shifter above that never promised it.

**4. A held note keeps its target through a waver.** The target was the nearest note at every moment, with 12 cents of hysteresis. A singer who holds a note near the line between two notes and wavers by more than that sent the target back and forth, and the tuner made the waver bigger: a test voice 47 cents sharp of A3 with a +/-20 cent vibrato came out with a 45 cent waver, 34 cents from either note. Now a note that has been held (within 35 cents of its own average for 150 ms) keeps its target until the voice has stayed past the line for 80 ms, is 35 cents past it, or got there in one move of 45 cents within 60 ms. The same voice: a 19 cent waver, 14 cents from the note. A real step is taken where it always was (flat semitone step, at once and as a 60 ms glide: no later than before, apart from point 2), and a voice that slides, as speech and rap do, is not held at all. Classic is left as it was.

| | 1.5.1 | 1.6.0 | Limit |
|---|---|---|---|
| Alto/Tenor, held notes at 78 to 92 Hz, output pitch error at instant speed | 35.0 cents (not corrected) | 0.19 cents | 5 |
| Alto/Tenor, 87 Hz, non-harmonic energy in the corrected voice, Low Latency / High Quality | not corrected | -38.5 / -45.6 dB (Low Male -41.9 / -45.7) | -35 |
| Pitch movement against the sound, test voice, worst of three Input Types, Low Latency | 13.2 ms early | 0.86 ms early | 1 |
| The same, High Quality (not fixed) | 15.2 ms early | 15.2 ms early | none |
| An uncorrected voice after a correction, against the input at the reported delay | +5.0 dB | -168.8 dB | -90 |
| A note on the line with +/-20 cents vibrato, waver in the output | 45.4 cents | 19.4 cents | 20 |
| Flat semitone step, time to the new note, at once / 60 ms glide, Low Latency | 10 / 4 ms | 16 / 8 ms | 20 / 14 |
| After a hard step the new note is on pitch, 95th percentile, Low Latency | 0.06 cents | 1.80 cents | 5 |
| Real speech, worst 1% of 20 ms frames, level change, Low Latency / High Quality | 2.86 / 2.64 dB | 2.89 / 2.64 dB | 3 |

| Luke's 11 working takes, Key set to the song, default settings unless said | 1.5.1 | 1.6.0 | Limit |
|---|---|---|---|
| Held notes under 98 Hz left exactly as sung, of 12 (Low Latency / High Quality / Classic) | 4 / 3 / 3 | 0 / 0 / 1 | 0 |
| Held notes under 98 Hz within 10 cents of their note, Classic, of 12 | 1 | 11 | |
| Octave errors on held notes | 0 | 0 | 0 |
| Output over a semitone from the voice for 20 ms or more, Alto/Tenor (Low Latency / Classic) | 27 / 7 | 10 / 5 | 0 |
| The same in Low Male, Low Latency | 171 | 16 | 0 |
| Where the voice slides, output moving the other way, sung / rap takes, Alto/Tenor | 3.28 / 4.73% | 2.98 / 4.32% | 5 |
| The same in Low Male | 5.80 / 8.71% | 2.97 / 4.40% | 5 |
| Pitch movement against the reported latency, median of takes, Low Male (Low Latency / Classic) | 332 / 332 samples early | 3 / 37 early | 48 |
| The same in Alto/Tenor, Low Latency | 8 early | 54 late (notes under 140 Hz, see point 2) | 48 |
| The same, High Quality, Alto/Tenor (not fixed) | 73 early | 50 early | 48 |
| Every note set to Bypass: output against the input at the reported delay, worst take, Low Latency | -6.5 dB | -155.9 dB | -90 |
| The same, stretches more than 24 samples off the reported delay | 93 of 2631 | 0 of 2633 | |
| The same in High Quality (the harmonic shifter above 220 Hz keeps its own timing) | -5.8 dB | -33.6 dB | -90 |
| Held notes on another scale note than his median was nearest, clear / on the line, of 739 | 2 / 22 | 3 / 25 | 0 clear |
| Held notes left as sung though 20 cents or more off, of 739 | 21 | 17 | 0 |
| Clicks by the shared rule | 8 | 9 | 0 |
| Start lag, clean starts, median / 90th percentile | 105 / 285 ms | 105 / 285 ms | 50 / 120 |

What the takes say about point 4: nothing either way. The notes that end on the other side of a line, or stay where they were, are mostly notes where his own pitch moves across the line during the note, and the default's Humanize 30 follows that 140 ms late (README). The test voice shows the rule working; his takes do not show it helping yet.

Not fixed, and why: the ninth held out set's speaking formant move still reads 4.619% against 4, exactly as in 1.5.1 (High Quality's formant path was not touched, so no tenth set was written); High Quality's control lead; the grains cut one period back from 98 to about 140 Hz in Alto/Tenor Low Latency (README, "Found on Luke's real takes"). Full suite: 539 pass, 3 fail (; 1.5.1: 520 pass, 1 fail, on 21 fewer checks).

## 1.5.1, 2026-11-09

High Quality's grains, used below about 220 Hz, now sit on the tracker's pitch marks instead of on a pulse position found once at the start of each voiced stretch. Low Latency, Classic and latency are bit for bit 1.5.0.

The held pulse position drifted up to half a period away from the real pulses on low voices, so each grain cut a pulse in two and the output from 800 to 1800 Hz came out 4 to 9 dB low. It showed on vowels whose first two formants sit close together, such as Cantonese u (353 and 721 Hz) at 149 Hz moved down 3.

| High Quality | 1.5.0 | 1.5.1 | Limit |
|---|---|---|---|
| Eighth held out set, speaking formant move | **6.32%** | 3.28% | 4% |
| Eighth held out set, speaking level | 1.16 dB | 1.10 dB | 1.5 dB |
| Ninth held out set (Mandarin), speaking formant move | **5.94%** | **4.62%** | 4% |
| Ninth held out set, speaking level | 1.49 dB | 0.94 dB | 1.5 dB |
| Formant move, +/-1 to +/-3 | 2.11% | 2.30% | 4% |
| First formant flatter than a real voice | 3.80 dB | 4.17 dB | 7 dB |
| Seventh held out set, speaking level | 1.04 dB | 1.22 dB | 1.5 dB |
| CPU, voice at 180 Hz, 48 kHz stereo | 5.36% | 5.43% | 8% |

Bold fails. Nothing that passed in 1.5.0 fails now.

How clean the held out numbers are: the eighth set was read to find the cause, so it is a regression test now. The fix was chosen on new dev voices (`EngineHarness speak8`). The ninth set, Mandarin men and women from Meng, Chen and Li (2006), was written blind and run once at the end. It still fails speaking formant move, 4.62% against 4, but less than 1.5.0's 5.94%; singing is identical on both. On close-formant vowels like these, a perfect real voice at the new pitch already reads about 3 to 3.7% against the input, so the room under 4% is small. Nobody has listened to 1.5.1 yet.

## 1.5.0, 2026-11-08

High Quality gets a new shifter for high and sung notes. Low Latency and Classic are bit for bit 1.4.0, and latency is unchanged.

**A harmonic shifter above about 220 Hz.** High Quality reads the voice one period at a time so each harmonic sits on its own frequency bin, and moves each harmonic by how the vocal tract differs between where it was and where it is going. The vocal tract comes from a memory of each harmonic's level against the pitch it was heard at, and on held notes from a fit of the voice source plus five resonances (`src/HeldEnvelope.h`). Below about 220 Hz the grains from 1.4.0 do better and are kept. The switch is at 205 Hz going down and 235 Hz going up, after the pitch stays past it for 40 ms, with a 20 ms level-matched crossfade.

**The level trim trusts its average only as far as its 1 s memory has filled.** In the first part of a glide across a narrow formant it had been pushing the gain up to 8 dB the wrong way.

| High Quality | 1.4.0 | 1.5.0 | Limit |
|---|---|---|---|
| Formant move, +/-1 to +/-3 | 2.29% | 2.11% | 4% |
| First formant flatter than a real voice | 5.38 dB | 3.80 dB | 7 dB |
| Level change, +/-1 to +/-3 | 0.60 dB | 0.67 dB | 1.5 dB |
| Big shifts +/-7, formant move | 2.50% | 1.48% | 8% |
| High notes, formant move | 16.48% | 11.42% | 16.89% |
| High notes, first formant against its surroundings | **13.14 dB** | 4.01 dB | 9 dB |
| Held notes, harmonics against a real voice | **4.54 dB** | 2.87 dB | 3.5 dB |
| First held out set, 392 to 740 Hz, first formant | **15.93 dB** | 6.36 dB | 9 dB |
| Seventh held out set (Korean), speaking level | 1.12 dB | 1.04 dB | 1.5 dB |
| Seventh held out set, singing first formant | **13.83 dB** | 6.78 dB | 9 dB |
| Seventh held out set, held notes | **3.65 dB** | 2.41 dB | 3.5 dB |
| Eighth held out set (Japanese, Cantonese, Greek), singing first formant | **11.63 dB** | 6.41 dB | 9 dB |
| Eighth held out set, held notes | **4.16 dB** | 2.39 dB | 3.5 dB |
| Eighth held out set, speaking formant move | **6.32%** | **6.32%** | 4% |
| CPU, voice at 180 Hz, 48 kHz stereo | 4.41% | 5.33% | 8% |

Bold fails. The full suite, with all eight held out sets, is 514 pass, 1 fail, the eighth set's speaking formant move (1.4.0: 481 pass and 3 fail, on fewer tests). Nothing that passed in 1.4.0 fails now.

How clean the held out numbers are: sets 1 to 6 were all seen while choosing settings, so they are regression tests now. The seventh set was written blind and run once, failed speaking level (1.587 dB), and was then used to fix that, so it is no longer clean either. The eighth set was written blind and run once at the end. It shows one fail, speaking formant move on low men's voices, 6.32% against 4, and 1.4.0 reads exactly the same there because below 205 Hz both run the same grains. So that is an old weakness, not a new one.

Trade-off: the trim now takes a full second to settle, so on a held note transposed by up to 24 semitones the level 0.5 to 1.0 s in reads 2.64 dB (was 0.82). That line has no limit and is back to its old number by 1 s. Nobody has listened to 1.5.0 yet.

## 1.4.0, 2026-11-08

Two fixes. Low Latency changes on purpose for the first one, so its null test now holds 1.4.0's output.

**Notes starting after silence no longer burst.** Where a note starts, each new 40 ms window holds far more of it than the last, so the vocal tract model changed completely within a few ms. The grains read up to a period either side, so the voice was whitened by one model and shaped again by another, and that mismatch rang out. In Low Latency the output at a note start reached 4.85, about 14 dB over full scale, on test voices peaking at 0.4. Now, for 40 ms after the level jumps, the model's shape moves to the new one over 2.5 ms instead of at once. The level itself is not held. After the first half second, steady test notes differ from 1.3.0 by 65 to 92 dB under the signal, from the slower level trim below. Speech, sung phrases and vibrato, which start notes often or swing in level, differ by 17 to 39 dB under the signal. A new test plays vowels at 110 to 560 Hz starting hard after silence, at once or over 20 ms, and looks for a burst in the output against the input.

**High Quality's glide memory model has 48 poles instead of 32.** With the memory showing it the peaks between harmonics, the finer model follows them, so less of the vocal tract is left in the grains to move with the pitch. Only that model gets 48: a direct fit with more poles locks on to single harmonics, measured as level jumps on speech (3.50 dB against 2.52), and a model through one moment's harmonics on high notes moved the formants further. The memory now runs below 400 Hz instead of 350. Low Latency keeps its level trim over 2 seconds instead of 0.6, because holding the shape through note starts moved its worst level change from 1.47 to 1.53 dB. High Quality keeps 0.6, because 2 seconds moved high notes' formants past their limit.

Also: vDSP's FFT gave slightly different results on buffers not 64 byte aligned, and where the heap put them changed from run to run, so the same input could give two slightly different outputs. The FFT buffers are now aligned.

| | 1.3.0 | 1.4.0 | Limit |
|---|---|---|---|
| Low Latency, burst at a note start after silence | 9.09 dB | **2.10 dB** | 3 dB |
| Low Latency, loudest sample at a note start, test voices | 4.85 | 0.47 | none |
| Low Latency, level change at +/-1 to +/-3 | 1.47 dB | 1.45 dB | 1.5 dB |
| High Quality, burst at a note start | 0.72 dB | 0.96 dB | 3 dB |
| High Quality, formant move, +/-1 to +/-3 | 3.32% | 2.29% | 4% |
| High Quality, formant move, other vowels and pitches | 3.11% | 2.91% | 4% |
| High Quality, first formant flatter than a real voice | 6.02 dB | 5.38 dB | 7 dB |
| High Quality, level change | 1.10 dB | 0.60 dB | 1.5 dB |
| High Quality, big shifts +/-7 | 2.09% | 2.50% | 8% |
| High notes, formant move | 16.48% | 16.48% | 16.89% |
| High notes, first formant against its surroundings | 13.15 dB | 13.14 dB | 9 dB |
| First held out set, 98 to 220 Hz, formant move | 4.24% | **1.52%** | 4% |
| First held out set, level change | 1.69 dB | **0.72 dB** | 1.5 dB |
| First held out set, 392 to 740 Hz, first formant flatter | 16.54 dB | 15.93 dB | 9 dB |
| Held notes, harmonics against a real voice | 4.59 dB | 4.54 dB | 3.5 dB |
| Second held out set, formant move | 6.32% | **2.29%** | 4% |
| New third held out set, formant move | 4.62% | **3.47%** | 4% |
| Third held out set, level change | 1.15 dB | 0.80 dB | 1.5 dB |
| Real speech, 99th percentile frame level change, High Quality | 2.42 dB | 2.52 dB | 3 dB |
| Real speech, the same, Low Latency | 2.71 dB | 2.86 dB | 3 dB |
| CPU, High Quality, voice at 180 Hz, 48 kHz stereo | 3.24% | 4.41% | 8% |

Bold now passes. 481 tests pass, 3 fail (1.3.0: 474 and 6). All 3 failed in 1.3.0 too, and nothing that passed in 1.3.0 fails now. AuBench, which runs the default Low Latency, reads 2.7% default and 3.0% worst preset, as before.

How clean the held out numbers are: the first and second sets were both used to choose 1.4.0's settings, so they are no longer clean. The third set, men at 123 and 185 Hz and women at 233 and 311 Hz from the Hillenbrand table, was written before any of it was run. It was run four times, because two settings changed after its first run, each for a failure on another test: it read 3.47% every time. 1.3.0 on the same voices reads 4.62%.

## 1.3.0, 2026-11-07

High Quality keeps formants in place much better. Low Latency is bit for bit the same as 1.2.1.

A held note's harmonics sit either side of a narrow formant, so no one moment shows how tall it is. As the pitch moves, by a glide or vibrato, the harmonics sweep across it. High Quality now remembers each harmonic's level where it was heard, for about a fifth of a second, and draws the vocal tract model through all of those points. Below 350 Hz, up to 2.5 kHz. When the pitch moves faster than 30 semitones a second, as at the end of a spoken phrase, the harmonics smear and the memory is let go. With the peaks seen, the old fixes are turned down: sharpening from 0.5 to 0.1, and the slower grain reading when going down is off.

| High Quality | 1.2.1 | 1.3.0 | Limit |
|---|---|---|---|
| Formant move, +/-1 to +/-3 semitones | 4.24% | **3.32%** | 4% |
| Formant move, other vowels and pitches | 4.15% | **3.11%** | 4% |
| First formant flatter than a real voice | 6.84 dB | 6.02 dB | 7 dB |
| Big shifts +/-7, move against a real voice | 5.94% | 2.09% | 8% |
| High notes, first formant against its surroundings | 13.5 dB | 13.2 dB | 9 dB |
| First held out set, 98 to 220 Hz, formant move | 5.82% | 4.24% | 4% |
| First held out set, level change | 1.17 dB | 1.69 dB | 1.5 dB |
| New second held out set, women 175 to 262 Hz, formant move | 11.2% | 6.32% | 4% |
| Second held out set, colour against a real voice | 3.08 dB | 1.79 dB | none, the input itself reads 1.71 |
| Real speech, 99th percentile frame level change | 2.40 dB | 2.42 dB | 3 dB |
| CPU, voice at 180 Hz, 48 kHz stereo | 2.45% | 3.24% | 8% |

Bold now passes. One limit now fails that passed: on the first held out set, one case comes out 1.69 dB louder against 1.5. The first held out set was looked at while tuning this version, so it is no longer clean. A second held out set was written for 1.3.0 before any of it was run, and run once at the end, with 1.2.1 on the same voices for comparison. Neither set reaches 4%.

## 1.2.1, 2026-11-07

Renamed to Tune Pro, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.2.0

A new window, in ink, coral and white. No DSP changes. The window with it open and painting nulls against no window at all, largest sample difference 0.

- The hero is an eight second piano roll that runs edge to edge: your voice in white, the corrected voice in coral, the gap between them filled, and each note being hit as a block in its lane.
- Under it, a full width octave of keys. Click one to go Tune, Remove, Bypass, double-click to reset. The note being hit lights coral. A removed note is gone from the keyboard, with only its name left.
- Retune Speed leads the controls, with what the setting does in words. Flex Tune, Humanize, Natural Vibrato and Mix sit beside it.
- A live note and correction bar in the header, presets, key and scale.
- Everything else is under Advanced: voice, tuning, added vibrato and MIDI. It reports how many of those are changed while closed, and the display gives up the room when it opens.
- All controls are the same parameters with the same IDs, names and ranges. Window size and whether Advanced is open are the only things added to saved state, both range checked.
- Added `tests/RenderWindow.cpp`, which draws the real window on real speech to `docs/window/` and measures its CPU, and new window tests in `tests/PluginTest.cpp`.

## 1.1.0

Quality work. High Quality numbers, worst case on the test voices, each step measured against the one before. Flat means the first formant reads flatter than a real voice.

| Step | Flat | Formant move | Level | Other |
|---|---|---|---|---|
| Version 1, plain grains | 11.6 dB | 4.98% | 1.15 dB | big shifts 15.7% and 10.2 dB, high notes 45% |
| Take the vocal tract out before grains and put it back after | 9.9 dB | 4.77% | 5.1 dB | big shifts 7.1% and 5.6 dB, high notes 21% |
| Narrow the peaks that get put back, 25% | 8.5 dB | 4.98% | 4.0 dB | big shifts flat 2.8 dB |
| Centre grains on the pulses | 7.6 dB | 4.98% | 4.2 dB | non-harmonic energy needed a slower offset, below |
| Slower pitch mark slide, from Smooth Harmony | 7.5 dB | 4.56% | 4.2 dB | worst output pitch 2.98 to 0.80 cents |
| Draw the model through harmonic peaks on high voices | 7.8 dB | 5.6% | 2.0 dB | colour 3.3 to 2.8 dB, level from a first trim, slowed later |
| Sharpening set per frequency, 45 to 50% | 6.9 dB | 4.98% | 1.45 dB | |
| Hold the pulse offset for the whole note, not re-found | 6.8 dB | 4.98% | 1.45 dB | stopped a repeating jump that had put Low Latency non-harmonic energy at -14.7 dB, now -46 dB |
| Slower grain reading when shifting down, whitened grains | 6.8 dB | 4.24% | 1.45 dB | |

High Quality also: voicing read 10 ms ahead, so a note is on pitch 98% of the time from 10 to 40 ms against 88% in Low Latency, and an S after a vowel is untouched from its first sample. Through consonants the true input takes over from the filters.

Low Latency gets the model too, with no added latency: flat 11.6 to 10.0 dB, formant move 4.98 to 4.43%.

Limits changed after measuring, with the reasons in `tests/PassLimits.h` and the README: high note formant move, big shift formant move, and how peak height is read on high notes and big shifts. Still failing: High Quality formant move 4.24% against 4%, and first formant on high notes 13.5 dB against 9 dB.

## 1.0.0

First version, started from a copy of Smooth Tune. Same tracker and grain shifter, with these added: per-note Tune, Remove and Bypass on top of any key and scale, MIDI notes as targets with sustain pedal and octave choice, Classic mode, two engines that hand over with a crossfade, Mix, a vibrato generator, and a custom window with a keyboard and a scrolling pitch view.

Baseline, measured before any quality work, in High Quality and Low Latency alike:

- First formant flatter than a real voice: 11.6 dB
- Formant move at +/-1 and +/-3 semitones: 4.98%
- Big shifts of +/-7 semitones: formant move 15.7%, first formant flatter 10.2 dB
- High notes around 440 and 660 Hz: formant move 45.1%, first formant flatter 27.8 dB
