# Duo

Automatic vocal doubling. Your vocal plays untouched, and 1 to 3 copies follow it, each with its own wandering timing and pitch, so they sound like separate takes instead of an echo or a chorus. You can also change each copy's vibrato and the size of its voice, and place the lead and the copies in the stereo field.

## How to load it

Audio Units > Luke Garito > Duo. AU code: `aufx SDuo Luke`. Works mono to stereo (the usual choice on a vocal track), mono to mono, and stereo to stereo. No licence system. Zero latency at every sample rate, bypassed or not.

Switchboard sees and sets every control through the link (`src/SmoothLink.h`, since 0.5.0): a card of every parameter in `~/Library/Application Support/SmoothPlugins/link/`, and set commands that move the real parameter the way the window does.

The host's Reset (Logic sends one when playback starts and before a bounce) empties the delay lines and starts each double's wander over from its beginning, so the doubles never sing what came before it and two bounces of the same part come out the same. The Reset JUCE sends right after the host's own bypass switches on is left out, so the doubles still fade to dry in 20 ms.

## Controls

| Control | Range | What it does |
|---|---|---|
| Doubles | 1 to 3 | How many separate copies. Copies fade in and out in 30 ms. |
| Tightness | 0 to 100 % loose | 0 is tight, 100 is loose. How far timing and pitch wander, and how late the copies sit. |
| Width | 0 to 100 % | 0 keeps all copies in the centre, with left and right identical. 100 pulls them apart. With 2 or 3 doubles the pair goes hard left and right. A double with no partner (the only one, or the third) keeps its place and fades to two different read heads, one per side, so the picture stays balanced. Width never changes a delay, so moving it never bends the pitch. |
| Lead Level | Off, -59.9 to +6 dB | Level of the untouched vocal. Bottom of the range is off. |
| Double 1, 2, 3 Level | Off, -59.9 to +6 dB | Level of each copy. |
| Timing Amount | 0 to 200 % | Scales the slow timing wander set by Tightness. 100 is normal. |
| Pitch Amount | 0 to 200 % | Scales the quicker pitch wander set by Tightness. 100 is normal. Timing wander bends pitch a little too, so 0 here is not perfectly still unless Timing Amount is also 0. |
| Vibrato Variation | 0 to 100 % | Follows the vibrato in your vocal and changes it in the copies. At 100, double 1 sings almost straight, double 2 sings half again as wide, double 3 sings half as wide. Does nothing to a voice with no vibrato, and leaves slides and note changes alone. |
| Precise Vibrato | on or off | On (a new Duo opens with it on): Vibrato Variation finds your vibrato's own speed and lines its change up with each double's delay, so it works at any vibrato speed and at any Tightness. Off: the way Vibrato Variation worked before 0.4, which only got it right for a vibrato near 5.5 Hz with the doubles close. Sessions saved before 0.4 load with it off, so they sound exactly as they did. Every preset switches it on. |
| Timbre Variation | 0 to 100 % | Moves each copy's formants without moving its pitch, so it sounds like a different throat. At 100: double 1 up 3 semitones, double 2 down 3, double 3 up 1.5. At 0 the copies are exact, unprocessed copies. |
| Vocal Range | Soprano, Alto or Tenor, Baritone or Bass, Instrument | Where to look for the pitch. Only Vibrato Variation and the formant follower use it. |
| Lead Pan | -100 to +100 % | Places the lead. Equal power. On one output it does nothing. |
| Doubles Pan | -100 to +100 % | Moves all copies together, on top of Width. |
| Extra Delay | 0 to 100 ms | Adds a fixed delay to every copy. Turn it up for a slapback. Moving it bends the pitch for a moment, like moving a tape head. |
| Mono Safe | on or off | Off unless you switch it on. Each double goes to left and right with opposite signs. Add left and right together and the doubles cancel, leaving your own voice alone: that is what a phone speaker plays, and what a tuner or harmony plug-in after Duo listens to (Choir Loft tracks left plus right). In stereo the doubles are all still there at the same level, wide and not placed anywhere. Width and Doubles Pan have nothing to move while it is on. With one output it does nothing. |
| Match Level | on or off | On in a new Duo and in the Natural Pair preset, off in the other presets and in sessions saved before 0.4.1. Turns the lead and the doubles down together by what they add up to, so Duo comes out as loud as the vocal that went in (at the default that is 1.8 dB). The balance between you and the doubles, the pans and the width stay as set. With it on the lead is no longer the input sample for sample: it is the input, turned down. |
| Bypass | on or off | Fades to the exact dry signal in 20 ms. |

## The window

A stage seen from straight above, with the audience at the bottom. The title reads Duo, and the build name stays Duo until the rename.

- **Left to right** is where each singer is panned. **Further back** is how far behind the lead a double is singing, read live from the sound. The rows on the floor are marked in ms and squeezed, so a few ms near the lead reads clearly and a 100 ms slapback still fits.
- **Lanes:** each double has a tape mark where its path is centred, and a lane showing how far it can drift. Tightness makes the lane longer and moves it back. A double wanders inside its lane while you sing.
- **Turning shoulders:** a moving delay is a pitch change, so each double's shoulders turn with its pitch drift, worked out from how fast its delay is moving.
- **Pools of light:** as big as the input times that singer's level. A double with no partner across the centre has its light stretched to both sides by Width, because that is what Width does to it.
- **Size:** Timbre Variation makes a higher throat a smaller singer and a lower throat a bigger one.
- **Drag a singer:** a double sideways is Width, a double back or forward is Tightness, whichever way the drag starts. The lead sideways is Lead Pan. Double-click resets.
- **Around the stage:** Width underneath, with one handle under each double. Tightness up the side. Doubles and Bypass above. Level faders on the right, with the note being followed and each double's live delay under them.
- **Advanced:** Timing, Pitch, Vibrato, Timbre, Vocal Range, Lead Pan, Doubles Pan, Extra Delay, and three switches at the right: Precise Vibrato, Mono Safe and Match Level. While Mono Safe is on, Width and Doubles Pan are dimmed, as on a mono track, and the stage still draws the doubles where Width would put them.
- Everything is a real AU parameter with its old ID, name and range. The window adds only its size and whether Advanced is open to a session.
- The preset box names the preset last chosen, which the session keeps since 0.4.3. A session saved before that does not say, so it reads "Choose a preset" until one is chosen, and the arrows then go to the first or the last preset.
- With nothing playing, doubles walk back to their marks and the window stops drawing.

Pictures are in `docs/window/`, made by `build/RenderWindow_artefacts/Release/RenderWindow`. Inter is under the SIL OFL, see `assets/fonts/OFL.txt`.

## Presets

- **Natural Pair**: the default. 2 doubles, one on each side, with Match Level on so it comes out as loud as the dry vocal.
- **Tight Double**: 1 close copy, a moderate spread.
- **Quiet Thickener**: 2 quiet copies, close to centre.
- **Wide Stack**: 3 doubles, full Width, a little vibrato and timbre change.
- **Loose Gang**: 3 doubles, very loose, the most difference between voices.
- **Doubles Only Wide**: lead off, 2 copies wide. For a send or a copied track.
- **Two Singers**: lead left of centre, one copy right, with changed timbre and vibrato.
- **Slapback Double**: 1 copy at about 90 ms, for an obvious echo double.

Which preset was chosen is saved with the session (since 0.4.3), only to name it: its values are in the session already and are not applied again.

## Signal flow

Lead: input > Lead Level and Lead Pan > output, never delayed.

Doubles: input summed to mono > spectrum step that moves formants (11 ms) > one delay line per double > read head moved by its own slow timing curve, its own quicker pitch curve and the vibrato bend > per double level, pan and side heads > output.

Reading through a moving delay changes pitch by the rate the delay moves. The curves are random smooth paths with no repeat, each double with its own seeds and speeds, so there is no sweep rate. Reads use a 32 tap windowed sinc. The spectrum step's 11 ms comes out of each double's own delay, which is why the plug-in has zero latency.

A stereo input feeds the doubles from its mono sum, so sound that is opposite on left and right does not reach them. The lead keeps its stereo image.

Vibrato Variation with Precise Vibrato on: the pitch follower gives a reading every 5 ms. A straight line plus one sinusoid is fitted to the last cycle and a half of those readings, for each vibrato speed from 4 to 8.5 Hz, and the speed that explains the pitch best wins. If that sinusoid explains less than half of what the line leaves, is under 3 cents, or the line moves more than three times as far as the sinusoid swings, it is not vibrato: it is never used, and the bend fades out over 50 ms on the last sinusoid that was, which runs on as it fades. Each double then reads the sinusoid at the moment it is playing (its own delay back from now), so the bend lands on the vibrato it is meant to change. When the note jumps (more than 150 cents in 40 ms), a double gets no bend from the old note's vibrato once it plays the new note. When a vibrato stops on a held note (since 0.4.4): once fits have passed for 100 ms, each new reading is checked against the fit from 20 to 40 ms before it, which says where it should land. Two readings in a row that land off it towards its line by more than 0.3 of the swing (at least 6 cents, plus what that fit missed its own newest reading by) and by more than 6 times as far as readings have lately been landing off (so a real voice's wander does not count) mean the vibrato has ended. From there a double gets no bend, as at a new note, and no fit is used until the readings follow one again for 30 ms.

Mono Safe: each double is added to the left and taken from the right (the second double the other way round) at its own level, through its one centre read head.

Match Level: the squares of every voice's gain are added up for each side, the two sides averaged, and every gain divided by the square root of that. Real voices through the doubles add in power to within 0.2 dB of that sum.

## Why a moving delay and not a pitch shifter

Both were measured on the same sines, with a small two head shifter written for the test. The moving delay leaves the note cleaner by over 90 dB and has no level ripple, against up to 6.9 dB for the shifter. The catch: a moving delay cannot hold a steady pitch offset for long, because the delay would run away. The pitch wanders around the lead instead of sitting sharp or flat.

## What was measured

All in `tests/results.txt`, from `tests/PluginTest.cpp`, on synthetic signals. Limits were set before the results. These numbers do not prove it sounds good on a real voice.

- Doubles: an impulse comes back as lead plus exactly 1, 2 or 3 copies.
- Tightness, measured from the audio of each voice: timing swings about +/-1.3 ms at 0 and +/-12 ms at 100. Pitch peaks at about 3 cents at 0 and 24 cents at 100. Both grow at every step.
- Level: lead and every double within 0.2 dB at 1 kHz and 4.8 kHz, at Width 0 and 100.
- Width: left and right correlation on noise from 1.00 to 0.00, falling at every step, with 1, 2 and 3 doubles.
- Doubles against each other: correlation at most 0.05 on noise. Timing and pitch paths over 5 minutes share almost nothing.
- Zipper and read noise, loosest setting, worst double: -101 dB at 1 kHz, -92 dB at 4.8 and 9.6 kHz, -89 dB at 16 kHz. Limits were -70 dB, and -50 dB at 16 kHz. Level ripple 0.001 dB or less. Movement slower than about 100 Hz counts as the voice's own drift, not as this noise. A known -60 dB side tone reads as -60 dB, so the measure works.
- Sweep rate: the largest spectral line holds a small share of the path's energy, against 96 percent for a plain chorus sweep measured the same way.
- Timing Amount, Pitch Amount, Extra Delay, both pans, Vibrato Variation, Timbre Variation and Vocal Range each have their own measured checks.
- Latency: 0 samples at 44.1, 48, 88.2 and 96 kHz, bypassed or not.
- Timbre's spectrum frame is 10.7 to 11.6 ms at every rate from 44.1 to 192 kHz, and the formant move holds at 176.4 and 192 kHz as well.
- Precise Vibrato into a jump of a fifth: once double 1 sings the new note the bend stays under 3 cents (it was 31 cents for about 50 ms before 0.4.3). Switched back on while a straight note plays, it adds under 2 cents (35 before).
- Precise Vibrato on a 50 cent 5.5 Hz vibrato that stops on a held note: from 60 ms after the stop the bend stays at 3.2 cents or under (31 cents at Tightness 30 and 35 at Tightness 85 before 0.4.4). A vibrato that keeps going, steady or wandering in speed and depth, plays bit for bit as on 0.4.3 on every preset that uses it.
- Host Reset: play, reset, play gives exactly what a new instance gives; silence after a reset is silent; it allocates nothing.
- A session saved by the installed 0.4.2 (default, Slapback Double, and Loose Gang with Precise Vibrato off) plays bit for bit what 0.4.2 played with it. See `tests/baseline/`.
- Null test: every output sample's bits, on real speech and sung-style material from macOS `say`, hash the same as the 0.2.0 source for all 8 presets, as first opened, mono layouts, 44.1 and 96 kHz, and with every control moving. Both with no window and with the window open and drawing. See `tests/NullTest.h` and `tests/WindowTests.h`.
- Window CPU: 2.4% to 4.4% of one core with Loose Gang playing, nothing drawn when silent. See `docs/window-cpu.md`.
- Window: every parameter has a control, automation moves each one, each drag is one gesture, double-click resets, typing works. Singers stay on their real delay and inside their lanes. 30 opens and closes with audio running.

## Level

Duo adds voices, so without Match Level it is louder than the dry: 1.6 to 1.8 dB over sung parts with the default's levels, which is the power sum the level controls set (+1.77 dB). The default opens with Match Level on: it turns the lead and the doubles down together (1.8 dB at the default) and leaves the balance alone, so Duo comes out at the level of the dry vocal. With Match Level off the lead at Lead Level 0 dB is the input exactly, sample for sample. On Luke's takes the output is then within 0.2 dB of the dry at the default and within 0.15 dB at Wide Stack, Loose Gang and Doubles Only Wide. Doing it by hand gives the same: Lead -1.8 dB, Double 1 and 2 -7.8 dB. Peaks can pass 0 dBFS when the input is already near it and Match Level is off, never by more than every voice adding in phase allows. Logic mixes in floating point, so nothing clips inside the channel. There is no limiter.

## On real singing

Measured on the 11 working takes of Luke's Rode session (01, 02, 04, 05, 06, 07, 09, 10, 12, 14, 16: 32 minutes of singing and rap) by `_shared/realvox/src/extra/duo.cpp`, 2026-11-11, version 0.4.0. The numbers are the same as 0.3.1's wherever the three new switches are as an older session loads them.

- Timing swing and pitch offset of each double, measured from its audio: +/-1.2 ms and 2.0 to 2.3 cents at Tightness 0, +/-4.0 ms and 6.3 to 7.1 cents at 30, +/-10.6 to 10.7 ms and 16 to 18 cents at 100. With Timing and Pitch Amount at 0 nothing moves.
- At Vibrato Variation 0 the doubles follow the lead's pitch movement (depth 1.00, lag 0.3 ms) and its scoops (1.00), breathy singing included.
- Vibrato Variation at 100 with Precise Vibrato on, on these takes with a 40 cent vibrato put onto the held notes: double 1 keeps 0.28 of it at 4.5 Hz, 0.18 at 5.5 Hz, 0.18 at 6.8 Hz and 0.29 at 4.5 Hz with Tightness 85. Double 2 sings 1.41 to 1.44 times as wide, double 3 0.56 times. With Precise Vibrato off (the 0.3 law) double 1 keeps 0.52, 0.18, 0.47 and 0.70. Luke's own takes hold almost no vibrato (3 stretches in 32 minutes), so on his voice as recorded Vibrato Variation has next to nothing to change, by design.
- Steps of its own: none. The shared click check counts 13 small steps at the default; each is a sharp sound of the take that a double plays again (the four looked at sample by sample are mouth ticks 25 to 33 ms earlier). With the take low passed at 2.5 kHz all 13 are gone and the output holds nothing above 8 kHz: -76 dB at the default against a limit of -50 dB, -71 dB or lower at Tightness 100 with 3 doubles, with Vibrato at 100 and with Timbre at 100. A 9.6 kHz sine as long as each take comes out with nothing over -103 dB under it.
- Mono: at the default the mono sum is 0.40 to 0.42 dB under the two sides in every octave band from 125 Hz to 8 kHz, which is what the pans alone give (0.42 dB), and 1.3 to 1.4 dB over the dry. At full Width with 1, 2 and 3 doubles it is 0.4, 0.8 and 0.9 dB quieter, no lasting comb (deepest lasting notch 1.1 dB). Two voices on one note beat: the lowest band dips 6 dB or more under the dry for 7% of the time.
- Left against right on a mono voice: 0.01 dB over the takes. Over 400 ms it swings by 0.5 dB (median) and 2.2 dB (95th percentile), which is the doubles moving.
- Timbre Variation moves formants by 2.7 to 2.8 semitones at 100 (claim 3) and 1.4 to 1.5 at 60 (claim 1.8), level within 0.2 dB. The pitch does not move: every harmonic stays within 0.8 cents (95th percentile) on held notes.
- What a pitch tracker after Duo gets, as the share of held-note frames where the shared detector reads the mono sum of Duo's output more than 30 cents from the dry or loses it: see the table in `_vox/rode/work/duo/REPORT.md`. With Mono Safe on it is 0, because the mono sum is then the dry voice.
- CPU: 0.7% of one core on average, worst block 6%.

## Not done

- On Luke's voice as recorded, Vibrato Variation changes almost nothing, because he sings almost no vibrato. The first real-vocal check counted slides and wobble as vibrato and so read this as a fault; it still reads that way (0.98 against a limit of 0.5), and Duo is right not to flatten slides.
- At Timbre Variation 100 the take's own mouth ticks come out with a changed shape in the doubles (up to 2.1 times as large in one of 24 cases, 1.07 times at the median). The shared click check does not know a reshaped copy from a new click, so it counts 24 events at Timbre 100 against 7 at Timbre 0.
