# Throat

Reshapes the vocal tract of a voice without moving its pitch. It analyses the formants of the voice, then rebuilds them as a tube of a new length and shape would have them. It can also add breath and change how pressed or soft the vocal cords sound. All the code is written from scratch.

## How to load it

In your host: Audio Units > Luke Garito > Throat. AU code `aufx SThr Luke`. Mono and stereo. Latency is 1536 samples at 44.1 and 48 kHz (34.8 ms and 32.0 ms) and 3072 samples at 88.2 and 96 kHz (34.8 ms and 32.0 ms), the same when bypassed.

## The window

The tube is your singer's vocal tract seen from the side: cords at the bottom, lips at the right. **Bone** is the singer's own shape and **red** is the model. Both move with the voice, because the walls are worked out from where the formants sit. **Gray** is the realistic range: between 0.80 and 1.20 in width and length.

- Drag a point out or in for width, and along the tube for its place. Shift gives a finer drag. Double-click resets a point. Reset puts Length, Width and all five points back to the singer's own. A point too wide to fit is drawn 12 px inside the edge, and a drag starts from where it really is, so the first pixel does not change it (since 0.7.1).
- Under the bend: pitch and F1 to F5 in Hz, original against model, above the two formant curves. Pitch stays the same in both rows.
- The table under the tube holds every point's numbers. Click one to type, or drag it up and down.
- Advanced holds the voice types, Pulse, Air tone (Breath High Pass), Range, Precision, Level Match and Peak Guard.
- Drag the corner to resize. The size and whether Advanced is open are saved with the session, and so is the preset last chosen (since 0.7.1), only to name it again. A session saved before 0.7.1 does not say, so the box reads "Choose a preset" and the arrows go to the first or the last.

The live shape is a picture of the formants through a simple tube formula, compressed to fit. It is the same physics the engine uses, but it is not a measurement of a real throat, and it drives nothing in the sound. Pictures are in `docs/window`.

## Controls

Ratios read as plain numbers, where 1.00 is the singer's own. Between 0.80 and 1.20 is the realistic range. They move in steps of 0.001, and a third decimal shows when it is not 0, so 1.00 and "The singer's own" mean exactly 1, where the move is off (since 0.7.1; before, 0.995 to 1.004 all read 1.00).

- **Throat Length** (0.50 to 2.00, default 1.00): every formant moves by 1 over this number. 1.25 lowers them all to 80%.
- **Throat Width** (0.50 to 2.00, default 1.00): the whole tract wider or narrower. A wider mouth acts like a slightly longer tube, so formants fall by up to 6% at 2.00 and rise by up to 3% at 0.50. Wider also means less wall loss and so narrower formants, and narrower means broader ones.
- **Width 1 Vocal Cords, Width 2 Throat, Width 3 Back of Mouth, Width 4 Roof of Mouth, Width 5 Lips** (0.33 to 3.00, default 1.00): local width at five points from the cords to the lips. Between points the shape is a straight line in log width. Formants go where a real tube of that shape has them: narrow lips lower everything, a narrow roof of the mouth lowers F1 and raises F2.
- **Position 2 Throat** (15 to 38 %, default 27), **Position 3 Back of Mouth** (38 to 58 %, default 45), **Position 4 Roof of Mouth** (58 to 82 %, default 68), **Position 5 Lips** (82 to 100 %, default 100): where along the tract each point sits. 0 is the cords and 100 the lips. The ranges cannot cross. Point 1 stays at the cords. A position does nothing while the widths around it are 1.00.
- **Breathiness** (0 to 100 %, default 0): noise shaped by the same modelled tract as the voice, so it passes through the same formants. Harmonics to noise above the high pass is about 12 dB at 50 and about -6 dB at 100, a full whisper. Mostly follows voiced sound, with a quarter of it on consonants.
- **Breath High Pass** (200 to 10000 Hz, default 1500): the breath sits above this, falling at 18 dB per octave below it. Low is raspy and high is whispery.
- **Source Voice Type** (Soft, Medium, Loud, Intense, default Medium): how hard the input was sung.
- **Model Voice Type** (Soft, Medium, Loud, Intense, default Medium): how hard it should sound. Only the difference between the two matters: set them the same and the voice source is left alone. Soft is -3 dB per octave from Medium, Loud is +2.5, Intense is +5, above about 500 Hz. Voiced sound only.
- **Glottal Pulse Width** (-100 to +100 %, default 0): trims the model voice. Wider is darker: +100 is -4.5 dB per octave and -100 is +4.5.
- **Vocal Range** (Auto, Soprano, Alto Tenor, Bass Baritone, default Auto): limits the pitch search. Auto covers 67 to 1000 Hz. Pick a range if the pitch reading wanders.
- **Throat Precision** (Subtle, Medium, Extreme, default Medium): how narrow the model's formant peaks can be. Move to Extreme for big changes or if you hear whistling.
- **Mix** (0 to 100 %, default 100): blend with the dry signal, delayed to match. The wet path has no phase change, so it blends without combing.
- **Output Gain** (-24 to +12 dB, default 0).
- **Level Match** (on or off, default on): holds the level steady as formants and the voice type change. About 150 ms from the input's envelope with the gain each frame gets, then about 2 s from the real spectrum. For 320 ms after a phrase starts it follows each frame's real spectrum, so a phrase starts at its own level (since 0.7.0). The same while a control moves, but only one that changes the sound: a position while the widths around it are 1.00, or Air tone while Breathiness is 0, changes nothing at all (since 0.7.1).
- **Bypass**: fades to the input, delayed by the same amount. Takes Output Gain out too. A host that switches its own bypass gets the same fade, with no gap.
- **Peak Guard** (on or off, default off): moving formants changes the shape of the wave, so a peak can come out higher than it went in at the same level. On, the output is held at or under the highest peak the input has had since playback started (times Output Gain): the gain drops at once on a sample that would pass it and comes back over about 100 ms. A take that did not clip going in then cannot clip coming out. Off, nothing is added and the sound is as before. No preset changes it.

Read only, for a host to read by name. It never writes automation and is never saved:

- **Output Level**: peak after everything, -60 to +24 dBFS.

## Presets

Neutral, A Little Longer, A Little Shorter, Big Chest, Small Bright, Rounded Lips, Open Smile, Pinched Throat, Soft and Airy, Raspy Edge, Full Whisper, Giant, Tiny, Subtle Blend. On speech every one stays within 0.42 dB of the input level (Raspy Edge 0.42, Giant 0.29, the rest 0.11 or less; this read 0.2 dB until 0.7.1, which Raspy Edge and Giant already passed in 0.7.0).

## Signal flow

```
input > clean bad samples > dry delay (same length as the engine) ---------------> Mix > Bypass and Output Gain > output
      \> long window (43 ms) > pitch, voicing, envelope through the harmonics > formant model
                                                                                   |
         tract shape > frequency map > gain per bin = model (low) or envelope (high) at mapped place - here, + source tilt, + level match
                                                                                   |
      \> short window (21 ms) ------------------------- gain --- + breath ---> overlap add -> Mix
```

- The gain is real and smooth, so harmonics stay at their frequencies and pitch cannot move.
- With nothing asked of it the gain is exactly 1, and the overlap adds back to the input.
- Both channels share one analysis, so the stereo image holds.
- The tube is solved exactly, not by a small-change formula. A wave's phase is followed along it through 40 sections.
- The map is computed at every frequency instead of at tracked formants. There is no formant tracker to lose its place on a consonant.
- The model is an all-pole curve fitted to the harmonics themselves on voiced sound (discrete all-pole fitting), and to a smoothed envelope on unvoiced sound.
- The further a frequency moves, the more its harmonics take their level from the model. That removes the ghost of the old formant, which simple filtering leaves behind.
- From 4 times the pitch up (fully from 8 times), where many harmonics sit on each formant, the move reads the envelope through the harmonics instead of the model, which is smoother than the voice there. On real vocals that moves formants the whole way the tube says (since 0.6.0; before, about 0.7 to 0.8 of the way). On voiced frames that envelope is a plain cepstral one, every quefrency under 0.6 of the period kept, so small moves such as the width points carry the formant's whole shape (since 0.7.0). The voice source's fall comes off it first, as off the model, so the move does not carry the source's slope (since 0.7.1; in 0.7.0 that added a shelf from 4 to 8 times the pitch up of about +2.9 dB at Length 0.70 and -2.7 dB at 1.50). On frames that are not voiced the last pitch sung is not used to place this change: under a voicing of 0.2 it sits as at 60 Hz, so a consonant is shaped the same whatever note came before it (since 0.7.1).
- Above 6 kHz the gain is never more than 8 dB over what moving the envelope itself would give (11 dB in 0.4.1). The all-pole fit can make a peak at the top of its band that the voice does not have, and before 0.4.1 that came out as a burst of hiss on some sung vowels.
- No harmonic from 500 Hz up stands out from its neighbours by more than 8 dB, or 2 dB more than the input's most prominent one, so the voice does not whistle (since 0.5.0; since 0.6.0 the harmonics are read at whole multiples of the pitch).
- A phrase that starts after a pause gets its own Level Match average, so its first 30 ms are not turned down by the phrase before (since 0.5.0). A start is judged against the level 43 ms before, so one that rises over 30 ms is found too (since 0.7.0).

## For other plug-ins

`src/formant` has no plug-in code in it, and can be copied as a whole or in parts:

- `SpectralEnvelope.h`: pitch, voicing, envelope, formant model and formant readings from a power spectrum.
- `TractModel.h`: tube shape to frequency map.
- `FormantEngine.h`: the whole frame loop, driven by a Controls struct, with a lock-free Analysis block.

For a later window, `PluginProcessor::analysis()` gives input and output envelopes on 96 log spaced points, F1 to F5, pitch and voicing, without blocking audio.

## What was measured

Window: `tests/NullTest.cpp` compares against renders saved by the 0.2.0 build before any window code went in (`tests/baseline`). Seven cases on two macOS `say` voices, with the window closed, open and drawing, and being opened and closed during audio: every sample identical. It also checks that opening the window writes no parameter, that every parameter has a place on screen, that automation moves the controls, that a drag has begin and end gestures, and that the handle lands under the mouse.

Full list in `tests/results.txt`. Synthetic voices are a glottal pulse through formant filters, with the pulse's own level taken back off when reading formants. Real speech is two macOS `say` voices, which are synthetic too. **None of this proves it sounds good on a sung vocal.** That needs ears.

- Throat Length 0.70, 0.85, 1.20, 1.50 on four vowels: F1 to F4 within 4.4% of F divided by Length, at 44.1, 48 and 96 kHz.
- One limit changed: a formant within 3.5 times the pitch has only two or three harmonics on it, so it is held to a quarter of the harmonic spacing instead of 5%. Only the male ee's first formant needs this. It misses by 5 to 10%, which is 18 to 24 Hz on a voice around 100 Hz.
- Pitch: 0.003 cents at most on a held voice. On speech the first harmonic moves 0.3 to 1.2 cents (median of voiced frames).
- Width points: all F1 to F3 shifts of 1.5% or more go the way an exact tube calculation does, within a few percent points.
- Neutral: -142 dB from the input on voice and speech.
- Host Reset: play, reset, play gives exactly what a new instance gives, silence after it is silent, and it allocates nothing. The reset that comes with a host switching its own bypass on is left out (since 0.4.0).
- Old sessions: states read out of the installed 0.7.0 (`tests/baseline/v0.7.0-*`) name no preset and load exactly what choosing that preset here loads. The default plays what 0.7.0 played, bit for bit.
- Mix: Mix 0 is -141 dB from the delayed input, Mix 50 is -143 dB from half and half, no comb notches.
- Consonants: no added level movement on hiss (0.1 dB at most), plosive pre-echo at -78 dB, speech consonants move 0.65 to 1.00 times as much as the input.
- Latency: an impulse lands on the reported sample at all four rates, including bypassed.
