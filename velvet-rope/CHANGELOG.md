# Changelog

## 0.4.0

A more modern window, after Luke's first look. Keeps the blue voice with gold reduction, the layout and the graph as the hero. The glazed texture, bevels, dial bodies and dividers are gone: a calm near-flat dark base, thin rings, slim sliders, quiet pill switches, softer gold with no glow. Inter replaces Source Sans. The axes moved outside the plot.

The side panel now shows Depth, Sharpness, Selectivity, Attack, Release and Mode, with the rest behind an Advanced section that says how many of its controls are changed. Every control is still a real parameter with the same ID. Whether Advanced is open is saved with the session.

Window CPU is down from about 27% of one core to about 17%, at 45 frames a second instead of 60. The DSP is unchanged. New renders are in `docs/window-v2/`, and the old ones stay in `docs/window/`.

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
