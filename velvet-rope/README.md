# Velvet Rope

Finds the narrow peaks that stick out of a voice and turns them down only while they are there. Written from scratch from the general technique: an overlapping STFT, a comparison of each band with the peaks around it, and gain smoothed over time and frequency. No license system.

## How to load it

Audio Units > Luke Garito > Velvet Rope. AU code `aufx SmRs Luke`. The window's title reads Velvet Rope, the final name. The build and AU keep the old name until the rename.

Mono, stereo and surround in matching layouts, with an optional mono or stereo side chain input. Latency depends on Quality, is reported to the host, and is the same when bypassed:

| Quality | 44.1 and 48 kHz | 88.2 and 96 kHz | At 48 kHz |
|---|---|---|---|
| Low Latency | 512 samples | 1024 | 10.7 ms |
| Normal (default) | 2048 | 4096 | 42.7 ms |
| High | 4096 | 8192 | 85.3 ms |

**When Quality changes, the host is told at once, but a host may only apply its new delay compensation when playback restarts.** Stop and start playback after changing it, so the track lines up again. Do not automate Quality across a section you need in time.

## The window

A calm, near-flat dark window with the colour kept for the graph. Your voice is a body of blue light standing on the floor of the graph, and whatever is being turned down hangs from the ceiling and comes off the voice in soft gold. Gold only ever means "what is being taken out".

- **Graph:** frequency across, 20 Hz to 20 kHz. The edge of the voice is the sound after treatment. The scale on the right is reduction in dB, after Mix. The tilt of 4.5 dB an octave is for display only. When Link is low, a thin pale line shows the second channel's cut.
- **Dots 1 to 8:** the sensitivity curve. Double-click an empty part of the graph to add a dot. To take one away, hover it and click its x, use Remove in the strip, or right-click it. Drag a dot to move it. Up treats more there, down protects it. Scroll on a dot, or hold Command and drag, for width. Hold Shift for a finer drag. Double-click a dot to reset it.
- **Low and High lines:** drag to set the range. Nothing outside them is touched.
- **Strip under the graph:** Low, High, a button for each dot, the values of the selected dot, Listen and Remove. A faint number is a free dot, and clicking it adds it.
- **Band Listen on:** a solid mint banner on the graph says so, everything outside the region goes dark, and Listen lights up. A click on the banner stops it.
- **Side panel:** Depth, Max Cut, Sharpness, Selectivity, Attack, Release, Quality and Mode are always there. Held notes sits at the end of Mode's row and goes grey in Hard mode, where it does nothing. Threshold comes out under Mode in Hard mode.
- **Surround:** a row under Advanced shows each channel's name and deepest cut. The LFE shows none. The gold on the graph is the deepest of all channels, and the pale line is the shallowest. Stereo Mode is greyed out.
- **Advanced:** click it to open Threshold, Stereo, Link, Focus, the three tilts, Wet trim and Side chain. Closed, it says how many of them are changed from their defaults, and they keep working. Whether it is open is saved with the session.
- **Footer:** Bypass, Delta, Mix, Gain, and a one-line hint for whatever is under the mouse.
- **Every control:** drag it, double-click to reset it, click its value to type one (`3k` works for a frequency), hold Shift when you start a drag for a finer one.
- **Size:** drag the bottom right corner, 870 to 1856 points wide, always the same shape. The size is saved with the session.

Every control is a real parameter with its old ID, name and range, shown or not, so automation moves the window and the window moves them, with begin and end of gesture. The window keeps no sound settings of its own. Its graph data comes from the lock free `readGraph`, so it cannot block the audio thread.

Pictures are in `docs/window-v4/` (0.6.0, with Held notes), with the earlier designs kept in `docs/window-v3/`, `docs/window-v2/` and `docs/window/` for comparison. They are made by `build/RenderWindow_artefacts/Release/RenderWindow`, which runs real speech from macOS `say` with resonant boosts through the real processor and renders the real window to PNG with no host. `RenderWindow --live` opens a real window with all but one column of pixels off the screen, and measures frames a second, CPU and instructions.

Measured on the 16 GB MacBook's 2x screen with other builds running: 30 frames a second, and 3.0% to 5.0% of one core over 8 runs with audio playing. If macOS puts it on the slow cores it reads 7% to 9% for the same work. With nothing moving it hardly redraws. Closed, it costs nothing. Details are in `docs/window-cpu.md`.

The window's font is Inter, under the SIL Open Font License. See `assets/fonts/OFL.txt`.

## Try first on a harsh vocal

1. Load the preset **Harsh Lead Vocal**. Depth 12, Selectivity 50, 1.2 to 10 kHz.
2. Turn on **Delta**. If you can make out words in it, it is working too hard: raise Selectivity or lower Depth. Turn Delta off.
3. For a whole stack, use **Vocal Stack Smoother** on the group.

## Controls

- **Depth** (0 to 24 dB, default 6): how hard it works, and the most any band can be turned down. Real dB, not a percentage. Means the same as it always has.
- **Max Cut** (0 to 24 dB, default 24): a ceiling on the deepest cut at any spot and any moment. At 24 it does nothing, since Depth stops there. Depth 24 with Max Cut 6 and Depth 6 alone reach the same depth, but the first holds down 1.4 times as much of the area around a peak.
- **Quality** (Low Latency, Normal, High): see the table above and the numbers below. Changing it crossfades over 30 ms once the new setting has warmed up, up to about 115 ms in all, with no click.
- **Sharpness** (0 to 100%, default 50): notch width, a third of an octave at 0 to a 24th at 100.
- **Selectivity** (0 to 100%, default 50): how far a peak must stick out before it is treated. 0 treats small bumps, 100 only the tallest. In Hard mode it is how far a bin must stand out from its surroundings, on top of its level.
- **Attack** (1 to 200 ms, default 15) and **Release** (10 to 1000 ms, default 120): how fast the cut arrives and leaves, per band. The real response can't beat the 43 ms window.
- **Low Frequency** and **High Frequency** (20 Hz to 20 kHz, default 1000 and 12000): full effect inside, a sixth of an octave taper outside. Low above High does nothing.
- **Mix** (0 to 100%, default 100): processed against untouched, delay-matched.
- **Wet Trim** (-24 to +24 dB, default 0): level of the processed signal before Mix.
- **Output Gain** (-24 to +24 dB, default 0): after Mix.
- **Delta**: outputs only what is removed. Mix and Wet Trim apply, Output Gain does not, so `output / gain + delta = input` at any setting.
- **Bypass**: keeps the delay, crossfades over 20 ms.
- **Mode**: Soft judges a peak against the bands around it, so it works the same at any level. Hard uses a fixed level, so loud parts get treated and quiet ones don't. Hard is what you want for side chain carving.
- **Hard Threshold** (-80 to 0 dB, default -36): Hard mode only. A bin over this level is held down toward it, up to Depth. Compared after a 3 dB per octave lift above 1 kHz, so one number suits a voice's falling highs.
- **Held Notes** (Off or On, default Off): also takes down a single harmonic that rings on a held note. Off is the sound of 0.5.1 to the sample, so nothing saved before it existed changes. It only works in Soft mode, and an S, a breath or noise has no harmonics, so it leaves them alone. On costs about a fifth more CPU: 1.5% of one core off, 1.8% on, mono at 48 kHz on the MacBook with other work running. Numbers on real takes are under What was measured.
- **Stereo Mode**: Stereo or Mid/Side. Switches with no click. Does nothing over two channels.
- **Stereo Link** (0 to 100%, default 100): 100 means one gain curve for all channels, so the image can't wander. 0 means each channel on its own. Mid and side are always treated apart. In surround it joins every channel except the LFE.
- **Stereo Focus** (-100 to +100%, default 0): -100 puts all the treatment on left or mid, +100 on right or side. In surround it goes by left and right side channels, and center channels keep theirs.
- **Band 1 to 8** Frequency, Sensitivity (-12 to +12 dB, default 0), Width (0.1 to 4 octaves, default 1), On: make a range more or less sensitive. Default centres 300 Hz, 3 kHz, 7 kHz, then 150 Hz, 600 Hz, 1.5 kHz, 5 kHz, 11 kHz. Bands 1 to 3 start on and 4 to 8 start off. -12 turns treatment off at the centre of the band.
- **Band Listen** (Off, Dot 1 to Dot 8, default Off): hear only that dot's region, the same bell its width marker shows, half height at the marker's ends. It pairs with Delta: without Delta you hear the region after treatment, with Delta only what is coming out of it, and the two add up to the region. Mix and Output Gain act as they normally do. Bypass and Side Chain Listen win over it.
- **Sharpness Tilt** (-100 to +100%, default 0): positive makes the highs sharper and the lows wider.
- **Attack Tilt**, **Release Tilt** (-100 to +100%, default 0): positive makes the highs up to 4 times slower and the lows up to 4 times faster. Below about 500 Hz and above about 2 kHz the tilt is complete.
- **Side Chain**: detect from the key input instead of the signal. If no key is connected it falls back to the signal itself. **Side Chain Listen** plays the key, lined up with the main signal.

Read-only readings, real AU parameters so a host can read them by name. Never saved and never written as automation:

- **Max Reduction**: largest cut anywhere right now.
- **Max Reduction Frequency**: where it is. Reads None when nothing is being cut.

Parameter IDs are saved in sessions and automation. Never rename them.

## Presets

- **Gentle Vocal Polish**: the default settings.
- **Harsh Lead Vocal**: Depth 12, a little more sensitive around 3.2 kHz.
- **Vocal Stack Smoother**: wide and slower, Depth 9, for a group of voices.
- **Wide Stack Mid Side**: the same idea with mid and side apart.
- **Sibilance Soother**: fast, 4.5 to 14 kHz, Depth 10.
- **Boxy Low Mids**: slow, 180 Hz to 1.1 kHz.
- **Hard Whistle Catcher**: Hard mode, narrow and picky, catches a loud whistling peak.
- **Side Chain Carve**: Hard mode through the key. Put it on music with a vocal in the key to make room.
- **Tracking Low Latency**: Low Latency, Depth 8, fast. For singing through it while recording.
- **Eight Dot Vocal Sculpt**: all 8 dots in use, protecting the body of the voice and the air, and looking harder where a vocal gets harsh.
- **High Detail With Ceiling**: High, Depth 24, Max Cut 6. Strong and detailed, and never more than 6 dB anywhere.
- **Gentle Polish With Held Notes**: the default settings with Held Notes on.

## Surround

Layouts, with the same one in and out: mono, stereo, quad, LCRS, 5.0, 5.1, 6.0, 6.1, 7.0, 7.1, 5.1.2, 7.1.2, 5.1.4, 7.0.4, 7.1.4, 9.1.6. All pass auval. Unnamed channels are refused, since no one could say which is the LFE.

Every channel is analysed on its own, and Stereo Link joins them: at 100% each takes the deepest cut of them all. The LFE is never analysed or treated, cannot affect another channel, and comes out as the input, delayed, exact to the bit. It stays silent in Delta, Band Listen and Side Chain Listen, and Output Gain and Bypass apply to it as to the rest. A stereo key feeds left side channels from its left, right side channels from its right, and center channels from its left.

**Known limit, from JUCE:** its AU wrapper reads every 5.1 layout tag as L R C LFE Ls Rs. A host that sends another 5.1 order, such as the LFE last, would get channel 4 left untreated instead of the LFE. `tests/LayoutCheck.cpp` reports this. A plug-in never sees the tag, and JUCE is outside this repo.

## Signal flow

Windowed FFT (2048 at 48 kHz in Normal, one eighth hop, Hann in and out). Power per bin, smoothed 15 ms. The detector is the local peak around a bin, the reference is the average of the peak envelope a quarter to three quarters of an octave either side (closer in below 2 kHz). The excess is weighed by how long the peak has been sticking out, over 500 ms, because a resonance stays put and a passing formant does not. Then threshold, slope, Depth limit. Then Link, widening across frequency, attack and release per bin, light smoothing, Low and High range, Stereo Focus. Real gains on the spectrum, inverse FFT, overlap-add.

Harmonic spacing is tracked from the spectrum, so on a high note the peak envelope bridges the wider gaps, and the threshold rises a little.

With Held Notes on, in Soft mode, when the voice has clear harmonics (at least 4.5 FFT bins apart, about 105 Hz in Normal, so not in Low Latency below about 420 Hz), each bin is judged a second time against the two harmonics below it and the two above. That catches a held note's ring that the wide reference misses when the formants beside it are as tall. It has to clear its neighbours by 2 dB more than Selectivity asks, its long view is 100 ms at half weight, and it follows a harmonic a 24th of an octave either way as the note wavers. The deeper of the two reductions is used, and Depth is still the most any spot is cut. With Held Notes off none of this runs.

## Graph data

`SmoothResonanceProcessor::readGraph (smoothres::GraphFrame&)` copies the newest frame without ever blocking the audio thread: 256 log-spaced points from 20 Hz to 20 kHz, about 47 times a second, with input spectrum, output spectrum, gain curve per slot, and the sensitivity curve. `Max Reduction` and `Max Reduction Frequency` are plain atomics.

## What was measured

Everything is in `tests/results.txt`. Rerun with `zsh tests/run_all.sh` after `cmake --build build --target InstallAU -j 1`. Build each test first with `cmake --build build --target ProcessorTest NullTest FeatureTest DspTest SpeechTest HostCheck LayoutCheck -j 1`.

Held Notes on Luke's real takes (11 whole songs on a Rode NT1, 23.9 minutes of voiced sound, default settings, off then on). A ring here is one harmonic 10 dB or more over the two harmonics either side of it that stays put for a quarter of a second or more; there are 48, most at 1.8 to 1.9 kHz and 2.4 to 4.0 kHz.

- Rings: median cut 4.74 dB off, 5.86 dB on. The 20 between 2 and 5 kHz: 0.31 dB off, 4.96 dB on. Share cut by 3 dB or more: 58% off, 83% on.
- S sounds above 4 kHz: -1.27 dB off and on. Consonant onsets above 2.5 kHz: -0.55 dB off, -0.58 dB on. Note tails: the gain moves 0.05 dB a frame off, 0.06 dB on.
- High notes, first three harmonics: -0.01 dB off and on. Level: -0.05 dB off, -0.07 dB on. Latency: 2048 samples off and on.
- With it off, every sample of all 11 takes equals 0.5.1, at the default and at Harsh Lead Vocal, Boxy Low Mids and Eight Dot Vocal Sculpt.
- On 6 more takes kept unseen until the end, measured once on the build this judgement was developed on: rings 1.59 dB with the old sound, 5.43 dB with the new judgement. On one quiet, low take four of five rings still came down less than 0.3 dB.

Old sessions: the default, all 8 old presets and a session with every old control moved, loaded both ways, null exactly against renders saved from the 0.4.1 build (`tests/baseline/`). So do the default and presets on real speech at 44.1, 48, 88.2 and 96 kHz against a frozen copy of the 0.4.1 engine (`tests/legacy/`).

Each Quality at 48 kHz, stereo:

| | Low Latency | Normal | High |
|---|---|---|---|
| Latency | 10.7 ms | 42.7 ms | 85.3 ms |
| Time for a cut to reach half way after a tone starts | 5.3 ms | 13.3 ms | 24.0 ms |
| Size of one FFT bin | 93.8 Hz | 23.4 Hz | 11.7 Hz |
| Width of the cut at half depth, sharpest | 656 Hz | 211 Hz | 176 Hz |
| Cut comes back up between tones 400 Hz apart | 0 dB, one cut | 12 dB | 12 dB |
| Mean removed from a 12 dB boost at 3150 Hz, 4 voices | 5.8 dB | 6.3 dB | 6.6 dB |
| CPU, one core | 1.7% | 1.4% | 1.5% |

Low Latency cannot tell two peaks 400 Hz apart, and on low notes it cannot see separate harmonics at all.

A Quality switch runs two settings at once for a moment. Worst block of 128 samples during a switch, best of 5 tries: 9% in stereo, 38% in 7.1.4.

CPU per layout through the real AU at default, one core: stereo 0.8%, 5.1 2.0%, 7.1 2.9%, 7.1.4 4.7%, 9.1.6 6.4%. All measured with other builds running.

Real speech, 4 voices from macOS `say`, a +12 dB Q 6 boost added at a known frequency, Depth 12, the rest default. Reads as boost in the band, then how much came out:

- 1250 Hz: +9.5, removed 5.2 dB
- 2000 Hz: +9.7, removed 4.8 dB
- 3150 Hz: +9.7, removed 7.6 dB
- 4500 Hz: +9.2, removed 3.8 dB
- 6300 Hz: +9.0, removed 5.2 dB

On the same voices with no boost: overall under 0.15 dB, crest factor under 0.1 dB, Delta 20 to 23 dB below the voice, worst third octave from 1 to 8.5 kHz between 1.2 and 2.8 dB. The boost does not spread: under 0.9 dB elsewhere compared with the clean voice. These voices stop dead at 10.25 kHz with a peak just under it, and that peak gets cut by 2 to 9 dB.

Speech from `say` is not a singer. Neither speech nor any synthetic signal proves how it sounds on a real vocal. Things that still need ears are listed in the report.
