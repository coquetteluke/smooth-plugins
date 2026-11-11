# Changelog

## 0.6.0, 2026-11-11

One new control, **Held Notes**, off by default. With it off nothing changes: every sample is the 0.5.1 sound, and sessions saved before load and play as they did.

- **What it does.** On, in Soft mode, it also takes down a single harmonic that rings on a held note. 0.5.1 only judged a peak against the spectrum a quarter to three quarters of an octave away, so it missed a ring whose neighbouring formants are as tall, and it forgot one as the note wavered. Held Notes judges each bin a second time, against the two harmonics below it and the two above. Hard mode ignores it, and an S, a breath or noise has no harmonics, so it leaves them alone.
- **On Luke's real takes** (11 whole songs on a Rode NT1, default settings, off then on). Rings that last a quarter of a second or more (48 of them): median cut 4.74 then 5.86 dB. The 20 between 2 and 5 kHz: 0.31 then 4.96 dB. Share cut by 3 dB or more: 58% then 83%. S above 4 kHz -1.27 dB both. Consonant onsets -0.55 then -0.58 dB. Note tails: the gain moves 0.05 then 0.06 dB a frame. High notes, first three harmonics, -0.01 dB both. Level -0.05 then -0.07 dB.
- **Off is 0.5.1.** NullTest is unchanged and passes. All 11 takes rendered by the installed 0.5.1 and by this build with Held Notes off are equal sample for sample, at the default and at three presets.
- **What on costs.** About a fifth more CPU: 1.5% then 1.8% of one core, mono at 48 kHz, measured with four other jobs running. The worst single block read 7 to 9% off and 9 to 11% on in quiet rounds; on that loaded Mac single blocks of either jumped to 20% and more now and then. Off costs nothing. Latency is unchanged, 2048 samples in Normal.
- **In the window.** A Held notes switch at the end of Mode's row. It goes grey in Hard mode, where it does nothing. Pictures are in `docs/window-v4/`. The window's own CPU is unchanged: 4.6% to 4.9% of one core at 30 frames a second, 2.9% in fast core terms.
- **A real parameter.** New ID `heldNotes`, named Held Notes, added last. No existing ID, name, range or choice changed, and Mode still has two choices. The AU codes are the same.
- **One new preset, added at the end:** Gentle Polish With Held Notes, the default settings with Held Notes on. Presets 0 to 10 keep their numbers and their sound, and each turns Held Notes off.
- **Tests.** New `tests/HeldNotesTest.cpp`, written first: 24 of its checks failed on 0.5.1. It covers the control, off against the frozen old engine, a 12 dB ring on synthetic held notes coming down 5.4 to 5.8 dB (limit 3), S and T left alone, old sessions loading with it off, switching without a click, the presets and the window. Every older test still passes.
- **Not done.** The two limits an earlier pass loosened (DspTest at 330 Hz, SpeechTest's least voice) are not restored: they do not pass with Held Notes on either, since they are about fixed boosts on speech. New values for Boxy Low Mids and Eight Dot Vocal Sculpt wait for Luke on the branch `sung-resonances`.

## 0.5.1, 2026-11-07

Renamed to Velvet Rope, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

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
