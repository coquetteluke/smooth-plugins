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
| Mode | LFO, Manual | LFO sweeps the wet by itself. Manual holds it where Flange puts it, for riding a flange by hand or with automation. Depth, Rate and the LFO controls do nothing in Manual. |
| Flange | -5 to +5 ms | Manual only. Where the wet sits against the dry path: + is late, - is early. Exactly 0 takes the wet deck out. |
| Fade | 0 to 2000 ms | Manual only. How long the wet takes to fade in when Flange leaves 0, and out when Flange returns to 0. Never faster than 10 ms. |
| Invert Phase | off, on | Flips the wet polarity from whatever it would otherwise be, so at Mix 50 the sound cancels at the crossing. With negative Feedback it flips it back. |
| Delay | 0 to 20 ms | Moves the whole sweep later. 0 is through zero. Delay 8 with Depth 3 sweeps 5 to 11 ms and never crosses, a classic flanger or a short double. No added latency. The feedback loop follows it. |
| LFO Shape | Triangle (0) to Sine (50) to Square (100) | Morphs the sweep. Reads as Triangle, Sine or Square at those points, and a percentage between. |
| LFO Symmetry | -100 to +100 % | Share of each cycle spent rising. 0 is even, +100 rises for 95 % of the cycle, -100 for 5 %. |
| LFO Phase | 0 to 360 degrees | Moves where the sweep starts. When synced, it moves the sweep against the bar, so the crossing can land on a chosen beat. |
| LFO Swing | -100 to +100 % | Makes every other cycle longer and the one after shorter. +100 is 1.5 and 0.5 cycles. Average speed stays put. |
| Wow Depth, Wow Rate | 0 to 2 ms, 0.1 to 5 Hz | Slow tape speed drift on the wet deck. |
| Flutter Depth, Flutter Rate | 0 to 0.3 ms, 5 to 30 Hz | Quicker tape speed wobble on the wet deck. |
| Random Depth, Random Rate | 0 to 2 ms, 0.1 to 20 Hz | Smooth random drift on the wet deck. Repeats exactly after each prepare, so a bounce is the same every time. |
| Random Unsync | off, on | Gives left and right their own random drift, which widens the stereo image. |
| Input Gain | -24 to +12 dB | Before everything, dry path included. Not made up afterwards, so it pushes the Saturation harder. |
| Output Gain | -24 to +12 dB | After everything. |
| Low Cut | Off, 21 to 1000 Hz | Two gentle poles on the wet only. Keeps the flange out of the low end of a voice. |
| High Cut | 1 to 20 kHz, Off | Two gentle poles on the wet only. Keeps the flange off S sounds. |
| Feedback Compensation | on, off | On holds the broadband level as Feedback rises. Off is louder and more resonant at high Feedback, with a soft ceiling on the delay line. |
| Bypass | off, on | Fades the wet out and keeps the same delay. Input Gain and Output Gain drop out too. |

## Presets

- **Slow Zero Crossing:** slow, gentle sweep through zero.
- **Wide Vocal Sweep:** lower mix with left and right a quarter cycle apart.
- **Takeoff Roar:** full depth, high feedback, very slow.
- **Hollow Inverted:** negative feedback, cancels and hollows out at the crossing.
- **One Bar Swirl:** synced, one sweep per bar, wide.
- **Dark Tape Haze:** heavy saturation, top end rolled off.
- **Quarter Note Wobble:** synced to quarter notes, left and right opposite.
- **Manual Ride:** Manual mode, 150 ms fade, low end kept clean. Dry until Flange moves off 0, so automate Flange.
- **Triangle Jet:** triangle sweep, high Feedback, both cuts in.
- **Off Zero Double:** Delay 14 ms, small sweep, a little wow, sides opposite. A short tape double that never crosses zero.
- **Wandering Tape:** no sweep, only wow, flutter and unsynced random drift.
- **Square Flip:** synced square flips the wet from early to late on half notes.
- **Phase Flip Thin:** Invert Phase with a narrow slow sweep, thin and hollow near the crossing.

The first seven are unchanged from version 1.0.0. Loading any preset sets every control, and puts the gains and drift rates back to their defaults.

## Signal flow

```
input -> oversample (4x up to 60 kHz host rate, 2x above) -> Input Gain
  dry:  fixed delay ------------------------------------------+
  wet:  input + feedback -> saturation -> delay line          |
        read at fixed delay + Delay                           |
             + (Depth x LFO wave, or Flange in Manual)        |
             + wow + flutter + random                         |
        -> make up -> polarity and Invert Phase -> Tone       |
        -> High Cut -> Low Cut -> Fade and Mix ---------------+-> Output Gain -> oversample down -> output
  feedback: read at a length equal to the distance from the dry path (never under 0.25 ms)
        -> head loss at 12 kHz -> low cut at 20 Hz -> limiter -> back to the line
```

The feedback loop length follows the wet to dry distance, so comb peaks and Mix notches move together. It is rounded off at 0.25 ms near the crossing so the loop never reaches zero length.

Nothing moves the wet delay faster than 0.5 seconds per second, so a square wave, a Flange jump or a Mode switch can bend pitch but cannot click. A sine at full Depth and top Rate is 0.31, so it never reaches that limit. The wet can never read ahead of the write point: all the drift at full stops at -5.25 ms.

The processor also offers `getReadings()` for a later window: the wet to dry delay on each side, how far the Manual fade has got, and input and output peaks. These are not AU parameters and never write automation.

## Measured

Every number is from `tests/PluginTest.cpp` and saved in `tests/results.txt`. Delays are read from outside the plug-in, by cross correlating the output with the input.

- Latency: 324 samples at 48 kHz (6.75 ms), 303 at 44.1 kHz, 535 at 88.2 kHz, 577 at 96 kHz. Measured dry delay matches at each rate, with Mix at 0 and with Bypass on.
- Through zero: Depth 3 ms measures +3.000 ms to -3.000 ms. An inverted wet at Mix 50 cancels the dry path by 41.8 dB at the crossing.
- Notches: first notch follows 1 / (2 x measured delay) on noise, median error 0.7 % after zero and 0.8 % before zero, worst 6.9 %. It moves from 95 Hz to over 1.7 kHz in the range measured (delay of 0.3 ms or more). Flat within 0.18 dB at the crossing.
- Rate: 0.25, 1 and 5 Hz measure 0.250, 1.000 and 4.998 Hz. Synced values within 0.1 %. No playhead gives 120 BPM.
- Depth: sweep range within 4 % of twice Depth at 44.1, 48 and 96 kHz.
- Feedback, Mix 100: peaks stand about 4, 9 and 21 dB above the median at 30, 60 and 90 %, both signs, both sides of zero. Peak positions within 3.8 % of the loop length.
- Feedback bounds: at +95 and -95 %, with full scale noise or sine, peak is +3.4 dBFS or less, and the tail is under -80 dBFS within 3 s.
- Saturation: THD at 1 kHz, -12 dBFS, 0.000, 0.525, 2.288, 5.773 and 11.772 % at 0, 25, 50, 75 and 100 %. Largest inharmonic product -77.8 dB on a 7.3 kHz sine.
- Tone: 12 kHz moves -11.8, -5.9 and +11.8 dB at -12, -6 and +12. 200 Hz does not move.
- Spread: 45, 90, 135 and 180 degrees measure within 0.01 degree.
- Mix: matches a straight blend of dry and wet to -145 dB. Mix 0 is within 0.002 dB of the input and nulls 73 dB or more against it.
- Same output with a playhead at 120 BPM or with none, at block size 64 or 1024, and after a second prepare.

Added in 1.1.0, limits set before the first run:

- Same sound: default settings and the first seven presets null against a saved render of 1.0.0 to -156 dB or better. Latency is the same at each rate. A session saved by 1.0.0 loads with every new control at its default.
- Flange: +/-2, 0.5 and -4.5 ms measure within 0.002 ms at 44.1, 48 and 96 kHz. The LFO moves it 0.003 ms in Manual. Flange 0 is dry only, with the wet out by -240 dB.
- Fade: half way at 250, 500 and 1000 ms for Fade 500, 1000 and 2000 ms, within 7 ms, in and out. Engaging at the crossing moves 1 kHz by 0.000 dB at Mix 50.
- Invert Phase: cancels by more than 160 dB at Depth 0, Mix 50. Only the wet flips, null -240 dB.
- Delay: 1.5, 12 and 20 ms measure within 0.002 ms at each rate. Delay 8 with Depth 3 sweeps 5.000 to 11.000 ms. Feedback repeats land on the sample.
- LFO Shape: mean level against Depth 0.501, 0.638 and 0.967 for Triangle, Sine and Square (theory 0.5, 0.637 and near 1).
- LFO Symmetry: share of the cycle rising 0.052, 0.261, 0.523, 0.736 and 0.948 at -100, -50, 0, 50 and 100.
- LFO Phase: within 0.001 degree, free running and locked to the bar.
- LFO Swing: cycle lengths within 0.002 cycle, average rate 1.000 Hz at every setting.
- Delay speed: Square, full Depth, top Rate tops out at 0.500 seconds per second, biggest output step 0.005 on a -18 dBFS sine.
- Wow: 1 ms measures 1.005 ms at 0.8 Hz and 1.002 ms at 3 Hz, rates exact to 3 places.
- Flutter: side bands round 1 kHz within 0.002 dB of the Bessel figure for 0.1 and 0.2 ms, at 12 and 25 Hz.
- Random: 1 ms stays at 0.994 ms or less, 0.504 ms rms. Sides identical with Unsync off, 0.715 ms rms apart with it on. Rate 15 Hz crosses zero 52 times against 3 at 1 Hz.
- Input and Output Gain: -12 and +6 dB measure to 0.000 dB. Input Gain +12 dB takes THD from 0.437 to 4.416 % at Saturation 50. Bypass ignores both.
- Low Cut 200 Hz: 50 Hz moves -24.7 dB, 2 kHz moves -0.14 dB. High Cut 4 kHz: 16 kHz moves -24.4 dB, 1 kHz moves -0.53 dB. Same at 96 kHz. Dry path moves 0.000 dB.
- Feedback Compensation off: +7.21 dB at Feedback 90 (theory 7.2), no change at 0. With Delay 20 ms and Feedback +/-95 on full scale noise, peak +4.0 dBFS or less and -104 dBFS or lower 4 s later.
- All new controls together: same output at block 64 and 1024, after a second prepare, and with or without a playhead, null -240 dB.

These are synthetic test signals. They do not prove it sounds good on a real voice.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target PluginTest -j 1
build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
cmake --build build --target InstallAU -j 1
```
