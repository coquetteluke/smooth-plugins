# Tape Color

Oversampled tape saturation with the things that make tape sound like tape: head bump and top end loss that follow the tape speed, wow, flutter, hiss, wear, a noise reduction compander, and the sound of small old equipment.

Load it in a host: Audio Units > Luke Garito > Tape Color. Mono and stereo. AU code: `aufx STcl Luke`.

## The window

Load it and the window says Tape Color. The build keeps the name Tape Color for now.

- **Age** is the big idea, right across the window: drag the ribbon from a fresh reel to a worn cassette, or click the number to type one. Top end, Hiss and Wobble beside it show what that setting costs. The reel beside it turns at the tape speed, shakes with the real read head, stops when nothing is playing, and shrinks into a cassette hub at Age 55.
- **The picture:** cream is the real spectrum going in, amber is the real spectrum coming out, on a scale tilted 3 dB an octave. The line is the tone of the tape from its settings, on the dB scale at the right. Rust is what Age has taken from the same settings at Age 0.
- **Saturation:** the curve the tape really uses, the straight line it would be without saturation, and in amber the part the sound is using right now. The number is how far the loudest moment is being held down.
- **Main controls:** Tape (Drive, Bias, Tape speed, Noise), Motion (Wow, Flutter), Finish (Mix, Output), Auto level, Bypass, presets.
- **Advanced** opens five sections and says what is in use while closed: Surface (Hardness, Texture, Dropouts, Dropout rate), Compander (Type, Encode, Decode, with the live boost), Playback (Low cut, High cut, Head gap, Head tilt), Device (Type, Device mix), Drift (Random, Stereo).
- Drag a block to change it, click its value to type (the cuts take "5k" and "off"), double-click anything to reset it. The line at the bottom says what the control under the mouse does. Resize from the corner: the window remembers its size and whether Advanced is open.

Renders are in `docs/window/`, made by `build/RenderWindow_artefacts/Release/RenderWindow` from real speech (macOS `say`, saved in `tests/baseline/voice.f32`) through the real processor. They are not screenshots of a host, and nobody has seen this window in Logic yet.

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

- **Low Cut** (Off, then 20 to 800 Hz, default Off) and **High Cut** (1 to 20 kHz, then Off, default Off): 12 dB per octave filters. High Cut holds that slope all the way to the top of the band.
- **Device** (Off, Small Radio, Telephone, Small Speaker, Dictation Machine, Old Console, default Off) and **Device Mix** (0 to 100 %, default 100): a short filter with the shape of small old equipment. These shapes are my own design, not captures of real hardware.
- **Mix** (0 to 100 %, default 100): blends dry and wet before the moving read head, and the depth of the head movement follows Mix. Wet and dry share one read head, so wow and flutter cannot comb against the dry signal. The tape comes out on the same sample as the dry signal, so a blend does not lose top end to a timing gap. Mix 0 is the input, delayed.
- **Output** (-24 to +24 dB, default 0).
- **Auto Level** (on or off, default on): holds the level steady across Drive. It only learns the saturation loss, so it does not undo the tone controls or the compander.
- **Bypass**: fades to the delayed dry signal. Latency stays the same.

## The old response

0.4.0 changed four things that had been wrong, and each of them changes the sound of a setting:

- The tape now comes out exactly on the reported latency. Up to 0.3.1 it came out 11.7 microseconds after it (0.56 sample at 48 kHz), so any Mix under 100 lost top end where the two paths met out of time.
- High Cut now falls 12 dB per octave all the way up. The old one fell much faster near the top of the band (27 dB in the octave above 10 kHz at 48 kHz).
- The compander's shelves now follow the analog shape. The old ones ran up to 0.5 dB over it at 10 kHz for a 10 dB boost, and 1.3 dB for 20 dB.
- Auto Level now leaves the tone alone. The old one compared the dry signal with the tape after its top end losses, so on sound that is mostly top end it took the loss for saturation and turned it back up: with Age at 100 it gave a 10 kHz tone back all 14 dB that Age had taken, and on a real voice the S sounds came out 2.6 dB hotter against the voice than with Auto Level off.

So that nothing made with 0.3.1 changes its sound, the plug-in keeps the old response for whatever was made with it:

- Six factory presets keep it, because their sound hangs on one of those four things: Parallel Body (Mix 40 and High Cut), Airy Encode (compander), Worn Cassette (compander, and Auto Level on dark tape), Slow Reel Haze, Starved Grit and Old Radio Voice (Auto Level on slow tape). They sound as they did.
- A session saved before 0.4.0 keeps it if it uses Mix under 100, High Cut, the compander, or Auto Level together with slow tape, Age over about 40, Head Spacing over about 30 or Bias over about 55. With none of those in use there is nothing to keep.
- Everything else gets the new response: a new instance, the other five presets (Tape Start, Clean Reel, Vocal Glue, Warm Lead, Phone Call), and any session saved from 0.4.0 on keeps whichever it had.

It is not a control and has no button: it travels with the preset or the session, in eight bytes after the saved settings. Picking a preset sets it for that preset. At Mix 100 the old response also leaves the tape on the reported latency, so the timing fix reaches every session.

## Presets

| Preset | What it is for |
|---|---|
| Tape Start | Drive 12 at 15 ips, nothing else |
| Clean Reel | Light Drive at 30 ips on soft tape |
| Vocal Glue | Drive 13, a touch of Bias, faint wow and flutter |
| Warm Lead | Drive 17, Bias 30, light Age, a little hiss and grain |
| Airy Encode | Dynamic Soft encode only, for air on a vocal. Keeps the old response |
| Parallel Body | Drive 26 on hard tape, High Cut 12 kHz, Mix 40. Keeps the old response |
| Slow Reel Haze | 7.5 ips, Age 20, Random and Stereo Drift, head spacing. Keeps the old response |
| Starved Grit | Bias -70 on hard tape with grain. Keeps the old response |
| Worn Cassette | 3.75 ips, Age 100, dropouts, mismatched compander. Keeps the old response |
| Old Radio Voice | Small Radio device with tape wear. Keeps the old response |
| Phone Call | Telephone device with hot tape |

## Signal flow

Input > dry delay (the oversampling's latency) > Mix

Input > compander encode > Drive > oversample up (16x at 44.1 and 48 kHz, 8x at 88.2 and 96 kHz) > record boost > curve > boost taken out > top end losses from Tape Speed, Age and Head Spacing > oversample down > DC blocker > head bump and dip > low end loss from Age > Auto Level gain > level drift and dropouts > Texture > Noise > compander decode > Low Cut > High Cut > Device > Mix > moving read head for Wow, Flutter, Random, Stereo Drift and Azimuth > Output

- The curve is tanh, averaged between samples through its antiderivative, plus a smaller tanh that makes the starved dead zone. That removes most of the aliasing the oversampling leaves behind.
- Latency is the oversampling plus 6 ms of tape path, in whole samples, always the same. Bypass is the input delayed by exactly that, and the tape comes out exactly then too: the oversampling filters keep their own delay, the 11.7 microseconds of the curve and the two top end filters at the default tone are counted, and a short delay at the high rate fills the rest up to a whole sample. That short delay follows Tape Speed, Bias, Age and Head Spacing as they move their filters, so the tape stays on the reported latency for as long as there is delay left to take off: 30 and 7.5 ips, any Bias at 15 and 30 ips, Age to about 25. Past that (3.75 ips, high Age, Head Spacing) the tape comes out late by what is left, and the cuts and the Device add the small delay any tone filter has below its corner.
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
- **Auto Level:** spread across Drive 0 to 30 is 0.05 dB on 1 kHz at -18 dBFS, 0.14 dB at -6 dBFS, 0.72 dB on a voice like test signal. What Age 100, 3.75 ips or Head Spacing 100 take off a tone at 3, 6 and 10 kHz is the same with Auto Level on as off within 0.2 dB (up to 0.3.1 Auto Level gave back 5 to 14 dB at 10 kHz).
- **Mix:** at Mix 50 the worst drop below the quieter path is 0.01 dB (0.8 dB up to 0.3.1), the same with Wow and Flutter at 100. Mix 0 nulls with the delayed input at -141 dB.
- **Heavy tape at Mix 50 (3.75 ips, Age 40, Bias 50):** one broad dip of 2.8 dB where the filtered wet path turns its phase (7.9 dB up to 0.3.1, when the wet path was also late). It is one dip, not a comb. A deliberate 1 ms misalignment does trigger the notch counter, as a control.
- **Compander:** Fixed boosts 10 kHz by 9.1 dB (the analog shape is 9.03) and both sides undo each other within 0.01 dB. Dynamic Soft boosts 9.1 dB at -50 dBFS and 0 dB at -6 dBFS. Dynamic Strong boosts 17.4 dB at -60 dBFS. Hiss falls 7.1, 7.1, 10.9 dB by type.
- **Head Spacing:** -11.98 dB at 10 kHz at 100 % and 15 ips. **Head Azimuth:** 0.100 ms between channels at 100 % and 15 ips.
- **Dropouts:** 49 dips in 30 s at a rate of 2 per second (limit 60 within 30 %), deepest -23.8 dB. The gain never moves more than 0.7 % from one sample to the next (up to 0.3.1 a dropout that began before the last had recovered jumped by up to 25 %). **Texture:** -22.1 dB against the tone at 100 %.
- **Device:** within 0.04 dB of each set shape at 48 and 96 kHz.
- **Latency:** 355 samples at 48 kHz (7.40 ms): 67 for oversampling, 288 for the tape path. 332 at 44.1 kHz, 596 at 88.2 kHz, 642 at 96 kHz. The same when bypassed. Bypass nulls with the delayed input. The tape itself comes out 0.22 microsecond (0.01 sample at 48 kHz) after the reported latency at all four rates, read from the phase of a low level impulse from 300 Hz to 8 kHz, and its peak lands on the reported sample. Up to 0.3.1 it came out 11.7 microseconds late (0.52, 0.56, 1.02 and 1.11 samples at the four rates), so at 88.2 kHz and up the reported number is one sample more than before. It stays there within 1.6 microseconds at 30 and 7.5 ips, Bias -100 to +100 and Hardness 100 (300 Hz to 3 kHz, at 48 and 96 kHz). Where the room runs out it is late by 0.6 sample at 3.75 ips, 0.9 at Head Spacing 50 and 2.2 at Head Spacing 100 (48 kHz). Taking all of that off as well would cost about 15 more samples of latency.
- **Left and right:** a mono signal comes out with both channels identical, bit for bit, and a mono instance gives the same samples as the left of a stereo one.
- **CPU:** through the real AU at 48 kHz with 128 frame blocks, percent of one core: default 2.6, worst preset 3.7 (Old Radio Voice), worst single block 5. The test suite with everything on measures about 4.
- **Same sound after the CPU work:** `tests/null_results.txt` holds the null of 0.2.1 against 0.2.0 for the default, every preset and random automation. Worst peak -114.4 dBFS, on a signal at 24 times full scale.

The limits that had been changed after a first run are back where they started, each with the fault it was covering fixed (0.4.0). Why is written beside each one in `tests/PluginTest.cpp`:

- High Cut one octave out: within 2 dB of 12.3 dB again. It had been made one sided because the filter fell too fast near the top of the band. The filter now follows the analog shape (12.3, 12.3, 12.3, 11.4 dB an octave above 1.5, 3, 6 and 10 kHz at 48 kHz).
- Heavy tape Mix: the dip is held to 6 dB again. It had been swapped for a notch count when the dip measured 8.6 dB. Most of that dip was the wet path being late, not the low pass. It is 2.8 dB now.
- Channels at Head Azimuth 0: identical again. It had been relaxed to -120 dBFS and put down to float rounding in the oversampling. The cause was the control: its middle came back as 0.0000015 and not 0, so both channels read through the moving head's interpolation.
- The compander's shelf: held to the analog shape again. It had been compared with the digital filter's own response instead, because that ran over the analog shape near the top of the band. The shelf is now matched to the analog shape (within 0.08 dB at 10 kHz for 10 dB).
- Dropouts are counted from the first frame after the latency. The first form read the latency's own silence as a dropout, and it had been moved half a second in.
- New in 0.4.0, from the real vocal checks: the tape path against the reported latency in microseconds, a dropout that never steps, and Auto Level against the top end losses.
- The window's tone line is read at every point on the chart. It had skipped tones near the hiss. Each tone is now read in its own narrow band, so none is skipped.

## The window, measured

- **Same sound (0.3.0 against 0.2.1):** `tests/null_results.txt` holds 55 renders through the real AU against 0.2.1: the default, every preset and random automation at three rates and in mono. All 55 are bit identical. From 0.4.0 the tape path sits 11.7 microseconds earlier, so renders no longer match 0.3.1 bit for bit; what changed and what did not is in CHANGELOG.md. `tests/baseline/renders-0.2.1.sha256` lists their checksums. `tests/PluginTest.cpp` also checks a window open against closed, with the tap running: not one bit differs.
- **The picture tells the truth:** the drawn tone against real tones through the real processor, in 12 settings: worst difference 0.48 dB (limit 1 dB). Top end against the measured bandwidth: 14.0 and 4.5 kHz against 14.1 and 4.6. Hiss: 0.3 dB from the real floor. The saturation number against a real signal: worst 0.07 dB. Tones within 6 dB of the hiss at high Age cannot be measured, so those points are skipped, and the Device blend is drawn by magnitude.
- **Real controls:** all 26 parameters have a control and nothing lives only in the window. The window's tone line follows the response in use, old or new. Each block types, automates, carries begin and end gestures and resets. The window opened and closed 40 times with audio running and 10 times with none.
- **Window CPU**, real window, Retina, Warm Lead on real speech, `RenderWindow --live`, the method in `~/smooth-resonance/docs/window-cpu.md`. One other test running. Six runs, percent of one core: 2.6, 2.8, 7.4, 9.8, 7.3, 5.0. The work never changed, 1.9 to 2.2 % of a fast core. The readings follow which cores macOS picked: 2.6 and 2.8 had over 90 % of it on fast cores, 7.4, 9.8 and 7.3 had 14 to 43 %. So it is under the 5 % budget on fast cores and over it on slow ones. Forced onto slow cores: 9.8, 9.5, 8.5. Silent, nothing moving: 0.7, 0.7, 1.0.
- **Audio CPU** through the real AU with AuBench, no window open: default 2.6, worst preset 3.7, the same as 0.2.1. Worst single block 8 against 7, which is run to run noise.
- **What made the difference:** the reel is far from the spectrum, so its repaint stretched the area being drawn to cover every label in between. Keeping that text and the ribbon in cached images took the work from 3.9 to 2.1 % of a fast core.

## Left out on purpose

- Separate on and off switches per section: setting a section's controls to their neutral values does the same.
- A low latency mode: changing latency in the middle of a session is not safe in a host. It stays at a fixed 7.4 ms.
- A steady detune: a constant pitch offset needs a delay that grows forever.
- A tape resistance control: Low Cut already does that job.
- Head width: no clear benefit on a vocal.
- Real device captures: the Device shapes are my own design, because I have no hardware to capture and did not take data from any other plug-in.
