# Duo

Automatic vocal doubling. Your vocal plays untouched, and 1 to 3 copies follow it, each with its own wandering timing and pitch, so they sound like separate takes instead of an echo or a chorus. You can also change each copy's vibrato and the size of its voice, and place the lead and the copies in the stereo field.

## How to load it

Audio Units > Luke Garito > Duo. AU code: `aufx SDuo Luke`. Works mono to stereo (the usual choice on a vocal track), mono to mono, and stereo to stereo. No licence system. Zero latency at every sample rate, bypassed or not.

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
| Vibrato Variation | 0 to 100 % | Follows the vibrato in your vocal and changes it in the copies. At 100, double 1 sings almost straight, double 2 sings half again as wide, double 3 sings half as wide. Does nothing to a voice with no vibrato. |
| Timbre Variation | 0 to 100 % | Moves each copy's formants without moving its pitch, so it sounds like a different throat. At 100: double 1 up 3 semitones, double 2 down 3, double 3 up 1.5. At 0 the copies are exact, unprocessed copies. |
| Vocal Range | Soprano, Alto or Tenor, Baritone or Bass, Instrument | Where to look for the pitch. Only Vibrato Variation and the formant follower use it. |
| Lead Pan | -100 to +100 % | Places the lead. Equal power. On one output it does nothing. |
| Doubles Pan | -100 to +100 % | Moves all copies together, on top of Width. |
| Extra Delay | 0 to 100 ms | Adds a fixed delay to every copy. Turn it up for a slapback. Moving it bends the pitch for a moment, like moving a tape head. |
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
- **Advanced:** Timing, Pitch, Vibrato, Timbre, Vocal Range, Lead Pan, Doubles Pan, Extra Delay.
- Everything is a real AU parameter with its old ID, name and range. The window adds only its size and whether Advanced is open to a session.
- With nothing playing, doubles walk back to their marks and the window stops drawing.

Pictures are in `docs/window/`, made by `build/RenderWindow_artefacts/Release/RenderWindow`. Inter is under the SIL OFL, see `assets/fonts/OFL.txt`.

## Presets

- **Natural Pair**: the default. 2 doubles, one on each side.
- **Tight Double**: 1 close copy, a moderate spread.
- **Quiet Thickener**: 2 quiet copies, close to centre.
- **Wide Stack**: 3 doubles, full Width, a little vibrato and timbre change.
- **Loose Gang**: 3 doubles, very loose, the most difference between voices.
- **Doubles Only Wide**: lead off, 2 copies wide. For a send or a copied track.
- **Two Singers**: lead left of centre, one copy right, with changed timbre and vibrato.
- **Slapback Double**: 1 copy at about 90 ms, for an obvious echo double.

## Signal flow

Lead: input > Lead Level and Lead Pan > output, never delayed.

Doubles: input summed to mono > spectrum step that moves formants (11 ms) > one delay line per double > read head moved by its own slow timing curve, its own quicker pitch curve and the vibrato bend > per double level, pan and side heads > output.

Reading through a moving delay changes pitch by the rate the delay moves. The curves are random smooth paths with no repeat, each double with its own seeds and speeds, so there is no sweep rate. Reads use a 32 tap windowed sinc. The spectrum step's 11 ms comes out of each double's own delay, which is why the plug-in has zero latency.

A stereo input feeds the doubles from its mono sum, so sound that is opposite on left and right does not reach them. The lead keeps its stereo image.

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
- Null test: every output sample's bits, on real speech and sung-style material from macOS `say`, hash the same as the 0.2.0 source for all 8 presets, as first opened, mono layouts, 44.1 and 96 kHz, and with every control moving. Both with no window and with the window open and drawing. See `tests/NullTest.h` and `tests/WindowTests.h`.
- Window CPU: 2.4% to 4.4% of one core with Loose Gang playing, nothing drawn when silent. See `docs/window-cpu.md`.
- Window: every parameter has a control, automation moves each one, each drag is one gesture, double-click resets, typing works. Singers stay on their real delay and inside their lanes. 30 opens and closes with audio running.

## Level

Duo adds voices, so it is louder than the dry: at the default about +1.7 dB over sung parts, which is the power sum the level controls set (+1.77 dB). That is on purpose and stays. The lead at Lead Level 0 dB is the input exactly, sample for sample, and turning it down to make room would change every session already saved. Peaks can pass 0 dBFS when the input is already near it (take 06: dry at 0 dBFS, Duo +4.4 dBFS), never more than every voice adding in phase allows. Logic mixes in floating point, so nothing clips inside the channel; pull the fader or a Lead or Double level down if the track runs hot. There is no limiter.

## On real singing

Measured on Luke's working set takes (01, 02, 03, 04, 06, 07, 09) by `_shared/realvox/src/extra/duo.cpp`, 2026-11-10, version 0.3.1:

- Timing swing and pitch offset of each double, measured from its audio: about +/-1.2 ms and 2 cents at Tightness 0, +/-4.1 ms and 7 cents at 30, +/-11 ms and 17 to 19 cents at 100. With Timing and Pitch Amount at 0 nothing moves.
- At Vibrato Variation 0 the doubles follow the lead's vibrato (depth 1.00, no lag) and its scoops (1.00), breathy singing included.
- Clicks: every click the shared tool counted as Duo's own (79) is a delayed copy of a sharp step already in the take, except one, 0.45 s after one of the take's own clicks, to be checked again on the new takes.
- Mono at full Width: 0.5 to 0.9 dB quieter, no lasting comb.
- Timbre Variation moves formants by 2.8 semitones at 100 (claim 3), level within 0.2 dB.

## Not done

- Vibrato Variation on real vibrato does much less than on the test tone: at 100, double 1 keeps 0.91 of the lead's depth (claim: almost straight) and double 2 sings 1.08 times as wide (claim: 1.5). It bends, but mostly out of step with the vibrato. Needs its own fix.
- Timbre Variation at 60 and 100 leaves pitch alone on most frames (median under 1 cent), but 5% of frames read 29 to 31 cents off, against 5 cents for a static filter read the same way. Needs ears.
