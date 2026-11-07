# Changelog

## 0.5.0

Five new features. With the new controls at their defaults, the default sound and all 8 old presets null exactly against 0.4.1, as do old saved sessions. The window now reads Velvet Rope. The build and the AU are still named Smooth Resonance.

- **8 dots.** Dots 4 to 8 are new parameters, off by default, and all 8 have an On. Double-click the graph to add one. Hover a dot and click its x, or use Remove in the strip, or right-click it, to take it away.
- **Quality.** Low Latency (10.7 ms at 48 kHz), Normal (42.7 ms, the old sound) and High (85.3 ms). Automatable, with a clean crossfade and no old audio left behind.
- **Max Cut.** A ceiling on the deepest cut anywhere, 0 to 24 dB, default 24, which changes nothing. Depth keeps its ID, range and meaning.
- **Band Listen.** Solos one dot's region. With Delta it plays only what is coming out of that region. A solid banner on the graph says when it is on.
- **Multichannel.** Quad, LCRS, 5.0, 5.1, 6.0, 6.1, 7.0, 7.1, 5.1.2, 7.1.2, 5.1.4, 7.0.4, 7.1.4 and 9.1.6, in matching layouts. Channels link with Stereo Link. The LFE is only delayed. Mid/Side only works on stereo, and Focus goes by left and right side channels.

Three new presets: Tracking Low Latency, Eight Dot Vocal Sculpt and High Detail With Ceiling. The old presets keep their numbers. New renders are in `docs/window-v3/`. New tests: `tests/NullTest.cpp`, `tests/FeatureTest.cpp`, `tests/LayoutCheck.cpp`.

## 0.4.1

Window CPU pinned down and cut: 3.0% to 5.0% of one core over 8 runs, down from a swing of 3.2% to 17.4%. The swing came from which CPU cores macOS picks, not from the work. If macOS puts the window on the slow cores it still reads 7% to 9%. Findings are in `docs/window-cpu.md`.

The window now runs at 30 frames a second instead of 45, keeps everything that does not move with the sound in one cached picture, and only repaints what moved. Grid lines now draw over the gold instead of under it, which is too faint to see. The DSP is unchanged. `RenderWindow --live` now reports instructions as well as CPU time.

## 0.4.0

A more modern window, after Luke's first look. Keeps the blue voice with gold reduction, the layout and the graph as the hero. The glazed texture, bevels, dial bodies and dividers are gone: a calm near-flat dark base, thin rings, slim sliders, quiet pill switches, softer gold with no glow. Inter replaces Source Sans. The axes moved outside the plot.

The side panel now shows Depth, Sharpness, Selectivity, Attack, Release and Mode, with the rest behind an Advanced section that says how many of its controls are changed. Every control is still a real parameter with the same ID. Whether Advanced is open is saved with the session.

Window CPU is down from 27% of one core (measured with other builds running) to between 3% and 17% over 5 runs, about 9% in the last two, at 45 frames a second instead of 60. The DSP is unchanged. New renders are in `docs/window-v2/`, and the old ones stay in `docs/window/`.

## 0.3.0

The custom window. A live graph with the voice rising from the floor and gold reduction pouring from the ceiling, three draggable dots on the sensitivity curve, Low and High lines, a side panel, a footer with Bypass, Delta, Mix and Gain, and a one-line hint for every control. Resizable with a fixed shape, and the size is saved with the session. Replaces the generic editor.

Every parameter keeps its ID, name and range. The DSP is unchanged. The only new saved item is the window width, kept in range when loaded. Adds `tests/RenderWindow.cpp`, pictures in `docs/window/`, and window tests in `tests/ProcessorTest.cpp`.

## 0.2.0

First version that has been compiled and run. Tuned on real speech from macOS `say`, not only synthetic signals.

New controls, matching what commercial resonance suppressors offer: Mode (Soft or Hard), Hard Threshold, Stereo Link, Stereo Focus, three sensitivity bands (Frequency, Sensitivity, Width), Sharpness Tilt, Attack Tilt, Release Tilt, Wet Trim, Side Chain, Side Chain Listen. Read-only Max Reduction and Max Reduction Frequency. Eight factory presets. Spectrum, gain curve and sensitivity curve readable for a later graph window, lock free.

Changed in the DSP from the first draft: the detector follows the local peak rather than the mean; the reference sits further from the bin, clear of a resonance's skirts; a peak is judged partly on how long it has stuck out; a band edge can no longer read as a resonance; harmonic spacing is tracked so a high voice's harmonics are not treated as resonances.

Existing parameter IDs and their meanings are unchanged. Selectivity's threshold range moved from 0 to 12 dB to 0 to 11.5 dB, with an offset by frequency. Moved from the first draft on purpose: Link 100% keeps the old behaviour of one shared gain curve in Stereo mode.

## 0.1.0

First draft of the engine and wrapper. Never compiled.
