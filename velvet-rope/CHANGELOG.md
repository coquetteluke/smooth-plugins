# Changelog

## 0.7.0, 2026-11-11

Held Notes does its job on the rings it used to pass over. With Held Notes off nothing changes: every sample is still the 0.5.1 sound. No new control, and sessions saved by 0.4.1, 0.5.1 and 0.6.0 load as they were.

- **Why it missed some rings.** 0.6.0 only watched a harmonic while it was close in level to the loudest thing in the whole voice, which is the fundamental. The line said 50 dB but worked out as 25 dB. On a low or dark voice a ring at 3 to 5 kHz is 28 to 40 dB under the fundamental, so it was never watched and came down by 0 to 2 dB. That was 9 of the 12 rings it missed on Luke's takes.
- **What changed.** Held Notes now goes by the loudest thing between Low and High, and reaches a real 50 dB under it. It counts how long a harmonic has stood over its neighbours: nothing is taken off before 110 ms, all of it from 170 ms, and the count follows a harmonic that wavers. Its cut is as narrow as the harmonic, so the harmonics beside the ring are left alone. On high notes, where harmonics are more than 200 Hz apart, a resonance wide enough to lift two or three harmonics together is no longer let off lightly once it has stayed.
- **In the window.** The part of the gold that Held Notes adds is drawn in rose gold, so the switch can be seen working. Pictures are in `docs/window-v5/` (`held-notes-ring-off` and `held-notes-ring-on` show the same ringing note).
- **The limits an earlier pass loosened.** DspTest's first limit, a +15 dB resonance coming down 5 dB at every pitch up to 330 Hz with the clean vowel within 2 dB, passes again with Held Notes on: -5.8, -5.8, -6.5 and -5.4 dB at 110, 165, 220 and 330 Hz. SpeechTest's first limit, the worst of four voices losing half a boost, cannot be met by anything that only hears the boosted voice: the voice that loses least has a dip of its own there, so the boost only fills it and nothing sticks out. The test now prints that proof and holds the first limit wherever the boost can be seen. The loosened checks of the old sound stay as they are, because that sound is frozen for saved sessions.
- **On Luke's real takes** (11 whole songs on a Rode NT1, default settings; off, then on in 0.6.0, then on in 0.7.0). Rings that last a quarter of a second or more (48 of them): median cut 4.74, 5.86, 5.92 dB. Share cut by 3 dB or more: 58%, 83%, 94% (45 of 48). The tenth of them cut least: 0.00, 0.31, 4.77 dB. The 20 between 2 and 5 kHz: 0.31, 4.96, 5.50 dB. S above 4 kHz -1.27, -1.27, -1.28 dB (limit 1.5). Consonant onsets -0.55, -0.59, -0.60 dB. Note tails: the gain moves 0.05, 0.06, 0.06 dB a frame. High notes -0.01 dB in all three. Pumping between 1 and 10 kHz: 2.60, 2.90, 2.92 dB (limit 3.0), which is the limit that keeps Held Notes from acting sooner. The three rings still under 3 dB get 2.2 to 2.5 dB.
- **Checked once at the end on takes it was not tuned on.** Six more Rode takes (11 rings): cut by 3 dB or more 45% off, 64% in 0.6.0, 91% now, everything else inside its limit. The first session's 11 takes on the QuadCast (87 rings): 49%, 57%, 97%. On that session the pumping between 1 and 10 kHz is over its 3.0 dB limit with Held Notes off already (3.18 dB), and on adds 0.25 dB: not fixed here.
- **What on costs.** About 1.3 times the CPU of off: 1.34% then 1.75% of one core over the whole working set, and 1.35% then 1.70% in a second round, mono at 48 kHz with four other jobs running. Worst single block of the whole set: 6.9% then 9.4%, and 8.5% then 9.7%. Alternating off and on six times on two takes, the middle reading of the worst block was 4.1% then 5.7% and 5.3% then 7.0%. Off costs nothing. Latency is unchanged, 2048 samples in Normal. The window does the same work as before: 7.7 million instructions a frame at 30 frames a second, 2.9% of a fast core, with Held notes on or off.
- **Off is 0.5.1.** NullTest is unchanged and passes. All 11 takes rendered by a 0.5.1 build and by this build with Held Notes off are equal sample for sample, at the default and at three presets.
- **Tests.** Written first and failing on 0.6.0: synthetic held notes with a ring 34 to 43 dB under the loudest harmonic (0.0 dB then, 3.9 to 5.8 dB now, limit 3). New: two sessions saved by the installed 0.6.0 load unchanged, and the graph data marks Held Notes' part. `tests/RingProbe.cpp` is a tool, not a test: it prints what the engine sees at one spot of a recording.

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
