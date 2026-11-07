# Smooth Resonance

Finds the narrow peaks that stick out of a voice and turns them down only while they are there. Written from scratch from the general technique: an overlapping STFT, a comparison of each band with the peaks around it, and gain smoothed over time and frequency. No license system.

## How to load it

Audio Units > Luke Garito > Smooth Resonance. AU code `aufx SmRs Luke`. Mono and stereo, with an optional side chain input. Latency is 2048 samples at 44.1 and 48 kHz (42.7 ms at 48 kHz) and 4096 at 88.2 and 96 kHz, reported to the host, and the same when bypassed.

## The window

Concept: a lapis glazed instrument repaired in gold. Your voice is a mountain of blue light standing on the floor of the graph, and whatever is being turned down pours from the ceiling and shaves off the voice in gold. Gold only ever means "what is being taken out".

- **Graph:** frequency across, 20 Hz to 20 kHz. The bright edge of the voice is the sound after treatment. The gold scale on the right is reduction in dB, after Mix. The blue tilt of 4.5 dB an octave is for display only.
- **Dots 1, 2, 3:** the sensitivity curve. Drag a dot to move it. Up treats more there, down protects it. Scroll on a dot, or hold Command and drag, for width. Hold Shift for a finer drag. Double-click to reset.
- **Low and High lines:** drag to set the range. Nothing outside them is touched.
- **Strip under the graph:** Low, High, and the values of the selected dot.
- **Side panel:** Depth, Sharpness, Selectivity, Attack, Release, Mode, Threshold, Stereo, Link, Focus, the three tilts, Wet trim, Side chain.
- **Footer:** Bypass, Delta, Mix, Gain, and a one-line hint for whatever is under the mouse.
- **Every control:** drag it, double-click to reset it, click its value to type one (`3k` works for a frequency), hold Shift when you start a drag for a finer one.
- **Size:** drag the bottom right corner, 870 to 1856 points wide, always the same shape. The size is saved with the session.

Every control is a real parameter with its old ID, name and range, so automation moves the window and the window moves them, with begin and end of gesture. The window keeps no sound settings of its own. Its graph data comes from the lock free `readGraph`, so it cannot block the audio thread.

Pictures are in `docs/window/`, made by `build/RenderWindow_artefacts/Release/RenderWindow`, which runs real speech from macOS `say` with resonant boosts through the real processor and renders the real window to PNG with no host. `RenderWindow --live` opens a real window with all but one column of pixels off the screen, and measures frames a second and CPU.

Measured on the 16 GB MacBook with other builds running: 60 frames a second, and about 27% of one core while the window is open with audio playing. That is the drawing and the screen's share together. With nothing moving it does not redraw. Closed, it costs nothing.

The window's font is Source Sans Pro by Adobe, under the SIL Open Font License. See `assets/fonts/OFL.txt`.

## Try first on a harsh vocal

1. Load the preset **Harsh Lead Vocal**. Depth 12, Selectivity 50, 1.2 to 10 kHz.
2. Turn on **Delta**. If you can make out words in it, it is working too hard: raise Selectivity or lower Depth. Turn Delta off.
3. For a whole stack, use **Vocal Stack Smoother** on the group.

## Controls

- **Depth** (0 to 24 dB, default 6): the most any band can be turned down. Real dB, not a percentage.
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
- **Stereo Mode**: Stereo or Mid/Side. Switches with no click.
- **Stereo Link** (0 to 100%, default 100): Stereo mode. 100 means one gain curve for both channels, so the image can't wander. 0 means each channel on its own. Mid and side are always treated apart.
- **Stereo Focus** (-100 to +100%, default 0): -100 puts all the treatment on left or mid, +100 on right or side.
- **Band 1, 2, 3** Frequency, Sensitivity (-12 to +12 dB, default 0), Width (0.1 to 4 octaves, default 1): make a range more or less sensitive. Default centres 300 Hz, 3 kHz, 7 kHz. -12 turns treatment off at the centre of the band.
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

## Signal flow

Windowed FFT (2048 at 48 kHz, one eighth hop, Hann in and out). Power per bin, smoothed 15 ms. The detector is the local peak around a bin, the reference is the average of the peak envelope a quarter to three quarters of an octave either side (closer in below 2 kHz). The excess is weighed by how long the peak has been sticking out, over 500 ms, because a resonance stays put and a passing formant does not. Then threshold, slope, Depth limit. Then Link, widening across frequency, attack and release per bin, light smoothing, Low and High range, Stereo Focus. Real gains on the spectrum, inverse FFT, overlap-add.

Harmonic spacing is tracked from the spectrum, so on a high note the peak envelope bridges the wider gaps, and the threshold rises a little.

## Graph data

`SmoothResonanceProcessor::readGraph (smoothres::GraphFrame&)` copies the newest frame without ever blocking the audio thread: 256 log-spaced points from 20 Hz to 20 kHz, about 47 times a second, with input spectrum, output spectrum, gain curve per slot, and the sensitivity curve. `Max Reduction` and `Max Reduction Frequency` are plain atomics.

## What was measured

Everything is in `tests/results.txt`. Rerun with `zsh tests/run_all.sh` after `cmake --build build --target InstallAU -j 1`. Build each test first with `cmake --build build --target ProcessorTest DspTest SpeechTest HostCheck -j 1`.

Real speech, 4 voices from macOS `say`, a +12 dB Q 6 boost added at a known frequency, Depth 12, the rest default. Reads as boost in the band, then how much came out:

- 1250 Hz: +9.5, removed 5.2 dB
- 2000 Hz: +9.7, removed 4.8 dB
- 3150 Hz: +9.7, removed 7.6 dB
- 4500 Hz: +9.2, removed 3.8 dB
- 6300 Hz: +9.0, removed 5.2 dB

On the same voices with no boost: overall under 0.15 dB, crest factor under 0.1 dB, Delta 20 to 23 dB below the voice, worst third octave from 1 to 8.5 kHz between 1.2 and 2.8 dB. The boost does not spread: under 0.9 dB elsewhere compared with the clean voice. These voices stop dead at 10.25 kHz with a peak just under it, and that peak gets cut by 2 to 9 dB.

Speech from `say` is not a singer. Neither speech nor any synthetic signal proves how it sounds on a real vocal. Things that still need ears are listed in the report.
