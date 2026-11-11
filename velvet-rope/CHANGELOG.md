# Changelog

## 0.7.1, 2026-11-11

Two things. The engine gives the same sound for about a third of the CPU: every sample that comes out is what 0.7.0 gave, at every setting, with Held Notes off or on. And two presets that thinned the voice, Boxy Low Mids and Eight Dot Vocal Sculpt, have the new values Luke said yes to on 2026-11-11. Nothing else sounds different: the default and the other ten presets are 0.7.0 to the sample. No control was added or changed, the latency is the same (2048 samples at 48 kHz in Normal), and sessions load as before, with the values they were saved with.

- **Why.** One test of the plug-in's own suite, "96 kHz stereo with moving controls" (limit 5% of one core), passed on a quiet Mac and failed on a busy one, where macOS puts the work on its slow cores. It was not one version's doing: 0.5.1, 0.6.0 and 0.7.0, timed back to back six times, read 3.96 to 5.45%, 3.87 to 5.53% and 4.15 to 5.23%. The limit, the test and the way it reads its result are untouched. The engine got cheaper.
- **Where the time went** (0.7.0, 96 kHz stereo, controls moving). 39% in two sliding maxima that cost most on noise, 18% in four pow() a bin every time Sharpness moved, 17% in the FFTs, 8% in three logarithms a bin, and all of it over all 2049 bins although nothing is ever cut outside Low and High, which at 96 kHz is 72% of them.
- **What changed in the engine.** The sliding maxima come from a table of the largest of every 2, 4, 8... bins, with no branch that depends on the sound. The detector, the judgement, the widening, attack and release, the gains and the readings only run over the bins between Low and High, and the reference only as far up as those bins look. Sharpness's widths are whole-number tables built once, with two pow() where there were four a bin. A bin that is plainly no taller than its surroundings is told so from the powers, and the logarithms are only taken for a bin that stands out. While nothing is ramping, each channel's samples go through in one run per frame. The range mask, the wipes and the graph's spectrum only touch what is used.
- **The numbers** (std::clock, the test's own loop, with four other agents working on the Mac). 96 kHz stereo with moving controls: 4.24 to 6.00% in 0.7.0, 1.24 to 1.38% now. With nice 20: 5.19 to 5.44%, now 1.37 to 1.41%. Forced onto the slow cores alone (`taskpolicy -b`): 9.2 to 13.6%, now 3.4 to 5.5%. The 48 kHz line: 2.42%, now 0.75%. Through the real processor at 48 kHz (FeatureTest, wall clock): stereo Low Latency 1.29 then 0.85%, Normal 1.53 then 1.19%, High 1.69 then 0.79%; Band Listen 2.31 then 0.83%; mono 1.01 then 0.28%; 7.1.4 6.10 then 4.15%; 9.1.6 8.00 then 5.38%.
- **What is left** is mostly the FFT, which is the same call as before: 60% of the cost on the slow cores. The same transform on split data would cost 2.8 instead of 7.0 millionths of a second, but its last bit differs in every frame, so it would not be the same sound to the sample, and it is not in this version.
- **Held Notes on** costs less than off did in 0.7.0: 2.70 to 2.90% then 1.59 to 1.79% of one core (stereo, 48 kHz, a sung note, busy Mac; off: 2.08 to 2.27% then 0.90 to 0.95%). Its own part of the work was not made cheaper, so on is now about 1.8 times off where it was 1.3 times.
- **Proof that the sound is the same.** The new engine and a frozen copy of 0.7.0's, side by side, bit for bit: 510 runs on random signals and pieces of real takes (1 to 8 channels, 44.1 to 192 kHz, blocks of 1 to 4096 samples, every control still, stepping or gliding, Quality switches, the side chain, Band Listen, resets), 0 differ in any output sample, reduction reading or graph frame. All 17 Rode takes whole at five settings, 680,796,000 samples: 0 differ. NullTest is unchanged and passes.
- **Boxy Low Mids, new values.** Depth 8 to 3, Selectivity 45 to 80, Low 180 to 300 Hz, dot 1 at 400 Hz from +3 to 0 dB. Why: from 300 Hz to 1.1 kHz the first harmonics of a voice are its body, and each of them reads as a peak. On Luke's 11 working takes, voiced sound against the dry take, old values then new: the fundamental -1.78 then -0.25 dB, 180 to 360 Hz -3.32 then -0.22 dB, 360 to 720 Hz -4.92 then -0.51 dB, 720 Hz to 1.4 kHz -0.62 then -0.06 dB, level -2.64 then -0.28 dB (worst take -3.37 then -0.45), the first three harmonics of high notes -5.04 then -0.84 dB, high-note frames that lose over 3 dB 42.3% then 0.1%, the whole level swinging 5.90 then 1.23 dB. The rule that a preset takes no more than 2 dB off the body failed in all 11 takes and passes in all 11. On high notes take 02 still loses 2.72 dB, take 06 1.68 and take 14 1.60.
- **Eight Dot Vocal Sculpt, new values.** Dot 1 at 300 Hz from -8 to -12 dB, dot 4 at 500 Hz from -5 to -8 dB, dot 5 at 900 Hz from +1 dB over 0.7 octaves to -6 dB over 1 octave. Old then new on the same takes: the fundamental -0.34 then -0.02 dB, 720 Hz to 1.4 kHz -0.79 then -0.14 dB, level -0.31 then -0.13 dB, the first three harmonics of high notes -1.13 then -0.11 dB (take 02 -3.83 then -0.15, take 14 -3.38 then -0.53), high-note frames that lose over 3 dB 16.7% then 7.5%, the whole level swinging 1.72 then 0.80 dB. The body rule failed on take 14 and passes on all 11. Above 1.4 kHz it works as before: 1.4 to 2.9 kHz -2.52 then -2.06 dB, 2.9 to 5.8 kHz -3.74 dB both, S -2.10 dB both.
- **Sessions.** A session saved with either preset keeps the values it was saved with: the state holds the values, not the preset's number.
- **Tests re-pinned for the two presets, no limit changed.** NullTest's line "preset-5: against the saved 0.4.1 render" pinned Boxy Low Mids' old sound. It now reads "preset-5: Boxy Low Mids with the values of 0.7.1, against the saved 0.7.1 render" and compares with `tests/baseline/preset-5-v0.7.1.f32` (same limit, -90 dBFS; the new values are 7.7 dB away from the old render). The line beside it, the session 0.4.1 saved with that preset against the 0.4.1 render, is unchanged and passes. New in NullTest: both presets load the approved values by name, and a session holding the old Eight Dot values keeps them. No other test mentions either preset's values.
- **Other tests.** None was changed for the CPU work. One line of FeatureTest can still fail on a busy Mac and it is not the plug-in's doing: "Worst single block" times each block with the wall clock, so a moment in which macOS takes the thread off its core counts whole. Measured beside it with the thread's own CPU time: in 300 runs under load 9 had a block over the 50% limit by the wall clock (the worst 3313%), while no block of those runs cost more than 6.6% in CPU time (0.7.0: 15 of 300, and 10.7%). `tests/RingProbe.cpp` has a third view, `spacing`.

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
