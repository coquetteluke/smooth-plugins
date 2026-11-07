# Smooth Punch

Makes a vocal hit harder and sit forward. Shapes the start and the tail of a sound, adds fast compression, evens out level and raises it, and holds the result under an output ceiling. Attack and Sustain follow the shape of the sound, not how loud it is, so there is no threshold to set. All the code is written from scratch.

## How to load it

In your host: Audio Units > Luke Garito > Smooth Punch. AU code `aufx SPun Luke`. Mono and stereo. Latency is 2 ms: 88 samples at 44.1 kHz, 96 at 48 kHz, 192 at 96 kHz, and the same when bypassed.

## Controls

- **Punch** (0 to 100 %, default 0): amount of fast compression, level matched. The threshold follows the loudest recent level, so it does the same job on a quiet track as on a loud one. Up to 36 dB below that peak at 10 to 1 at the top of the control.
- **Attack** (-15 to +15 dB, default 0): boosts or cuts the start of a sound. Full effect for about the first 5 ms, gone by about 30 ms.
- **Sustain** (-15 to +15 dB, default 0): boosts or cuts the tail, as a sound dies away. Starts after about 15 ms. A steady sound has no tail, so it is left alone.
- **Mix** (0 to 100 %, default 100): blend with the dry signal, which is delayed by the same 2 ms.
- **Bypass**: fades to the input, delayed by the same 2 ms. Takes Gain and Ceiling out too.
- **Gain** (-12 to +24 dB, default 0): level in front of everything, for quiet takes. It moves dry and wet alike, so Mix 0 still carries it. Attack, Sustain and Punch ignore level, so it does not change them. It does drive Impact harder.
- **Impact** (0 to 100 %, default 0): the one control for evening out a performance and raising it. Slower than Punch, so it rides the level of phrases instead of changing the shape of each sound. More Impact means a lower threshold (-6 to -36 dBFS), a higher ratio (1 to 1 up to 6 to 1) and more makeup (up to 24 dB). It reacts in about 5 ms. It lets go in about 90 ms after a short peak and about 500 ms after a level that lasted. Its makeup fades out below about -50 dBFS and is gone at -70 dBFS, so near silence isn't lifted.
- **Ceiling** (-24 to 0 dBFS, default -1): the most the output can reach while Ceiling On is on. Last in the chain, after Mix.
- **Ceiling On** (default off): off keeps the first version's behaviour, with no limit on the output. All the new presets turn it on. Impact raises the level, so turn this on when you use Impact.

Read only, for a host to read by name. They never write automation and are never saved:

- **Input Level**: peak before Gain, -60 to +24 dBFS.
- **Output Level**: peak after everything, -60 to +24 dBFS.
- **Impact Reduction**: how far Impact is pulling down, before its makeup, in dB.
- **Ceiling Reduction**: how far the ceiling is pulling down, in dB.

Not on the panel, fixed by design: 15 ms peak hold for the shaper and 8 ms for Punch, which is what keeps steady low tones clean. Punch releases in about 8 ms after its hold. Both channels share one gain, so the stereo image stays put.

## The window

The window is called Punch, and is the name this plug-in will carry after the rename. The build still installs as Smooth Punch.

- **The big picture is the last hit**, 400 ms of it. White is the sound as it came in. Orange is how it goes out. Dark orange under the white line is what the settings took off. Orange above the white line is what they added.
- The time scale is stretched at the start, because that is where a hit happens: the first 30 ms take close to half the width.
- **Attack sits under the start of the hit and Sustain under its tail.** Hovering over either one lights the part of the hit it works on.
- **Across the top, the last few seconds** as a row of bars. The bright bars are the hit shown below.
- **On the right, In and Out** on the same level scale as the picture, then **how far Impact and the Ceiling are pulling down**, hanging from the top, with large numbers under them.
- **The Ceiling is also a line on the picture** while it is on. Drag it, or double-click it to put it back.
- Before anything has played, the picture is an example hit, and says so. The controls still move it. When playback stops, moving a control redraws the last hit with the new settings, and says so. Both run the sound level through the real detector at the display's rate, so they are a close drawing, not the exact result. Against the real result on bursts, the average error is 0.1 dB for Attack or Sustain alone and 1.7 dB with everything on.
- Gain and Mix are under Advanced. When it is closed, the row says if either has moved.
- Double-click anything to reset it. Click a value to type one. The window can be resized, and its size is saved.

Every control is still a real AU parameter with the same ID, name and range, so automation reaches them all, and the window moves with them. A hit is the level at least doubling against 20 to 32 ms earlier, above about -45 dBFS, and no sooner than 350 ms after the last one, so one can be read before the next arrives. The drawn levels are smoothed by a few milliseconds to take out the ripple of a voice's own cycles. A cut at the very start of a hit reads about 2.6 dB short because of that: Attack -9 draws as -6.4.

## Presets

Punch, Attack, Sustain, Mix:

- **Neutral**: 0, 0, 0, 100.
- **Vocal Forward**: 30, +2.5, 0, 100.
- **Soft Consonants**: 20, -5, +1.5, 100.
- **Dry And Close**: 10, +1.5, -7, 100.
- **Longer Tails**: 15, 0, +6, 100.
- **Drum Snap**: 25, +8, -3, 100.
- **Parallel Weight**: 85, 0, +3, 40.

These add Gain, Impact and Ceiling, with Ceiling On:

- **Even Lead**: 0, +1.5, 0, 100. Gain 0, Impact 40, Ceiling -1.
- **Rap Upfront**: 20, +3, -2, 100. Gain 0, Impact 55, Ceiling -1.
- **Quiet Take Lift**: 0, 0, 0, 100. Gain +12, Impact 35, Ceiling -3.
- **Spoken Word**: 0, 0, -3, 100. Gain 0, Impact 45, Ceiling -3.
- **Full Level Hook**: 15, +2, 0, 100. Gain 0, Impact 70, Ceiling -0.5.
- **Parallel Impact**: 0, 0, 0, 50. Gain 0, Impact 90, Ceiling -1.
- **Ceiling Only**: 0, 0, 0, 100. Gain 0, Impact 0, Ceiling -1.

## Signal flow

```
input -> Gain -+-> louder of the two channels -> peak hold -+-> start detector (fast against slow rise) --> Attack --+
               |                                            +-> tail detector (fast against slow fall) ---> Sustain -+
               |                                            +-> Punch: threshold from recent peak, level match ------+
               |                                            +-> Impact: fixed threshold, two speed release, makeup --+
               |                                                                                                     |
               |                                                                              sum, in dB, smoothed <-+
               |                                                                                     |
               +-> 2 ms delay ----------------------------------------------------------------> Mix and Bypass set how much of this gain is used
                                                                                                     |
                                                                                                     +-> Ceiling -> output
```

There is one delayed copy of the input and one gain. Dry and wet are the same samples, so Mix can't comb filter.

Attack and Sustain compare envelopes with each other as ratios. Every envelope scales with the input, so the ratios do not change with level.

The ceiling adds no latency. The sample going into the 2 ms delay comes out 2 ms later, so its level at the current gain is the look ahead. A final exact step catches what the look ahead can't see, such as a lone spike at an onset while Attack's boost is still arriving. A signal under the ceiling passes untouched.

## What was measured

All offline, in `tests/PluginTest.cpp`, with results in `tests/results.txt`. Pass limits were written before the first run. All tests use synthetic signals. **This does not prove how it sounds on a real voice.**

Decaying burst: instant onset, 100 ms decay time. Start means the first 3 ms, tail means 60 to 300 ms after the onset.

- Attack +9 and -9 dB on a 1 kHz burst: start +8.97 and -8.97 dB, tail 0.00 dB.
- Sustain +9 and -9 dB on a 1 kHz burst: tail +8.96 and -8.96 dB, start 0.00 dB.
- The same on 200 Hz, 4 kHz and noise bursts: within 0.03 dB of those.
- Level independence at -6, -18, -30 and -42 dBFS: 0.000 dB spread for Attack and Sustain, start and tail.
- 44.1, 48 and 96 kHz: 0.002 dB spread or less.
- Steady 60 Hz, 100 Hz, 1 kHz and 9 kHz with Attack and Sustain at maximum: nulls fully against the input.
- Punch 25, 50, 75, 100 on a 300 Hz burst train (12.1 dB crest factor): crest factor down 1.49, 3.34, 4.68, 5.35 dB. RMS level change 0.03 dB or less.
- The same on a noise burst train (14.0 dB crest factor): down 1.43, 3.21, 4.45, 5.02 dB. RMS level change 0.03 dB or less.
- Punch 100 on a steady 100 Hz tone: level change -0.02 dB, everything other than level at -113 dB.
- Punch 50 across 36 dB of input level: crest reduction spread 0.001 dB.
- Mix 50 against the average of Mix 0 and Mix 100: difference -144 dB. Broadband response at Mix 50 spreads 1.2 dB. A control with a dry path misaligned by 20 samples reads 8.3 dB, so the measurement can see a comb.
- Mix 0 with every control on, and neutral settings at Mix 100 and 50: nulls fully against the delayed input.
- Impulse delay equals the reported latency at 44.1, 48, 88.2 and 96 kHz. Bypass nulls against the delayed input.
- Any block size from 1 to 4096 gives the same output, and preparing again gives a clean restart.
- Damaged saved state (NaN, infinity, huge, negative, garbage, cut off): controls stay in range and output stays finite. A dropped value goes back to its default. Those tests fail when the guard is taken out.
- Bad input samples (NaN, infinity, huge values): replaced or clamped on the way in, and the plug-in recovers fully afterwards.

Added in 0.2.0, with limits written before the first run:

- Same as 0.1.0: all seven first presets, and a state saved by 0.1.0 with none of the new values, match numbers recorded from the 0.1.0 code before any change. Largest difference in any 10 ms: 0.000 dB.
- Gain -12, +6, +12, +24 at neutral: level moves by exactly that, to 0.000 dB. Attack and Sustain with Gain +12 move 0.000 dB against no Gain.
- Gain +18 into Impact 50 on a -30 dBFS tone: 11.6 dB more out, so it drives the compressor.
- Impact 25, 50, 75, 100 on a 24 dB gap between loud and quiet stretches: gap shrinks by 3.8, 9.8, 16.1, 18.3 dB.
- Impact 25, 50, 100 on a -24 dBFS tone: level rises 6.0, 12.0, 14.0 dB. On -40 dBFS at 100: 24.0 dB. On -6 dBFS at 100: -1.0 dB.
- Impact 50 and 100 on a speech shaped test signal (18.1 dB level spread): spread shrinks by 5.5 and 9.6 dB. This is a synthetic signal, not a voice.
- Impact 100 on steady 65 Hz, 100 Hz and 1 kHz: the gain is flat, nothing but level changes.
- Impact 100, jump from -30 to -6 dBFS: gain within 0.1 dB of settled 25 ms later.
- Impact 100 recovery: 184 ms after a 50 ms peak, 948 ms after a 1.5 s hold.
- Impact 100 on noise at -80 dBFS: lifted 0.0 dB.
- Ceiling -1, -6, -12, -24 dBFS on hot hits with all boosts on, a lone onset spike, and at 44.1 and 96 kHz: at most 0.000 dB over. Sample peaks, not true peaks.
- Ceiling -6 on a -12 dBFS tone: nulls against the input. Steady 60 Hz, 100 Hz, 1 kHz, 6 dB over a -12 ceiling: lands on the ceiling, gain flat.
- Ceiling hard onset: largest gain move in one sample 0.002 dB, or 0.36 dB with Attack +12. Back within 1 dB 165 ms after a hit.
- Those ceiling tests were checked by breaking the code: without the look ahead five checks fail, and without the exact last step the onset spike goes 12.8 dB over.
- Latency with Gain, Impact and Ceiling on: 88, 96 and 192 samples at 44.1, 48 and 96 kHz, the same as before.
- Readings, read through their value strings: Input Level -12.0 and Output Level -6.0 with Gain +6 on -12 dBFS, Impact Reduction -10.7 at Impact 50 on -6 dBFS, Ceiling Reduction -6.0 at Ceiling -12 on -6 dBFS. Zero automation writes during audio.
- All seven new presets: stay under their ceiling on hot material, and raise a -30 dBFS tone or leave it as it was (0.0 to +20.3 dB).

Added in 0.3.0, with limits written before the first run:

- Audio unchanged: real speech from macOS `say`, two voices under four settings, through the installed AU with `tests/AuRender.cpp` and `tests/null/render.sh`. All eight results are byte for byte the same as 0.2.0, with hashes in `tests/null/`.
- Window open, ticking and painting against window closed, on real speech with everything on: difference exactly 0.
- Before and after on the display, at neutral: the same in every bin, so they line up in time.
- The drawn shape on a burst: Attack +9 draws +9.0 at the start and 0.0 in the tail. Sustain -9 draws -9.0 in the tail and 0.0 at the start.
- Hits: 5 bursts make 5 hits at -30 dBFS, a steady tone makes 1, silence and a -60 dBFS tone make none, real speech makes about 2 a second.
- 60 windows opened, painted and closed, 30 of them with audio running on another thread. One opens and paints before the host has prepared the plug-in. Frames and paints write no parameter and no automation. Ten seconds of silence from the host moves nothing.
- Audio CPU through `_shared/bin/AuBench`: 0.6 to 0.7% default before and after. Window CPU: see `docs/window-cpu.md`, measured with `build/RenderWindow_artefacts/Release/RenderWindow --live`.
- The pictures in `docs/window/` come from `RenderWindow`, with real speech going through the real processor. None of this has been opened in a real host window yet.

Limits to know about:

- Sustain follows how fast a sound dies away. +9 dB gives 9.0 dB on a 30 ms decay, 7.5 on 300 ms and 2.6 on 1 second. Slower decays read as steady sounds.
- Level match needs a moment. At Punch 50, the first hit after a long silence is 2.7 dB quieter than the input, the second 0.6 dB, the fifth 0.0 dB.
- At high Punch, the level match lifts quiet tails a lot, including any noise in them.
- Boosting Attack raises peaks. With Ceiling On off there is no limit on the output, so watch the level after it.
- The ceiling works on sample peaks, so a peak between samples can land slightly over it. Leave a little room if the next stage is a converter or a lossy encoder.
- Impact is a fixed threshold, so unlike the rest of the plug-in it depends on input level. A very quiet take needs Gain first.
- Impact's makeup lifts anything above about -50 dBFS, including room noise and breaths, by up to 24 dB at the top of the control.
- A lone spike with a big Attack boost is held by the exact last step, which is a hard edge for that one moment. Only that case uses it hard.
- Output is also clamped at +/-16 (+24 dBFS) as a last safety, even with Ceiling On off.
