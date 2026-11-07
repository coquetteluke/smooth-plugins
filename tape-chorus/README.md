# Smooth Tape Chorus

A tape style chorus. Your signal goes through a soft tape saturation, then 1 to 4 moving read heads pull separate copies off a delay line, with slow and fast tape speed wobble on top. Spread places the copies across the stereo field. Mix blends the result with the untouched dry signal.

## How to load it

Audio Units > Luke Garito > Smooth Tape Chorus. AU code: `aufx STch Luke`. Works mono in to mono out, mono in to stereo out, and stereo in to stereo out. No licence system.

## Controls

| Control | Range | What it does |
|---|---|---|
| Voices | 1 to 4 | Number of separate moving copies. Copies fade in and out in 30 ms, and level stays about the same. |
| Rate | 0.05 to 10 Hz | Speed of the sweep. |
| Depth | 0 to 8 ms | How far each copy's delay swings either way from its centre. More depth, more pitch movement. |
| Delay Time | 3 to 40 ms | Centre delay. With several voices, the copies sit evenly around it. |
| Wow | 0 to 100 % | Slow, irregular tape drift, mostly below 2 Hz. Up to 2.5 ms swing. |
| Flutter | 0 to 100 % | Fast, irregular tape shake, around 7 to 11 Hz. Up to 0.25 ms swing. |
| Saturation | 0 to 100 % | Soft tape clipping on the chorus voices only. A -12 dBFS signal stays at about the same level. The dry signal is never saturated. |
| Tone | -12 to +6 dB | Top end of the chorus voices. A gentle shelf from about 2.5 kHz up. 0 is flat. |
| Spread | 0 to 100 % | 0 keeps left and right identical. Higher moves the two sides apart in time and sweep, and places the copies left and right. |
| Mix | 0 to 100 % | Equal power blend of dry and chorus. 0 gives back the input exactly. |
| Bypass | on or off | Fades to the exact dry signal in 20 ms. Latency stays 0. |

If Delay Time is too short to hold the modulation you asked for, Depth, Wow, Flutter and the voice offsets all shrink together, so the delay never falls below about 1.3 ms.

## Presets

- **Default Tape Chorus**: 2 voices, a general starting point.
- **Soft Vocal Double**: 2 voices, slow and subtle.
- **Wide Four Voices**: 4 voices, full Spread.
- **Worn Cassette**: heavy Wow, Flutter and Saturation with a dark top end.
- **Slow Drift**: 3 voices, a long slow sweep.
- **Fast Shimmer**: quick, shallow and a little bright.
- **Mono Thickener**: 3 voices, no Spread, stays in the centre.

## Signal flow

Input > dry path straight to Mix.

Input > pre-emphasis > oversampled soft clip (4x, or 2x from 60 kHz up) > de-emphasis > DC block > delay line > up to 4 voices, each with a left and right read head (6 point interpolation) > sum > Tone > Mix > output.

Latency is 0 samples at every rate. The oversampling filters take 59.5 samples at 48 kHz, and that amount is taken off the read head, so the delay you set is the delay you get and the dry path needs no delay.

## What was measured

Offline, in `tests/PluginTest.cpp`. Full numbers are in `tests/results.txt`.

- Voices: one pulse in gives 1, 2, 3 and 4 copies out. Same with modulation on, and each copy travels 1.56 ms at Depth 0.8 ms.
- Voice changes: sharpest bend in the signal while switching is under 2 times steady playback. Noise level stays within 0.1 dB across voice counts.
- Rate and Depth, tracked from the instantaneous phase of a sine: 0.5, 1, 2 and 5 Hz measure on target, and swing is within 0.5 percent, at 44.1, 48 and 96 kHz.
- Delay Time: pulse arrival within 0.02 ms at 5, 15 and 30 ms, at all three rates. The average of 3 and 4 voices is also on target.
- Wow: 99.9 percent of its movement is below 3 Hz. Flutter: 99.9 percent is above 4 Hz.
- Saturation: THD at 1 kHz, -12 dBFS rises with the control, with 0 under 0.01 percent. `tests/results.txt` has each step.
- Fold back with a 5 kHz sine at Saturation 100 is below -96 dB from 2 to 22 kHz. One line at 23 kHz, above hearing, sits at -53.8 dB. It is the 25 kHz harmonic inside the output filter's transition band.
- Tone: -12 gives about -10.6 dB at 12 kHz, +6 gives about +5.9 dB, and 200 Hz moves under 0.1 dB.
- Spread: correlation of left and right falls steadily from 1.0 at Spread 0 to under 0.3 at Spread 100, at all voice counts. This uses noise darkened below about 400 Hz, because white noise reads near 0 from Spread 25 up.
- Mix: Mix 0 and Bypass match the input exactly. Mix 50 gives dry and wet each at -3.01 dB, with the dry on time.
- Mono to stereo: Spread 0 keeps both sides identical, Spread 100 wet correlation is near 0, and left and right match within 0.1 dB. A hard left stereo source stays on the left.
- Output is identical at block sizes 512, 37 and 1, and after preparing again.
- Damaged saved state and NaN, infinite or huge input cannot stick in the audio path.

These are synthetic test signals. They do not prove it sounds good on a real voice. That still needs listening.
