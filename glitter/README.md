# Glitter

A fast creative vocal tuner with effect modules. Hard tuning by default, held MIDI notes can set the pitch, and eleven modules with an On switch each, in a fixed order.

**Load it:** Audio Units > Luke Garito > Glitter. AU code `aumf STfx Luke`. It takes MIDI and goes in an audio effect slot. Mono, stereo, and mono in with stereo out. To play it from MIDI, send a track's MIDI to it with the slot's side chain menu.

Measured on synthetic voices, clean tones and macOS `say`, offline, and since 1.2.0 on Luke's real takes (see Real takes below).

## Signal flow

The order is fixed.

1. **Input** > pitch tracker (left and right mixed) > corrector. Key, Scale, Follow Key or held MIDI notes set the target. Retune Speed and Humanize set how it gets there.
2. **Tune:** a grain shifter (Shifter A) that only ever tunes.
3. **Pitch, Motion, Throat:** a second grain shifter (Shifter B) that tunes and also shifts. With all three off, the lead is Shifter A alone. Turning one on crossfades to B over 15 ms.
4. **Harmony and Doubler:** extra grain shifters running in parallel, working from the lead's pitch, mixed with the lead into stereo. They keep 30% of a consonant, so an S does not stack up.
5. **Vocoder tone:** a synth at the lead's pitch, shaped by the lead's spectrum, added under the voice.
6. **Distortion**, then **Filter**, then **Ring**, then **Delay**, then **Reverb**.
7. **Output Gain**, Bypass.

An off module is left out of the signal altogether. All the shifters keep running while off, so turning one on finds it already at the right pitch.

## Controls

Every control is a real AU parameter that a host can find by name.

| Tuning | What it does |
|---|---|
| Key, Scale | Root and scale: Chromatic, Major, Minor, Harmonic Minor, Melodic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian, Custom. |
| Retune Speed | Time constant of the correction, 0 to 400 ms. Default 0 reads Instant: hard tuning. |
| Humanize | Slows correction on the sustained part of a note, so a vibrato survives while the start of the note stays quick. Default 0. |
| Input Type | Voice range, which sets the latency: Soprano 150 Hz up, Alto/Tenor 98 Hz up (and follows notes down to 70 Hz at the same latency), Low Male 65 to 600 Hz, Wide Range 65 Hz up. |
| Follow Key | Takes key and scale from `~/Library/Application Support/SmoothPlugins/key.json`, written by Smooth Key, on a background thread. Missing, broken, stale, future dated or low confidence data is ignored: the last good key holds, and with none it uses Key and Scale. A Custom scale stays. |
| MIDI Target | Off, As Played, Any Octave. As Played: the nearest held note in the octave it was played, however far away, so one held note sets the pitch. Any Octave: the nearest held pitch class. With no notes held, the scale rules. |
| Reference A4 | 415 to 466 Hz. |
| Tracking | Which sounds count as pitched. 0 is choosy, 100 tunes rough sound too. |
| Custom C to Custom B | With Scale on Custom: Tune, Remove or Bypass. |

| Module | Controls |
|---|---|
| Pitch | Pitch On, Pitch Octave (-2 to +2), Pitch Semitones (-12 to +12), Pitch Moves Formants (off holds the voice's character, on gives the chipmunk or monster sound). |
| Throat | Throat On, Throat Length (60 to 150%. Above 100 lowers the formants). Only voiced sound changes. |
| Motion | Motion On, Motion Pattern (Trill Up, Trill Down, Octave Up, Octave Down, Arp Up, Arp Down, Stairs, Ostinato), Motion Rate (1/4 to 1/32), Motion Glide (0 to 200 ms). Walks the note through a 16 step pattern, in scale steps, tied to the song position. |
| Harmony | Harmony On, Harmony Voices (One or Two), Harmony Interval 1 and 2 (-24 to +24), Harmony In Key (moves an interval to the nearest scale note), Harmony Level, Harmony Width. |
| Doubler | Doubler On, Doubler Level, Doubler Timing (5 to 60 ms, wanders slowly), Doubler Detune (0 to 50 cents, one up, one down), Doubler Width. |
| Vocoder | Vocoder On, Vocoder Mix (voice stays whole up to 50, tone reaches full at 50, voice gone at 100), Vocoder Octave (-2 to +1), Vocoder Wave (Saw, Square, Super). 16 bands. Follows the output pitch, so it follows MIDI too. |
| Distortion | Distortion On, Distortion Type (Tube, Clip, Fold), Distortion Drive, Distortion Tone, Distortion Mix. |
| Filter | Filter On, Filter Type (High Pass, Band Pass, Low Pass), Filter Cutoff, Filter Resonance, Filter Sweep Depth (0 to 4 octaves), Filter Sweep Rate (4 Bars to 1/16). |
| Ring | Ring On, Ring Frequency (20 Hz to 5 kHz), Ring Mix. |
| Delay | Delay On, Delay Time (1/32 to 1/1, with dotted and triplet values), Delay Feedback, Delay Tone, Delay Mix, Delay Ping Pong. |
| Reverb | Reverb On, Reverb Type (Plate, Room, Hall), Reverb Decay, Reverb Pre-Delay, Reverb Mix. |

Also Output Gain and Bypass. Tempo comes from the host, and falls back to 120 BPM with none.

Readings, which never write automation: Pitch Off Target, Detected Pitch, Correction Applied, Target Note, Active Key, MIDI Notes Held. The window shows them live.

## Presets

Hard Tune, Robot Stack, Octave Monster, Chipmunk Hook, Wide Double, Power Harmony, Phone Line, Filter Riser, Dub Throw, Melody Machine, Alien Ring, Synth Voice. They set Retune Speed, Humanize, every module and Output Gain. They leave Key, Scale, Input Type, Follow Key, MIDI Target, Reference and Tracking alone, so they never fight the song.

## Latency

Exact and reported, the same bypassed, and no module has any say in it. At 48 kHz: Soprano 433 samples (9.02 ms), Alto/Tenor 659 (13.73 ms), Low Male and Wide Range 990 (20.62 ms). The lowest note in the range sets it. Changing Input Type fades out, jumps and fades in over 4 ms, then tells the host.

With Distortion on, its anti-aliasing puts the wet signal half a sample later, because of how it averages each sample step. So the direct voice measures 660 samples instead of 659 with all on. The reported number stays 659.

Held MIDI notes and Motion and the Filter sweep are delayed by the same latency, so they line up with the delayed voice.

## Measured

All in `tests/results.txt`. Limits are in `tests/PassLimits.h`, committed before the DSP. Run: `build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt`.

- Tuning at default settings: 0.7 cents worst, 110 Hz to 988 Hz, 95th percentile. 1.5 cents with vibrato and breath. Limit 5.
- Sweep of A2 to E5 on five vowels, worst 95th percentile: default settings 1.0 cents, Pitch +7, -5 and +12 1.3 cents, Harmony +7 1.2 cents, held MIDI note 1.1 cents.
- `say` material at default settings: see `tests/results.txt` for each file. Median distance from a note goes from 6 to 27 cents before, down to 10 cents or less on speech and 2.5 or less on sung and held material.
- MIDI target: 0.02 cents on a clean voice, 1.2 cents with vibrato and breath. Pitch reaches a new note 6 ms from its place in the delayed audio.
- Follow Key: 10 kinds of bad file ignored, good file used, last good held, off goes back to manual, the thread picks it up by itself, a held note wins over a followed key.
- Retune Speed 25, 50, 100, 200 ms measures 22, 48, 97, 197 ms. Humanize 100 keeps 99% of a vibrato, default leaves 1%.

| Module | Off | Main control |
|---|---|---|
| Pitch | stays on A3 | Octave and Semitones on the wanted note within 0.02 cents. Formants held within 3.7% at +/-1 octave, 3.3% at +/-5 semitones |
| Throat | formants stay, 1.00 | Length 130 moves formants by 0.78 (0.77 exact), Length 75 by 1.35 (1.33 exact) |
| Motion | no steps | Trill on pitch within 0.01 cents, steps land within 6.5 ms |
| Harmony | -147 dB at the interval | +7 lands on E4 within 0.5 cents, Level -6 measures -6.0 dB on a voice |
| Doubler | -107 dB at the detune | Detune 30 reads 28.5 and 30 cents, Timing 30 ms reads 30 ms, Level -6 reads -5.9 dB |
| Vocoder | 88 dB less at the sub | Octave -1 adds a tone at 110 Hz, Mix 100 on pitch within 0.03 cents, level within 0.01 dB of the voice |
| Distortion | exact null | THD 1.5, 4.2, 10.5, 24.5, 38% across Drive. Non-harmonic at 5.2 kHz: -55 dB |
| Filter | exact null | -3.02 dB at the cutoff, -24 dB two octaves out, Sweep swings 24 dB and repeats every 1.02 s for a 1 s setting |
| Ring | exact null | Sidebands equal within 0.00 dB, input gone at Mix 100 |
| Delay | -240 dB | Quarter note reads 499.98 ms, Feedback 50 falls 6.14 dB a repeat, bounded at Feedback 95 |
| Reverb | -240 dB | RT60 0.88 to 0.92 s for 1 s, 2.52 to 2.60 s for 3 s (reads a bit short, inside the 20% limit) |

- **Null:** every module off with its controls thrown to the far ends, every module on then off, and all of them on then off, all give a difference of -240 dB, which means exact zero, once the 15 ms fade is done.
- **No clicks:** biggest sample step while switching stays within 1.5 times the steady step on a low sine. Every control jumping end to end with all modules on stays under 0.30.
- **Latency:** the measured delay equals the reported one at every rate and Input Type, and does not move with any module.
- **Same output** with a playhead or none, at any block size, after `prepareToPlay` again, and after saving and loading state.
- **CPU:** see the last lines of `tests/results.txt`. About 3% of one core with all on, about 2% with all off, on a machine busy with other builds.

## The tuner, and what it shares

The tracker, corrector and grain shifter are the same copy of Smooth Tune that Tune and Tune Pro started from. Marks slide towards the loudest part of each period at 0.2% instead of 1% (that slide bends a shifted voice's pitch: the 1% gave 4.7 cents at default settings and 5.5 cents through Pitch, over the 5 cent limit; Smooth Harmony found this first), a grain's read speed stops growing past +3 semitones, and held MIDI notes can be the targets.

Since 1.2.0 it has the fixes Tune Pro found on Luke's real takes, as Tune 1.2.0 has them: Alto/Tenor follows notes down to 70 Hz, the pitch control is read in line with the audio (1.80 periods back), an uncorrected voice comes out exactly as the input at the reported delay, a held note keeps its target through a waver across the line (and a slow slide lands within 110 ms), and grains do not read ahead for 10 ms after a pause. Tests: `PluginTest rode` (low, timing, hold, onset, entry). Glitter has no vocal tract model, so Tune Pro's own formant work does not apply here.

## Real takes

Luke's Rode NT1 takes (2026-11-10), checked with realvox (`_shared/realvox/src/extra/tune-fx.cpp`), the song's real key at A440. Numbers and what is still open are in `~/smooth-plugins/_vox/rode/work/tune-fx/REPORT.md`. What they changed in 1.2.0: the tuner fixes above, Reverb switched on while playing decays again, the Vocoder's level match no longer bursts after an S, a pause, a switch on or on the first note (1.2.2), and Distortion holds the level of the voice going in (its make-up suited a -18 dBFS voice; quieter ones came out up to 6.8 dB louder), from the first 20 ms after it is switched on (1.2.1).

## What the numbers do not say

- The tone tests for pitch use clean tones or steady synthetic voices. Real singing is rougher, and hard tuning on real consonants is untested by ear.
- Grain shifters flatten formant peaks at big shifts: Smooth Tune measured that at small shifts, and it will be stronger at octaves. Positions hold, but it will sound less like the singer. The Throat module uses the same grain reading speed, not a proper formant engine.
- The level match on Pitch and Harmony (slow, within +/-6 dB) is why those read so exact on a held note. Harmonics landing on formants moved shifted voices by up to 3.4 dB without it. How it behaves on fast phrases needs ears.
- The vocoder is one filter bank, not models of old machines.
- Harmony and Level were first read from a clean tone, which turned out to be the wrong tool: a sine loses level when grains move its one peak. Those tests moved to a voice, with the same limit.

## Window

Black, with hot magenta for the voice and one colour per module. PNGs are in `docs/window/`.

- **Top:** Retune Speed and Humanize set large, the live pitch strip, and the note coming out. On the strip, white is your voice, magenta is what comes out, and Harmony's voices are in Harmony's colour. Pitch and Motion show up as the magenta line moving away from the white one. A flat piano on the right lights the scale and the note.
- **Middle:** eleven blocks in signal order. A lit block is on. Each says what it is set to, so the row reads at a glance. A click on a block shows its controls, and a click on its switch turns it on or off.
- **Bottom:** one panel, in the colour of the block it belongs to, with that module's name, a picture drawn from its real settings, and all of its controls. On most modules the picture is a pad, where a dot moves two controls together. Filter's picture is the real response, Distortion's the real curve, Motion's the real pattern in your key, with the step playing right now in white.
- **Advanced:** rises over the panel. Input Type, MIDI Target, Reference A4, Tracking, and a one octave keyboard for Custom C to Custom B. A click on a key goes round Tune, Remove and Bypass. Picking a module closes it.
- Double-click resets, click a value to type it (2.5k works), drag the corner to resize. The size, whether Advanced is open and which module is showing are saved with the session. Nothing else was added to saved state.

Every control is the same real AU parameter as before, with the same ID, name and range, so hosts still find them all by name. The window only reads and writes those parameters, inside begin and end gestures. No control lives only in the window, and the tests check that every parameter has its place.

Added for the display, all plain relaxed stores from the audio thread that the audio never reads back: Harmony's two notes in the pitch history, Motion's step, the swept Filter cutoff, and the scale in use. Bypassed, the history shows the voice unchanged, because that is what comes out. Proof it changes no audio: all 12 presets rendered on `say` speech are byte for byte the same files as the build before the window, and all on with the window open and drawing against no window is exact zero.

Run `build/RenderWindow_artefacts/Release/RenderWindow` from the repo root to redraw the PNGs on real `say` speech, and `RenderWindow --live [runs] [seconds] [audio, silent or background] [module]` to measure CPU on the real window.

Window CPU, MacBook 2x screen, at 30 frames a second, is in the changelog. What moved it: the strip writes its pixels straight into a ring image instead of stroking paths, text and the piano are kept as cached images because macOS merges nearby repaints, and Filter's sweep marker moves at half the frame rate.

`build/PluginTest_artefacts/Release/PluginTest demo` writes every preset on a `say` voice to `build/demo`, for a quick listen outside any host.
