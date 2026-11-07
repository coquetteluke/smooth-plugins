# Smooth Harmony

Up to four harmony voices from one lead vocal. Formants stay where they are, so the voices sound like people and not like a sped up tape. Each voice has its own timing, pitch wander, vibrato, throat and waveform, so together they sound like separate singers.

**Load it:** Audio Units > Luke Garito > Smooth Harmony. AU code `aumf SHrm Luke`. It takes MIDI and goes in an audio effect slot. Mono, stereo, and mono in with stereo out. With mono in, the voices still spread across the stereo field.

Everything here was measured on synthetic voices and macOS `say`. That does not prove it sounds good on a real singer. It has not been heard by anyone yet.

## First settings to try

- Scale Interval, your song's Key and Scale, Voices 2. That is the default: a third and a fifth above, 6 dB under the lead.
- MIDI Choir preset, then hold chords on the track's MIDI. Voices sing the chord, lead stays on top.

## Controls

Every control is a real AU parameter that a host can find by name.

| Control | What it does |
|---|---|
| Mode | Where voice pitch comes from. See below. |
| Key, Scale | Used by Scale Interval. Chromatic, Major, Minor, Harmonic Minor, Melodic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian. |
| Follow Key | Takes key and scale from `~/Library/Application Support/SmoothPlugins/key.json`, written by Smooth Key, on a background thread. Missing, broken, stale (over 10 minutes old), future dated or low confidence (under 0.5) data is ignored: the last good key holds, and with none it uses Key and Scale. |
| Voices | 1 to 4. Voices fade in and out. |
| Voice N Interval | -24 to +24 semitones. Its value string says both: `+4 st (3rd up)`. Fixed Interval uses the semitones. Scale Interval uses the name, so +3 and +4 both mean a third, and the scale decides its size. |
| Voice N Level | -60 (Off) to +6 dB. |
| Voice N Pan | Constant power, centre at unity. Full left is +3 dB left, nothing right. |
| Voice N Drift | 0 to 100%. Slow pitch and timing wander, plus small grain to grain roughness. 100 is +/-20 cents and up to 40 ms behind the lead. Each voice wanders on its own. |
| Voice N Throat | 70 to 140%. Above 100 lowers that voice's formants, as a longer throat would. Defaults differ per voice. |
| Humanize | How each note starts: a scoop from below, a small random detune and a late entrance. 100 is up to 70 cents, 8 cents and 18 ms. |
| Vibrato | 0 to 100%, up to +/-60 cents. Starts 180 ms into a note. Each voice's rate is slightly different. |
| Vibrato Rate | 3 to 8 Hz. Voice 1 runs at this, the others up to 7% away. |
| Lead Level | -60 (Off) to +6 dB. |
| Lead Pan | Same law as voice pan. |
| Harmony Level | -60 (Off) to +12 dB, all voices together. |
| Mix | 0 is the delayed dry input, 100 is lead plus voices. |
| Glide | 0 to 500 ms between notes, as a time constant. |
| Follow Lead | 0 to 100%. In modes with a set note, how much of the lead's own pitch wander the voices copy. 0 sits on the note. Fixed Interval always copies all of it. |
| Consonants | 0 to 100%. How much of the lead's unvoiced sound each voice carries. Those sounds are never pitch shifted. |
| Chord Root, Chord Type | Used by Chord Tones. |
| Chord Hold | Keeps a MIDI chord after the keys are up, until the next new note. The sustain pedal works too. |
| Input Type | Voice range. Sets tracking range and latency. Soprano 150 Hz to 1.3 kHz, Alto/Tenor 98 Hz to 1 kHz, Low Male 65 to 600 Hz, Wide Range 65 Hz to 1.3 kHz. |
| Tracking | Which sounds count as pitched. 0 is choosy, 100 lets rough, breathy sound count. |
| Reference A4 | 415 to 466 Hz. |
| Bypass | Keeps the same delay. |

Readings, which never write automation: Detected Pitch, Lead Note, Voice 1 to 4 Note, Voices Sounding, Active Key. The generic window does not redraw them live. Hosts read them by name.

## Modes

- **Scale Interval:** the lead's nearest scale note, then the Interval counted in scale steps. In C major a third on C is E, a major third, and a third on E is G, a minor third. In scales without seven notes it takes the scale note nearest that many semitones.
- **MIDI Chord:** held notes set the voices. No notes, no voices, and the lead passes exactly. More notes than voices: lowest and highest stay, then the newest. Fewer: spare voices rest. A held note keeps its voice, and on a change voices move the least in total. A note over two octaves from the lead moves by octaves to within reach.
- **Fixed Interval:** the lead's pitch plus the Interval in semitones, whatever the key.
- **Chord Tones:** the note of the set chord (Chord Root and Type) nearest lead plus Interval, no MIDI needed.
- **MIDI Channel:** voice N sings the newest note on MIDI channel N.

## Presets

Pop Thirds And Fifths, Third Above, Third Below, Full Stack, Gang Unison, Wide Octaves, Power Fifths, MIDI Choir, MIDI Chords Only (lead off), Chord Tones Pad. Presets never change Key, Scale, Follow Key or Input Type.

## Signal flow

Input > clean bad samples > history > pitch tracker (on left and right mixed) > note logic on a 1 ms tick > four grain voices reading the shared mono history > per voice all-pass, level match, fade, Level, Pan > summed with the delay matched lead > Mix > Bypass > output.

- `src/PitchTracker.h`: from Smooth Tune. One change: marks slide much more slowly towards the loudest part of each period, because that slide bent shifted pitch by up to 8.6 cents.
- `src/VoiceShifter.h`: pitch-synchronous grains, one input period of window each side of a mark, spaced at the wanted output period. Timing lag comes from choosing an older period, so it cannot bend pitch. Unvoiced sound is read straight at one held offset, so it is the input at a lag, with no grain spacing on it. A slight reading speed change cancels the formant drag at large shifts.
- `src/HarmonyBrain.h`: the modes, Glide, Follow Lead, and each voice's drift, vibrato and humanize. `src/MidiChord.h`: held notes and voice assignment. `src/Scales.h`: scales, chords, intervals. `src/KeyFollower.h`: the key file.
- `src/PluginProcessor.cpp`: parameters, latency, MIDI timing, the mix. MIDI waits for the latency, so a chord change lines up with the lead. Pitch from the tracker is looked up in a short history for the same reason.
- `getReadings()` and `getPitchPoint()` give a later window everything without touching audio.

## Latency

Exact and reported, the same bypassed. At 48 kHz: Soprano 9.0 ms, Alto/Tenor 13.7 ms (default), Low Male and Wide Range 20.6 ms. The lead and dry path are delayed to match. Changing Input Type fades out, jumps and fades in over 4 ms, then tells the host.

## Measured

All in `tests/results.txt`, 256 checks. Limits are in `tests/PassLimits.h`, committed before any DSP existed. Run: `build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt`.

- Voice pitch against target, 95th percentile, 24 keys, every degree, 1056 notes: Scale Interval 2.3 cents, Fixed Interval 1.8, MIDI Chord 0.03. Limit 5.
- Scale intervals: every seven note scale, root, degree and interval, all right. Chord assignment, Chord Hold, pedal and channels: all right, no voice crossings in 2000 random chords.
- Formants at +/-7 semitones: worst move 4.98% on the voices used for tuning, 3.1% on others, against a 5% limit. Thin margin. See below.
- Level: within 0.001 dB. A voice at 0 dB against the lead at +/-7: within 0.11 dB, thanks to the level match.
- Pan: law within 0.001 dB, far channel silent at full pan.
- Drift 100: 19.2 cents, timing spread 33 ms, typically 19 ms behind the lead. Drift 25, the default: 4.9 cents, 9.3 ms. Drift 0: 0.5 cents, 0.8 ms. Voices wander independently.
- Two voices on the same interval at defaults, steady lead: best lag correlation 0.53, adding the second raises level 3.3 dB (6 is a copy, 3 is unrelated). On `say` material 0.61 and 3.2 dB. With Drift, Humanize and Vibrato at 0, best lag correlation stays high at 0.93: those controls are what separate them.
- Clicks, against steady voices, biggest step: chord changes 1.2 times, voice count jumps 1.4, lead moving between notes 1.8. Limit 2.
- Consonants: an S through a voice keeps its shape within 0.2 dB, with no added repeat strength (0.009 at most). Real S and T sounds in three `say` voices: no rise. Level at Consonants 35 is -9.1 dB within 1 dB.
- MIDI mode with no notes: lead passes exactly, under -100 dB against the delayed input.
- Chord change timing against the lead: 4.3 ms. Voice onset against the lead: 2.4 ms.
- Vibrato depth within 1%, Glide within 2 ms, Throat 130 and 75 within 1%, Follow Lead within 0.01 cents.
- Real speech, fixed fifth: median distance from lead plus a fifth 1.3 to 16.5 cents against a two sided reference, which is not ground truth.
- CPU: about 1% of one core at 4 voices.

## What the numbers do not say

- Formants are the weak spot of any grain shifter at large shifts. Positions hold, but peaks get flatter: the first formant reads up to 11.6 dB flatter than a real voice at that pitch, worst case at +7. Going up sounds thinner than a real higher singer would, and only ears can say how much that matters.
- The reading speed correction was tuned on the test voices, hence the thin margin. Smooth Tune's values, tuned at +/-3, put formants 10% low at +/-7. Moves at other shifts: +3 1.7%, -3 0.9%, +5 2.1%, -5 1.3%, +12 5.2%, -12 3.3%, a smaller set of voices. `say` voices read 0 to 3% low at +/-7, a rough reading.
- Voices are built from the mono mix of a stereo input.
- Voices add level. Adding them to a lead near full scale can pass 0 dBFS, which the plug-in does not limit.

`build/PluginTest_artefacts/Release/PluginTest demo`, run from the repo, writes harmonised `say` files to `build/demo` for a quick listen outside any host.
