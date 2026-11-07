# Smooth Tape Flanger

A through-zero flanger. The dry path sits on a fixed delay and a swept copy moves to either side of it, so the wet to dry delay crosses zero and goes negative, the way two tape machines do when one is slowed by hand. The fixed delay is reported to the host as latency, so the plug-in stays in time.

## Load it

Audio Units > Luke Garito > Smooth Tape Flanger. AU: `aufx STfl Luke`. Mono and stereo.

## Controls

| Control | Range | What it does |
|---|---|---|
| Rate | 0.01 to 10 Hz | Sweep speed when Tempo Sync is off. |
| Tempo Sync | off, on | Takes the sweep speed from the host tempo. With no tempo from the host it uses 120 BPM. While the host plays, the sweep locks to the bar. |
| Sync Division | 8 bars to 1/16, with dotted and triplet values | Length of one sweep when Tempo Sync is on. |
| Depth | 0 to 5 ms | How far the wet path moves to each side of the dry path. 2.5 ms means from 2.5 ms early to 2.5 ms late. |
| Feedback | -95 to +95 % | Lifts the comb peaks. Negative values also flip the wet polarity, so the comb stays deep on that side and the sound hollows out at the crossing. Moving past 0 is therefore a change of character, and it fades over about 15 ms rather than clicking. |
| Saturation | 0 to 100 % | Soft tape style saturation on the signal going to the delay line, inside the feedback loop. 0 is a straight wire. Level at -18 dBFS stays within 0.5 dB. |
| Tone | -12 to +12 dB | High shelf at 4 kHz on the wet path. |
| Spread | 0 to 180 degrees | Offsets the right sweep against the left. Mono uses the left sweep. |
| Mix | 0 to 100 % | Straight blend of dry and wet. 50 % gives the deepest notches. |
| Bypass | off, on | Fades the wet out and keeps the same delay. |

## Presets

- **Slow Zero Crossing:** slow, gentle sweep through zero.
- **Wide Vocal Sweep:** lower mix with left and right a quarter cycle apart.
- **Takeoff Roar:** full depth, high feedback, very slow.
- **Hollow Inverted:** negative feedback, cancels and hollows out at the crossing.
- **One Bar Swirl:** synced, one sweep per bar, wide.
- **Dark Tape Haze:** heavy saturation, top end rolled off.
- **Quarter Note Wobble:** synced to quarter notes, left and right opposite.

## Signal flow

```
input -> oversample (4x up to 60 kHz host rate, 2x above)
  dry:  fixed delay ------------------------------------------+
  wet:  input + feedback -> saturation -> delay line          |
        swept read (fixed delay + Depth x sine) -> make up    |
        -> polarity -> Tone -> Mix ---------------------------+-> oversample down -> output
  feedback: read at a length equal to the distance from the dry path (never under 0.25 ms)
        -> head loss at 12 kHz -> low cut at 20 Hz -> limiter -> back to the line
```

The feedback loop length follows the wet to dry distance, so comb peaks and Mix notches move together. It is rounded off at 0.25 ms near the crossing so the loop never reaches zero length.

## Measured

Every number is from `tests/PluginTest.cpp` and saved in `tests/results.txt`. Delays are read from outside the plug-in, by cross correlating the output with the input.

- Latency: 324 samples at 48 kHz (6.75 ms), 303 at 44.1 kHz, 535 at 88.2 kHz, 577 at 96 kHz. Measured dry delay matches at each rate, with Mix at 0 and with Bypass on.
- Through zero: Depth 3 ms measures +3.000 ms to -3.000 ms. An inverted wet at Mix 50 cancels the dry path by 41.8 dB at the crossing.
- Notches: first notch follows 1 / (2 x measured delay) on noise, median error 0.7 % after zero and 0.8 % before zero, worst 6.9 %. It moves from 95 Hz to over 1.7 kHz in the range measured (delay of 0.3 ms or more). Flat within 0.18 dB at the crossing.
- Rate: 0.25, 1 and 5 Hz measure 0.250, 1.000 and 4.998 Hz. Synced values within 0.1 %. No playhead gives 120 BPM.
- Depth: sweep range within 4 % of twice Depth at 44.1, 48 and 96 kHz.
- Feedback, Mix 100: peaks stand 0 dB, about 4, 9 and 21 dB above the median at 0, 30, 60 and 90 %, both signs, both sides of zero. Peak positions within 3.8 % of the loop length.
- Feedback bounds: at +95 and -95 %, with full scale noise or sine, peak is +3.4 dBFS or less, and the tail is under -80 dBFS within 3 s.
- Saturation: THD at 1 kHz, -12 dBFS, 0.000, 0.525, 2.288, 5.773 and 11.772 % at 0, 25, 50, 75 and 100 %. Largest inharmonic product -77.8 dB on a 7.3 kHz sine.
- Tone: 12 kHz moves -11.8, -5.9 and +11.8 dB at -12, -6 and +12. 200 Hz does not move.
- Spread: 45, 90, 135 and 180 degrees measure within 0.01 degree.
- Mix: matches a straight blend of dry and wet to -145 dB. Mix 0 is within 0.002 dB of the input and nulls 73 dB or more against it.
- Same output with a playhead at 120 BPM or with none, at block size 64 or 1024, and after a second prepare.

These are synthetic test signals. They do not prove it sounds good on a real voice.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target PluginTest -j 2
build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
cmake --build build --target InstallAU -j 2
```
