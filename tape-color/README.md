# Smooth Tape Color

Oversampled tape saturation with the things that make tape sound like tape: head bump and top end loss that follow the tape speed, wow, flutter, hiss, wear, a noise reduction compander, and the sound of small old equipment.

Load it in a host: Audio Units > Luke Garito > Smooth Tape Color. Mono and stereo. AU code: `aufx STcl Luke`.

## Controls

Tape:

- **Drive** (0 to 30 dB, default 12): how hard the signal hits the tape.
- **Bias** (-100 to +100 %, default 0): 0 is calibrated. Going up moves the curve off centre: more even harmonics, a little less top end. Going down starves the tape: low level signal drops into a dead zone and picks up odd harmonics.
- **Tape Hardness** (0 to 100 %, default 50): how soon the tape saturates. Soft tape takes 6 dB more level, hard tape 6 dB less. Low level gain does not change.
- **Tape Speed** (3.75, 7.5, 15, 30 ips, default 15): moves the head bump and the top end rolloff. Slower tape also saturates the highs sooner.
- **Age** (0 to 100 %, default 0): one sweep from fresh reel to worn cassette. Less bandwidth, less headroom, more hiss, more wow and flutter, slow level drift.
- **Noise** (0 to 100 %, default 0): hiss, -90 to -45 dBFS. Exactly nothing at 0. Age adds hiss of its own, so silence is only exactly silent with both at 0.
- **Texture** (0 to 100 %, default 0): grain on the tape surface that follows the signal, about 20 dB under it at 100. Silence stays silent.
- **Dropout Depth** (0 to 100 %, default 0) and **Dropout Rate** (0.1 to 10 per second, default 1): short level dips where the coating is thin, up to 24 dB deep.

Playback head:

- **Head Spacing** (0 to 100 %, default 0): a gap between head and tape. Loses more the higher the frequency and the slower the tape.
- **Head Azimuth** (-100 to +100 %, default 0): a tilted head reads one channel early. 0.1 ms between channels at 100 % and 15 ips, more on slower tape. Stereo only. It adds width and loses top end when summed to mono.

Transport:

- **Wow** (0 to 100 %, default 0): 0.8 Hz main swing, 20 cents at 100 %, with a slower line under it.
- **Flutter** (0 to 100 %, default 0): 9 Hz main swing, 6 cents at 100 %, with a faster line and an irregular part.
- **Random Drift** (0 to 100 %, default 0): irregular slow movement between 0.3 and 4 Hz.
- **Stereo Drift** (0 to 100 %, default 0): lets the right channel move on its own. 0 keeps both together, which is safe in mono.

Noise reduction:

- **Compander** (Off, Fixed Emphasis, Dynamic Soft, Dynamic Strong, default Off): an encode shelf before the tape and a decode shelf after the hiss. Fixed is always 10 dB. Dynamic Soft boosts quiet highs by up to 10 dB and Dynamic Strong by up to 20 dB, and neither touches loud highs.
- **Encode Amount** and **Decode Amount** (0 to 100 %, default 100): set them apart for a deliberate mismatch. Encode without decode adds air and bite. Decode without encode goes dark and expanded.

Finish:

- **Low Cut** (Off, then 20 to 800 Hz, default Off) and **High Cut** (1 to 20 kHz, then Off, default Off): 12 dB per octave filters.
- **Device** (Off, Small Radio, Telephone, Small Speaker, Dictation Machine, Old Console, default Off) and **Device Mix** (0 to 100 %, default 100): a short filter with the shape of small old equipment. These shapes are my own design, not captures of real hardware.
- **Mix** (0 to 100 %, default 100): blends dry and wet before the moving read head, and the depth of the head movement follows Mix. Wet and dry share one read head, so wow and flutter cannot comb against the dry signal. Mix 0 is the input, delayed.
- **Output** (-24 to +24 dB, default 0).
- **Auto Level** (on or off, default on): holds the level steady across Drive. It only learns the saturation loss, so it does not undo the tone controls or the compander.
- **Bypass**: fades to the delayed dry signal. Latency stays the same.

## Presets

| Preset | What it is for |
|---|---|
| Tape Start | Drive 12 at 15 ips, nothing else |
| Clean Reel | Light Drive at 30 ips on soft tape |
| Vocal Glue | Drive 13, a touch of Bias, faint wow and flutter |
| Warm Lead | Drive 17, Bias 30, light Age, a little hiss and grain |
| Airy Encode | Dynamic Soft encode only, for air on a vocal |
| Parallel Body | Drive 26 on hard tape, High Cut 12 kHz, Mix 40 |
| Slow Reel Haze | 7.5 ips, Age 20, Random and Stereo Drift, head spacing |
| Starved Grit | Bias -70 on hard tape with grain |
| Worn Cassette | 3.75 ips, Age 100, dropouts, mismatched compander |
| Old Radio Voice | Small Radio device with tape wear |
| Phone Call | Telephone device with hot tape |

## Signal flow

Input > dry delay (the oversampling's latency) > Mix

Input > compander encode > Drive > oversample up (16x at 44.1 and 48 kHz, 8x at 88.2 and 96 kHz) > record boost > curve > boost taken out > top end losses from Tape Speed, Age and Head Spacing > oversample down > DC blocker > head bump and dip > low end loss from Age > Auto Level gain > level drift and dropouts > Texture > Noise > compander decode > Low Cut > High Cut > Device > Mix > moving read head for Wow, Flutter, Random, Stereo Drift and Azimuth > Output

- The curve is tanh, averaged between samples through its antiderivative, plus a smaller tanh that makes the starved dead zone. That removes most of the aliasing the oversampling leaves behind.
- Latency is the oversampling plus 6 ms of tape path, in whole samples, always the same. Bypass is the input delayed by exactly that.
- The read head uses a 16 point windowed sinc. With no movement it reads whole samples and changes nothing.
- All randomness uses fixed seeds, so a bounce is the same every time, with a playhead or without one.

## Measured

Offline, from `tests/PluginTest.cpp`. Full list in `tests/results.txt`. All test signals are synthetic, so none of this proves how it sounds on a real voice.

- **Drive:** THD at 1 kHz, -18 dBFS: 0.04, 0.13, 0.45, 1.66, 5.54, 13.8 % at 0, 6, 12, 18, 24, 30 dB.
- **Bias:** even minus odd harmonics at Drive 12: -29.6 dB at -100, -11.6 dB at 0, +23.3 dB at +100. At -30 dBFS and Drive 0, Bias -100 is 11 dB quieter with 2.7 % THD against 0.008 % at Bias 0.
- **Tape Hardness:** THD at Drive 12: 0.13, 0.45, 1.66 % at 0, 50, 100. Low level gain unchanged.
- **Tape Speed:** head bump at 30, 45, 69, 111 Hz and -3 dB point at 8.2, 14.4, 24.5, 40.4 kHz for 3.75, 7.5, 15, 30 ips. The response at 16 frequencies per speed is in `tests/results.txt`.
- **Wow:** 0.80 Hz, 2.31 ms, 20.0 cents at 100 %. **Flutter:** 9.00 Hz, 0.060 ms, 5.9 cents at 100 %.
- **Random Drift:** 0.38 ms RMS at 100 % (set 0.35). **Stereo Drift:** 2.36 ms RMS between channels with Wow 100 (set 2.37).
- **Noise:** -79.2, -67.9, -45.1 dBFS at 25, 50, 100 %. At 0 every sample on silence is exactly zero.
- **Age 0, 50, 100:** bandwidth 24.0, 14.1, 4.6 kHz. Noise floor silent, -62.4, -50.3 dBFS. Read head movement 0, 0.21, 0.83 ms RMS. Level swing 0, 0.7, 2.8 dB. Fresh end: 0.04 % THD at Drive 0, -0.15 dB at 10 kHz.
- **Low Cut and High Cut:** -3.01 dB at the corner at 48 kHz, within 0.5 dB at 44.1, 48 and 96 kHz.
- **Aliasing:** 15.1 kHz at -6 dBFS, Drive 30: non harmonic energy -73.7 dB against the tone.
- **Auto Level:** spread across Drive 0 to 30 is 0.05 dB on 1 kHz at -18 dBFS, 0.14 dB at -6 dBFS, 0.72 dB on a voice like test signal.
- **Mix:** at Mix 50 the worst drop below the quieter path is 0.8 dB, the same with Wow and Flutter at 100. Mix 0 nulls with the delayed input at -141 dB.
- **Heavy tape at Mix 50 (3.75 ips, Age 40, Bias 50):** one broad dip of 7.9 dB where the filtered wet path turns its phase. It is one dip, not a comb. A deliberate 1 ms misalignment does trigger the notch counter, as a control.
- **Compander:** Fixed boosts 10 kHz by 9.5 dB and both sides undo each other within 0.01 dB. Dynamic Soft boosts 9.5 dB at -50 dBFS and 0 dB at -6 dBFS. Dynamic Strong boosts 18.5 dB at -60 dBFS. Hiss falls 7.4, 7.4, 11.2 dB by type.
- **Head Spacing:** -11.98 dB at 10 kHz at 100 % and 15 ips. **Head Azimuth:** 0.100 ms between channels at 100 % and 15 ips.
- **Dropouts:** 48 dips in 30 s at a rate of 2 per second (limit 60 within 30 %), deepest -23.8 dB. **Texture:** -22.1 dB against the tone at 100 %.
- **Device:** within 0.04 dB of each set shape at 48 and 96 kHz.
- **Latency:** 355 samples at 48 kHz (7.40 ms): 67 for oversampling, 288 for the tape path. The same when bypassed. Bypass nulls with the delayed input.
- **CPU:** through the real AU at 48 kHz with 128 frame blocks, percent of one core: default 2.6, worst preset 3.7 (Old Radio Voice), worst single block 5. The test suite with everything on measures about 4.
- **Same sound after the CPU work:** `tests/null_results.txt` holds the null of 0.2.1 against 0.2.0 for the default, every preset and random automation. Worst peak -114.4 dBFS, on a signal at 24 times full scale.

Three test limits changed after a first run, each because the limit itself was wrong. Why is written beside each one in `tests/PluginTest.cpp`:

- High Cut one octave out is now one sided: digital filters fall faster near the top of the band.
- Heavy tape Mix now counts notches rather than capping the one dip.
- Channels at Head Azimuth 0 must match under -120 dBFS rather than exactly. They measure -168 dB, which is float rounding in the oversampling.

## Left out on purpose

- Separate on and off switches per section: setting a section's controls to their neutral values does the same.
- A low latency mode: changing latency in the middle of a session is not safe in a host. It stays at a fixed 7.4 ms.
- A steady detune: a constant pitch offset needs a delay that grows forever.
- A tape resistance control: Low Cut already does that job.
- Head width: no clear benefit on a vocal.
- Real device captures: the Device shapes are my own design, because I have no hardware to capture and did not take data from any other plug-in.
