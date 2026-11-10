# Choir Loft

Up to eight harmony voices from one lead vocal. Formants stay where they are, so the voices sound like people and not like a sped up tape. Each voice has its own timing, pitch, throat, tone, breath and consonants, so together they aim to sound like separate singers. The Tight preset turns all of that off and sits exactly on the notes.

**Load it:** Audio Units > Luke Garito > Choir Loft. AU code `aumf SHrm Luke`. It takes MIDI and goes in an audio effect slot. Mono, stereo, and mono in with stereo out. With mono in, the voices still spread across the stereo field.

Everything here was measured on synthetic voices and macOS `say`. That does not prove it sounds good on a real singer. Luke heard 1.0.0 on his voice, liked it, and found it robotic. 1.1.0 goes after that, but nobody has heard 1.1.0 yet, and the numbers below only show the causes moved. `demo/` has before and after files.

## First settings to try

- Scale Interval, your song's Key and Scale, Voices 2. That is the default: a third and a fifth above, 6 dB under the lead.
- Too loose? Pull Humanize and Drift down together, or load Tight for the exact 1.0.0 style tuning.
- MIDI Choir preset, then hold chords on the track's MIDI. Voices sing the chord, lead stays on top.

## Controls

Every control is a real AU parameter that a host can find by name.

| Control | What it does |
|---|---|
| Mode | Where voice pitch comes from. See below. |
| Key, Scale | Used by Scale Interval. Chromatic, Major, Minor, Harmonic Minor, Melodic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian. |
| Follow Key | Takes key and scale from `~/Library/Application Support/SmoothPlugins/key.json`, written by Smooth Key, on a background thread. Missing, broken, stale (over 10 minutes old), future dated or low confidence (under 0.5) data is ignored: the last good key holds, and with none it uses Key and Scale. |
| Voices | 1 to 4. Voices fade in and out. |
| More Voices | 0 to 4, added to Voices: Voices 4 and More Voices 4 is eight singers. Two controls because a host stores automation of Voices as a position in its range, and widening that range would have moved old automation. The window shows them as one row, 1 to 8. |
| Voice N On, Lead On | Whether you hear that singer. Off fades over 15 ms, and it stops using CPU while its note logic keeps going, so it comes back on the right note. Voices 1 to 8. |
| Voice N Interval | -24 to +24 semitones. Its value string says both: `+4 st (3rd up)`. Fixed Interval uses the semitones. Scale Interval uses the name, so +3 and +4 both mean a third, and the scale decides its size. |
| Voice N Level | -60 (Off) to +6 dB. |
| Voice N Pan | Constant power, centre at unity. Full left is +3 dB left, nothing right. |
| Voice N Drift | 0 to 100%. Slow pitch, timing and level wander, plus small grain to grain roughness. 100 is +/-20 cents, up to 40 ms behind the lead and about +/-1.5 dB. Each voice wanders on its own. |
| Voice N Throat | 70 to 140%. Above 100 lowers that voice's formants, as a longer throat would. Defaults differ per voice, about 20% between the extremes. |
| Voice N Tone | -100 to +100%. Darker or brighter, for good: a shelf at 1.6 kHz. Defaults differ per voice. |
| Humanize | How a person differs from a machine, note by note. A scoop from below (up to 70 cents, less when coming down), a slow settle (up to 18 cents), landing a little off the note (up to 8 cents at random plus up to 5 cents of the voice's own habit), standing behind the lead (up to 26 ms by voice plus up to 18 ms per note), a slower glide, a softer start to a phrase, its own vibrato depth and timing. Above 0 it also turns on period blending and high end unevenness. 0 is exact. |
| Vibrato | 0 to 100%, up to +/-60 cents. Starts 180 ms into a note. Each voice's rate is slightly different. |
| Vibrato Rate | 3 to 8 Hz. Voice 1 runs at this, the others up to 7% away. |
| Lead Level | -60 (Off) to +6 dB. |
| Lead Pan | Same law as voice pan. |
| Harmony Level | -60 (Off) to +12 dB, all voices together. |
| Mix | 0 is the delayed dry input, 100 is lead plus voices. |
| Glide | 0 to 500 ms between notes, as a time constant. |
| Follow Lead | 0 to 100%. In modes with a set note, how much of the lead's own pitch wander the voices copy. 0 sits on the note. Fixed Interval always copies all of it. With Humanize, some voices copy less than others. |
| Consonants | 0 to 100%. How much of the lead's unvoiced sound each voice carries. Those sounds are never pitch shifted. |
| Consonant Scatter | 0 to 100%. 0 passes a copy of the lead's consonant at the voice's lag. Above 0 each voice makes its own: a scatter of short random delays so it shares no waveform with the others, later (about 50 ms at most on top of the lag, by voice and by consonant), softer, and at a level picked per consonant. A long stretch without pitch is taken as a breath and fades out, since real singers do not all breathe together. |
| Breath | 0 to 100%. Noise of each voice's own, following that voice's high end. 100 is about 7 dB under the voice above 3 kHz. |
| Chord Root, Chord Type | Used by Chord Tones. |
| Chord Hold | Keeps a MIDI chord after the keys are up, until the next new note. The sustain pedal works too. |
| Input Type | Voice range. Sets tracking range and latency. Soprano 150 Hz to 1.3 kHz, Alto/Tenor 98 Hz to 1 kHz, Low Male 65 to 600 Hz, Wide Range 65 Hz to 1.3 kHz. |
| Tracking | Which sounds count as pitched. 0 is choosy, 100 lets rough, breathy sound count. |
| Reference A4 | 415 to 466 Hz. |
| Bypass | Keeps the same delay. |

Readings, which never write automation: Detected Pitch, Lead Note, Voice 1 to 8 Note, Voices Sounding, Active Key. An off singer reads no note and does not count as sounding. The window draws from them live. Hosts read them by name.

## The window

![The window with four voices](docs/window/four-voices@2x.png)

The top half shows what each voice sings and where it sits. On the left, each voice is a line through the last 8 seconds, the lead in warm white. Each line runs into its singer on the stage on the right.

- **Up and down** is the note, the same scale on both sides. Rows are the notes of your key, stronger on the root.
- **Left and right on the stage** is pan.
- **A singer's size** is its Level. The pale ring around it swells with how loud it is right now. A hollow ring means its Level is Off.
- **The thickness of a line** is how loud that moment was.
- **With nothing playing,** singers show where they would sit, so you can set up the voices before you sing.

**Click a singer to hear it or not,** on the stage or by its name on its card, so you can hear any 1, 2, 3 or more together. The lead works the same way, so you can hear only harmonies. Off singers and their lines dim. All, under the stage, brings everyone back.

Drag a singer: left and right pans it, up and down changes its interval. In the MIDI modes MIDI decides the note, so a drag only pans. A click with no movement switches, a drag moves. Two quick clicks are off and on again, so a double-click on a singer does not reset it. Double-click its controls for that.

Below the display, each voice has a card with its note, its interval in words, Level, Pan, Drift and Tone. Up to 4 voices there are 4 wide cards, with the spare ones dimmed. Above that there is a card per singer in narrower columns, so all of them stay in view. Under those are Humanize, Vibrato, Lead, Harmony and Mix. Advanced holds Throat, Vibrato Rate, Glide, Follow Lead, Breath, Consonants, Scatter, Lead Pan, Voice range, Tracking and Reference A4. When Advanced opens, the display gets shorter instead of the window getting bigger.

The Mode row says In Key where the parameter is called Scale Interval, and Fixed where it is Fixed Interval. Key and Scale swap with Chord and Type in Chord Tones mode, and Follow Key swaps with Chord Hold in the MIDI modes. Whichever pair is not by the display is in Advanced, so every control always has a place.

Every control is a real parameter with the same ID, name and range as before. Double-click resets, click a value to type it, the arrows on an interval step to the next interval, and Shift gives a finer drag. The size and whether Advanced is open are saved with the session, and nothing else was added to saved state. The line under the controls explains whatever is under the mouse.

`src/editor/`: `StageView` is the display, `Controls` the controls, `HarmonyEditor` the layout, `Theme` the colours. Font: Inter, SIL OFL, license in `assets/fonts/`. Pictures are in `docs/window/`: `build/RenderWindow_artefacts/Release/RenderWindow docs/window` draws them again from the `say` recordings in `tests/corpus`, which are not a singer.

The display reads a pitch and level history and never touches the sound. Two small additions feed it: the audio thread keeps the loudest sample of the lead and of each voice over each 5 ms and stores it in lock-free history. The `window` tests compare a bit for bit fingerprint of the output on 5 presets against the 1.1.0 source, with no window and with the window open and drawing. All are identical.

CPU, real window at 2x on this Mac: every run is in `docs/window-cpu.md`. `tests/AuViewCheck.mm` opens the installed window through the system 80 times, with and without audio.

## Modes

- **Scale Interval:** the lead's nearest scale note, then the Interval counted in scale steps. In C major a third on C is E, a major third, and a third on E is G, a minor third. In scales without seven notes it takes the scale note nearest that many semitones.
- **MIDI Chord:** held notes set the voices. No notes, no voices, and the lead passes exactly. More notes than voices: lowest and highest stay, then the newest. Fewer: spare voices rest. A held note keeps its voice, and on a change voices move the least in total. A note over two octaves from the lead moves by octaves to within reach.
- **Fixed Interval:** the lead's pitch plus the Interval in semitones, whatever the key.
- **Chord Tones:** the note of the set chord (Chord Root and Type) nearest lead plus Interval, no MIDI needed.
- **MIDI Channel:** voice N sings the newest note on MIDI channel N.

## Presets

Pop Thirds And Fifths, Third Above, Third Below, Full Stack, Gang Unison, Wide Octaves, Power Fifths, MIDI Choir, MIDI Chords Only (lead off), Chord Tones Pad, Tight. Six Part Stack, Eight Part Choir, MIDI Eight Part Choir. Presets never change Key, Scale, Follow Key or Input Type. They all put every singer back on.

Tight is exact tuning: no Drift, Humanize, Vibrato, Tone, Breath or Scatter, Throat 100, Follow Lead 0, Consonants 35. It meets the 1.0.0 limits. It is not a bit for bit copy of 1.0.0, because the vowel end fix below applies everywhere. For the 1.0.0 default sound itself, set Drift 25, Humanize 20, Vibrato 10, Throat 100, 96, 104, 92, Glide 20, Follow Lead 25, Consonants 35, Tone, Breath and Scatter 0.

## Level with many voices

Voices add level and there is no limiter. Up to 4 voices nothing changes. Above 4, the voices together turn down so 8 stay about as loud as 4, which is what unrelated voices add up to: -1.0 dB at 5, -1.8 at 6, -2.4 at 7, -3.0 at 8. Voices 5 to 8 also start 3 dB lower than the first four, at -9 dB, and the new presets turn the lead down a little.

On a vocal peaking at -6 dBFS, measured output peaks: two voices at defaults 0.64, Voices set to 8 at defaults 0.80, Six Part Stack 0.55, Eight Part Choir 0.48, MIDI Eight Part Choir 0.59. A vocal peaking near full scale can still pass 0 dBFS, as it could before. Turning singers off does not turn the rest up.

## Signal flow

Input > clean bad samples > history > pitch tracker (on left and right mixed) > note logic on a 1 ms tick > up to eight grain voices reading the shared mono history, each with its own consonant path > per voice all-pass, level match, Tone, Breath, fade, Level, Pan > summed with the delay matched lead > Mix > Bypass > output.

- `src/PitchTracker.h`: from Smooth Tune. Since 1.4.1 a start must also repeat in its even harmonics. Since 1.4.2 it must also repeat once its formants are taken out by linear prediction, unless plainly periodic, and reports how far back the start reaches so voices can be written back to it. Changes: marks slide much more slowly towards the loudest part of each period, because that slide bent shifted pitch by up to 8.6 cents. Since 1.4.0, once a note is under way, a move to two or three times the current period must also hold over three windows, so breathy sound no longer drops it an octave or a twelfth for a few ms.
- `src/VoiceShifter.h`: pitch-synchronous grains, one input period of window each side of a mark, spaced at the wanted output period. Timing lag comes from choosing an older period, so it cannot bend pitch. Unvoiced sound is read straight at one held offset, so it is the input at a lag, with no grain spacing on it. A slight reading speed change cancels the formant drag at large shifts. With Humanize above 0, a grain reads between two neighbouring periods instead of repeating or dropping whole ones, and where it reads is nudged at random per grain, so the high end stops repeating exactly while pitch stays put.
- `src/HarmonyBrain.h`: the modes, Glide, Follow Lead, and each voice's drift, vibrato and humanize. Each voice hears the lead late by its own lag, so its note changes come late by themselves. Since 1.4.0 a start is a phrase after silence, pitch coming back after 10 ms or more without it, or a reattack. In a start's first 50 ms plus the voice's lag, and after a note held under 80 ms, notes change without a glide; glides are for legato notes. While the lead rises out of a start, the voices bend with it. `src/MidiChord.h`: held notes and voice assignment. `src/Scales.h`: scales, chords, intervals. `src/KeyFollower.h`: the key file.
- `src/PluginProcessor.cpp`: parameters, latency, MIDI timing, the mix. Since 1.4.0: the first 30 ms of pitch of each stretch are rewritten with the newest reading; a reattack is the lead 9 dB louder than 20 to 30 ms before; the level match reads faster for 60 ms after a start. MIDI waits for the latency, so a chord change lines up with the lead. Pitch from the tracker is looked up in a short history for the same reason, and so is voicing, per voice: a voice keeps singing until the end of a vowel it is reading, not until the tracker has seen the vowel end, which in 1.0.0 cut each vowel short and left its last moments at the lead's pitch.
- `getReadings()` and `getPitchPoint()` give a later window everything without touching audio.

## Phrase starts (1.4.0)

Luke heard 1.3.1 miss vocal starts on his own vocals. `tests/OnsetTests.h` makes the starts a singer makes, at every Input Type and three pitches in its range, with the two default voices on their own channels: 432 voice starts per preset. A voice has sounded once it is within 50 cents of its note and within 3 dB of its own steady level against the lead, held 20 ms. Limits are in `tests/PassLimits.h`, committed before 1.3.1 was measured on them (commit d336daf).

| Kind of start | Default 1.3.1 | Default 1.4.0 | Tight 1.3.1 | Tight 1.4.0 |
|---|---|---|---|---|
| Hard start | 4 wrong, 148 ms | 0, 26 ms | 4, 82 ms | 0, 28 ms |
| Soft start | 6, 108 ms | 0, on time | 6, 64 ms | 0, on time |
| Scoop of 2 semitones | 20, 77 ms | 0, 8 ms | 14, 18 ms | 0, on time |
| Breathy start | 18, 288 ms | 2, 74 ms | 14, 168 ms | 0, 76 ms |
| Creaky start | 12, 133 ms | 0, 17 ms | 9, 88 ms | 0, 18 ms |
| s into the vowel | 4, 115 ms | 0, 24 ms | 3, 76 ms | 0, 30 ms |
| t into the vowel | 4, 146 ms | 0, 26 ms | 4, 82 ms | 0, 28 ms |
| Vibrato at once | 5, 129 ms | 0, 22 ms | 6, 82 ms | 1, 26 ms |
| After a 100 ms gap | 4, 145 ms | 0, 19 ms | 4, 82 ms | 0, 20 ms |
| Room reverb | 28, 148 ms | 0, 27 ms | 26, 134 ms | 0, 28 ms |
| Bright take | 2, 50 ms | 0, 19 ms | 2, 38 ms | 0, 26 ms |
| All 17 kinds | 115 wrong | 2 wrong | 100 wrong | 1 wrong |

Each cell: voices that sang another note first, then the 95th percentile time to sound past the voice's own lag (limit 40 ms). m and l into the vowel and first notes of 150 ms were already fine. Humanize 0: 111 wrong to 0, worst kind 252 to 73 ms. No voice skipped a first note of 150 ms or longer, before or after.

The `say` corpus, every voiced stretch of 150 ms or longer: every voice sounds in all 58, median 21 ms past the lag (1.3.1: 21 ms). Its 90th percentile went from 37 to 57 ms, because voices now bend with a rising start, and `say` speech rises a lot; a bent voice is not counted until it sits within 50 cents of a scale note.

1.4.1: a start must also repeat in its even harmonics (the sound plus itself half a period later), which a lone formant's ring does not. Breaths through a closed vowel no longer sing; the worst breath, through an open vowel, sings 29.8% of the time, longest 70 ms (1.4.0: 54.5%, 190 ms). Wrong first notes at the default: 2 to 1. Hard starts 95th percentile 26 to 17 ms. `say` median 21.7 ms. An open vowel's two formants ring like the 2nd and 3rd harmonics of a note between them, and only a 30 ms periodicity check told them apart; it made starts after a consonant or breath 10 to 18 ms later, so it was left out.

1.4.2: a start must also repeat once its formants are taken out (linear prediction, then whether what is left repeats a period later), unless the sound is plainly periodic or has stayed clean for 10 ms. A start is written back to where nearly periodic sound began, so voices standing behind the lead still come in on time. Breaths, s, sh, noise and room tone: 0 singing (limit 1%). Wrong first notes 0 at every preset tested. On Luke's held out takes breath leak fell from 12.0% to 3.1% and starts came in sooner, with 1 and 4 more wrong first notes at phrase and word starts. Breathy high starts still come in 67 to 72 ms late against 40: the aspiration matches the voice for its first 50 ms.

Still failing: breathy starts on high notes, where the aspiration noise goes through the vowel's own formants and the tracker reads an octave high for about 50 ms (74 to 76 ms against 40). Breaths through a closed vowel at -30 dBFS make a voice sing 54.5% of the time against a limit of 1%, longest 190 ms (1.3.1: 55.1%, 190 ms): a narrow formant rings like a note. Two fixes were tried and dropped: a check that a start repeats at twice its period (it delayed `say` starts, median 22 to 34 ms) and whitening the tracker's input (`say` starts slower, 2 of 58 missed).

`PluginTest onsets`, `PluginTest notrigger`; `PluginTest onsettrace`, `trackertrace` and `corpustrace` print one start frame by frame.

## Latency

Exact and reported, the same bypassed. At 48 kHz: Soprano 9.0 ms, Alto/Tenor 13.7 ms (default), Low Male and Wide Range 20.6 ms. The lead and dry path are delayed to match. Changing Input Type fades out, jumps and fades in over 4 ms, then tells the host.

## 1.1.0 against 1.0.0

`tests/HumanMetrics.h` was built against the 1.0.0 source (tag `v1.0.0`) and the current code, each at its own defaults. 1.0.0's numbers are in `tests/human-before-1.0.txt`, the current ones are the METRIC lines in `tests/results.txt`. Limits were set after measuring 1.0.0 and before measuring the new code.

| Cause | Measure | 1.0.0 | 1.1.0 |
|---|---|---|---|
| Too perfect | Voices arrive after the lead on a note change, average | 13 ms | 42 ms |
| | Earliest to latest voice on a note change, median | 7 ms | 17 ms |
| | Pitch moves in common, most alike pair, lead with vibrato | 0.68 | 0.23 |
| | Distance from target, body of a note, median, on purpose | 2.7 cents | 3.5 cents |
| | Same, 95th percentile | 7.0 cents | 10.0 cents |
| | Voices settle apart, highest average minus lowest | 1.1 cents | 4.4 cents |
| Same throat | Widest formant gap between two voices | 11.8% | 22.1% |
| | Widest brightness gap between two voices | 4.5 dB | 7.0 dB |
| Same words | When an S starts, spread across voices | 4 ms | 30 ms |
| | Two voices' S alike at the best lag | 0.85 | 0.05 |
| | A voice's S against the lead, above 4.5 kHz | -9.8 dB | -20.3 dB |
| Metallic | First formant flatter, worst case, the set that read 11.6 dB in 1.0.0 | 11.6 dB | 11.5 dB |
| | First formant flatter, +7, worst of 6 vowels at 3 pitches, up to a 220 Hz voice | 25.0 dB | 16.3 dB |
| | First formant flatter, -7, worst of the same | 10.7 dB | 10.7 dB |
| | First formant flatter, +7, median of open vowels | 0.2 dB | 0.2 dB |
| | Energy between harmonics at +7, lead's own is -29.7 dB | -32.0 dB | -34.1 dB |
| | Same at -7 | -28.2 dB | -29.5 dB |
| | Same at +12 | -31.7 dB | -35.3 dB |
| | High end period to period sameness at +7, lead's own is 0.72 | 0.82 | 0.79 |

- Tight: 0.46 cents at the 95th percentile, voices 4 ms apart, S level -9.2 dB, within every 1.0.0 limit.
- Latency unchanged. CPU about 1.7% of one core at 4 voices.
- No clear change: two voices on one interval at the best lag. Voices 1 and 2 over 8 s read 0.27 before and 0.40 now, while the worst of three pairs over 10 s read 0.53 before and 0.27 now. This reading swings with the window, so it says little either way. All under the 0.75 limit.
- Three limits changed, with reasons in `tests/PassLimits.h`. Humanize 100's note body went from 10 to 20 cents, since it now lands off target on purpose. The grain pattern limit at -7 was dropped because it asked for less than the test lead itself contains. The real material level limit now runs on Tight, since at defaults voices run on behind a lead that has stopped, which reads 9.0 dB.

## Measured

All in `tests/results.txt`: 563 pass, 3 fail as of 1.4.2, the 3 the breathy high starts in the phrase start test (see Phrase starts). Pitch, formant, level, pan, click and timing figures below are on plain settings, which is what Tight gives. Limits are in `tests/PassLimits.h`, committed before any DSP existed. Run: `build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt`.

- Voice pitch against target, 95th percentile, 24 keys, every degree, 1056 notes: Scale Interval 2.3 cents, Fixed Interval 1.8, MIDI Chord 0.03. Limit 5.
- Scale intervals: every seven note scale, root, degree and interval, all right. Chord assignment, Chord Hold, pedal and channels: all right, no voice crossings in 2000 random chords.
- Formants at +/-7 semitones: worst move 4.98% on the voices used for tuning, 3.1% on others, against a 5% limit. Thin margin. See below. A wider set, with a voice around 220 Hz and the closed vowels, moves up to 9.9%, which the tool cannot read well.
- Level: within 0.001 dB. A voice at 0 dB against the lead at +/-7: within 0.11 dB, thanks to the level match.
- Pan: law within 0.001 dB, far channel silent at full pan.
- Drift 100: 19.2 cents, timing spread 33 ms, typically 19 ms behind the lead, level 2.9 dB peak to peak. Drift 0: 0.5 cents, 0.8 ms. Voices wander independently.
- Two voices on the same interval at defaults: see `two voices on one interval` lines in `tests/results.txt`, against a limit of 0.75 at the best lag and 4.5 dB of level rise (6 is a copy, 3 is unrelated).
- Clicks, against steady voices, biggest step: see `no click` lines in `tests/results.txt`. Limit 2 times.
- Consonants: an S through a voice keeps its shape within 0.2 dB, with no added repeat strength (0.009 at most). Real S and T sounds in three `say` voices: no rise. Level at Consonants 35 is -9.1 dB within 1 dB.
- MIDI mode with no notes: lead passes exactly, under -100 dB against the delayed input.
- Chord change timing against the lead: 4.3 ms. Voice onset against the lead, unison on a clean start: see `starts with the lead's onset` in `tests/results.txt`. Phrase starts: see above.
- Vibrato depth within 1%, Glide within 2 ms, Throat 130 and 75 within 1%, Follow Lead within 0.01 cents.
- Real speech, fixed fifth: median distance from lead plus a fifth 1.3 to 16.5 cents against a two sided reference, which is not ground truth.
- CPU: about 1.7% of one core at 4 voices.

## What the numbers do not say

- Formants are the weak spot of any grain shifter at large shifts. Positions hold, but peaks get flatter on closed vowels with one narrow low formant, such as /i/ and /u/. Period blending helped only the higher voices: the set behind the old 11.6 dB figure barely moved, to 11.5 dB, -7 did not move, and open vowels were already fine at about 0.2 dB. Cause 4 is mostly not solved. The input's harmonics may simply miss a narrow formant, so nothing causal can put it back. Grain window sizes and shapes were tried again at +7 and did not help.
- Smooth Throat's engine was not used: 32 ms per voice, more than the whole plug-in's latency, and it does not repair flattening by itself.
- Whether any of this reads as separate people is a question for ears. These numbers show lateness, spread and difference, not character.
- The reading speed correction was tuned on the test voices, hence the thin margin. Smooth Tune's values, tuned at +/-3, put formants 10% low at +/-7. Moves at other shifts: +3 1.7%, -3 0.9%, +5 2.1%, -5 1.3%, +12 5.2%, -12 3.3%, a smaller set of voices. `say` voices read 0 to 3% low at +/-7, a rough reading.
- Voices are built from the mono mix of a stereo input.
- Voices add level. Adding them to a lead near full scale can pass 0 dBFS, which the plug-in does not limit.
- Phrase starts were measured on synthetic voices and `say`, not on a real singer. Whether 1.4.0 fixes what Luke heard needs his ears on his own vocals in Logic.

## Demo files

`demo/`, stereo 16 bit 48 kHz, all rendered at the same gain of -4.4 dB, Wide Range, Key C Major:

- `melody_*`: a synthetic sung phrase with S and T sounds and a breath, in C major. `sung_*`: macOS `say` Good News voice. `speech_*`: macOS `say` Samantha. None of them is a real singer, and `say` is not in C major.
- `*_lead_alone`: the input. `*_two_voices_before_1.0` and `*_four_voices_before_1.0`: the 1.0.0 source, its presets 1 and 4. `*_two_voices_after_1.1`, `*_four_voices_after_1.1`: this version, the same preset slots. `*_tight_after_1.1`: the Tight preset.

`build/PluginTest_artefacts/Release/PluginTest demo`, run from the repo, writes the after files again.
