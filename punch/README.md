# Smooth Punch

Shapes the start and the tail of a sound, and adds fast compression. Attack and Sustain follow the shape of the sound, not how loud it is, so there is no threshold to set. Written from scratch from general audio techniques.

## How to load it

In your host: Audio Units > Luke Garito > Smooth Punch. AU code `aufx SPun Luke`. Mono and stereo. Latency is 2 ms: 88 samples at 44.1 kHz, 96 at 48 kHz, 192 at 96 kHz, and the same when bypassed.

## Controls

- **Punch** (0 to 100 %, default 0): amount of fast compression, level matched. The threshold follows the loudest recent level, so it does the same job on a quiet track as on a loud one. Up to 36 dB below that peak at 10 to 1 at the top of the control.
- **Attack** (-15 to +15 dB, default 0): boosts or cuts the start of a sound. Full effect for about the first 5 ms, gone by about 30 ms.
- **Sustain** (-15 to +15 dB, default 0): boosts or cuts the tail, as a sound dies away. Starts after about 15 ms. A steady sound has no tail, so it is left alone.
- **Mix** (0 to 100 %, default 100): blend with the dry signal, which is delayed by the same 2 ms.
- **Bypass**: fades to the input, delayed by the same 2 ms.

Not on the panel, fixed by design: 15 ms peak hold for the shaper and 8 ms for Punch, which is what keeps steady low tones clean. Punch releases in about 8 ms after its hold. Both channels share one gain, so the stereo image stays put.

## Presets

Punch, Attack, Sustain, Mix:

- **Neutral**: 0, 0, 0, 100.
- **Vocal Forward**: 30, +2.5, 0, 100.
- **Soft Consonants**: 20, -5, +1.5, 100.
- **Dry And Close**: 10, +1.5, -7, 100.
- **Longer Tails**: 15, 0, +6, 100.
- **Drum Snap**: 25, +8, -3, 100.
- **Parallel Weight**: 85, 0, +3, 40.

## Signal flow

```
input -+-> louder of the two channels -> peak hold -+-> start detector (fast against slow rise) --> Attack --+
       |                                            +-> tail detector (fast against slow fall) ---> Sustain -+
       |                                            +-> Punch: threshold from recent peak, level match ------+
       |                                                                                                     |
       |                                                                              sum, in dB, smoothed <-+
       |                                                                                     |
       +-> 2 ms delay ----------------------------------------------------------------> Mix and Bypass set how much of this gain is used -> output
```

There is one delayed copy of the input and one gain. Dry and wet are the same samples, so Mix can't comb filter.

Attack and Sustain compare envelopes with each other as ratios. Every envelope scales with the input, so the ratios do not change with level.

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

Limits to know about:

- Sustain follows how fast a sound dies away. +9 dB gives 9.0 dB on a 30 ms decay, 7.5 on 300 ms and 2.6 on 1 second. Slower decays read as steady sounds.
- Level match needs a moment. At Punch 50, the first hit after a long silence is 2.7 dB quieter than the input, the second 0.6 dB, the fifth 0.0 dB.
- At high Punch, the level match lifts quiet tails a lot, including any noise in them.
- Boosting Attack raises peaks and there is no output limiter, so watch the level after it.
