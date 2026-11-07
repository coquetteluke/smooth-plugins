# Smooth Duo

Automatic vocal doubling. Your vocal plays untouched, and 1 to 3 copies follow it, each with its own wandering timing and pitch, so they sound like separate takes instead of an echo or a chorus. You can also change each copy's vibrato and the size of its voice, and place the lead and the copies in the stereo field.

## How to load it

Audio Units > Luke Garito > Smooth Duo. AU code: `aufx SDuo Luke`. Works mono to stereo (the usual choice on a vocal track), mono to mono, and stereo to stereo. No licence system. Zero latency at every sample rate, bypassed or not.

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

## Not done

- Formant movement is measured on noise and a sawtooth with a formant shape, not on a singer.
- The pitch follower for vibrato is measured on clean notes. Breathy or noisy singing may follow less well.
- A custom window comes in a later phase. For it, the processor already offers each double's current delay and the pitch being followed, safe to read from any thread.
