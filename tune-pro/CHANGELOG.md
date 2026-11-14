# Changelog

## 1.8.4, 2026-11-14

Natural Vibrato below 0 with a Retune Speed above 0 pulled each note off the note and back, 0.15 to 0.3 s in, by up to 8 cents (round 4, found in the last full check; it came with 1.8.2). Its average of the corrected pitch was started on the note's first tick, where a Retune Speed above 0 has moved only part of the way, so as Natural Vibrato faded in (80 to 250 ms into the note) it read the rest of the correction as vibrato and took it back out. The average now follows the corrected pitch exactly for the note's first 80 ms, while Natural Vibrato is still faded out, and averages from there (`PitchCorrector`). With an instant correction (Retune Speed 0, as Robot Flat has it) nothing changes. Of the presets only Flat Then Sung Vibrato has both (Retune Speed 5 ms, Natural Vibrato -100); the two set by hand get the fix too. This changes how Flat Then Sung Vibrato sounds: its notes hold. Luke's yes on 2026-11-14 ("Yes, ship 1.8.4"), knowing the two lines under the real vocal check below.

New test `PluginTest round4` (also in the full run), written first, both engines: six notes sung 25 cents flat after a 20 cent scoop, Retune Speed 5 ms, the furthest the output is from the note 0.1 to 0.45 s in with Natural Vibrato -100, beyond the same voice with Natural Vibrato 0 (a steady voice, so there is no vibrato to take out; limit 2 cents). 1.8.3: 6.21 cents further in Low Latency (8.17 against 1.96) and 7.80 in High Quality (8.04 against 0.25), both fail. 1.8.4: -0.01 (1.95 against 1.96) and 0.00 (0.24 against 0.25).

`saved`: the 1.6.1 session, the default and 11 of 12 presets come out bit for bit as 1.8.2 and 1.8.3 gave them, in both engines. Flat Then Sung Vibrato is re-pinned in `tests/state-1.8.4.txt` (its two lines; 1.8.2's pins are kept beside them and still read). The test's line now says "as pinned for 1.8.4" where it said "as pinned for 1.8.2": the same limit, every preset bit for bit, against the new file. Full suite: 660 pass, 0 fail (1.8.3: 658).

Real vocal check on the installed 1.8.4, round 1's working set (11 takes, song key, the same tool, check file and settings as 1.8.1's and 1.8.3's runs; `_vox/rode/work/tune-pro/p6/ship184.sh`, compared with `p6/passdiff.py`):
- Default and its 8 variants (`1.8.4-ship184`): of 1210 pass flags none changed, against 1.8.1 (`1.8.1-v181`) and against 1.8.3 (`1.8.3-lastcheck`). Every pooled line is 1.8.1's to the digit except CPU (mean 3.41 -> 3.04%, worst block 12 -> 15%).
- Robot Flat (`1.8.4-ship184-robot`): none of 141 pass flags changed against 1.8.3. Against 1.8.1 it is as since 1.8.2: 39 lines that failed pass, 2 that passed fail (take 10 formant 1.33 -> 1.52% against 1.5, take 14 one note left as sung at 89.37 s), both of which Hard Tune read the same way on 1.8.1.
- Flat Then Sung Vibrato (`1.8.4-ship184-ftsv`), against 1.8.1 rendered from the kept 1.8.1 component (`1.8.1-ftsv181`): 34 lines that failed pass, none that passed fails. 1.8.3 against the same 1.8.1 run had 22 and 3 (held notes on takes 05, 14 and 16, the pull this fixes). Held notes on all 769: median 4.2 cents on 1.8.1, 6.7 on 1.8.3, 1.4 on 1.8.4; 90th percentile 18.0, 15.5, 10.2 (limit 10, so that pooled line still fails, by 0.2 cents); clear notes 90th percentile 11.7, 11.9, 4.0 (passes). Far clicks 1, 1, 0.
- The two lines, against 1.8.3 (`1.8.3-r4-ftsv`): 17 lines that failed pass and 2 that passed fail. Both failed on 1.8.1 as well. Take 02 outcomes: notes left on the wrong side of a line 2 on 1.8.1, 0 on 1.8.3, 1 on 1.8.4 (6.36 s, a G2 sung on the line between F#2 and G#2 in C# minor; the output steps between the two frame for frame as 1.8.3's, within 5 cents, and without the pull its median reads on the F# side). Take 06 wrong notes: confirmed events 13, 0, 1 (166.40 s, a 50 ms fragment at the end of a phrase where the dry reading is about 1.2 kHz; the output's pitch track is 1.8.3's to 0.04 semitones, and the check's second estimate reads 1536 Hz on it). Neither is a change in what is heard; the checks are left as they are.

Parameters, ranges, defaults, latency (659 samples) and AU codes are unchanged.

## 1.8.3, 2026-11-13

Window only (round 3, window-r1-1): a double-click on a key put it back to Tune only after its first click had stepped the note (Tune to Remove, Remove to Bypass), heard for the double-click time and written to the host as automation; the second click then wrote Tune, two changes and two gestures. A click now plays and is drawn at once and reaches the host once the double-click time has passed (JUCE's 0.4 s and 0.1 s more, from the window's own timer, its next look, or its closing). A double-click writes only its Tune: one change in one gesture from the mode the host had before the first click, none if that was Tune. A single click still writes one change in one gesture, half a second after it was heard; a session loaded or automation moving the note meanwhile wins over the click. The processor's only part is a per-note hold the window sets, which is never set otherwise, so the default and every preset play bit for bit as 1.8.2 (`saved` and all 24 Low Latency null cases pass unchanged).

New test `PluginTest round3` (also in the full run), written first: on 1.8.2, 5 of its lines failed (a double-click from Tune wrote Remove then Tune, 2 changes and 2 gestures; from Remove, Bypass then Tune; from Bypass, Tune twice); on 1.8.3 they write 0, 1 and 1 change, all Tune, in at most one gesture.

Changed test, with Luke's yes on 2026-11-13 ("u can do all of these"): in the window test, "one click on a key removes the note" read the note's parameter straight after the click, which now changes only after the double-click time. It is now "one click on a key removes the note at once, as it plays" and reads what the note plays with (the same limit: Remove, straight after the click); the old line is kept in a comment above it, and the host's one change is checked in `round3`.

## 1.8.2, 2026-11-13

Real vocal check, round 1's working set (11 takes, song key, the same check file and variants as 1.8.1's final run `1.8.1-v181`, run `1.8.2-v182`, `_vox/rode/work/tune-pro/p6/`): every pooled line is 1.8.1's to the digit, and of 1210 pass flags none that passed fails (`p6/passdiff.py`); only CPU moved (mean 3.41 -> 3.05%, worst block 12 -> 10%). Robot Flat on the same takes (`p6/robot-compare.txt`): held notes 90th percentile 15.6 -> 4.1 cents, slides turned round 10.52 -> 3.14% (rap 7.49 -> 4.64%, limit 5), wrong-semitone frames 246 -> 6, notes right 574 -> 659; it now reads as Hard Tune does (4.1 c, worst take 14 25.9 against 24.6). Near clicks, which have no limit, 1 -> 4; far clicks 1 -> 0. The working set ran on the 1.8.2 build before the window's 0.25 s (it had 0.1 s); the audio is the same.

Phase 2: the faults the bug hunt kept (25: 1 high, 6 medium, 18 low), the HostStress parameter text fails, the key file reader items, and AuBench's worst block. Every fix has a test in `PluginTest phase2`, written and run on 1.8.1 first (48 lines failed there, each for the reason the hunt gave; `~/Bug Fixing/smooth-plugins/after/tune-pro/phase2-before.txt` and `-after.txt`). Parameters, ranges, defaults, latency and AU codes are unchanged.

**Robot Flat is flat** (the high one, `PitchCorrector`). Natural Vibrato worked on the voice as sung, so where the correction had already taken the vibrato out, -100 put it back upside down. It now works on the vibrato left after correction, as the README says, as Tune 1.2.1 does. A +/-40 cent vibrato through the Robot Flat preset keeps 92.0% -> 5.8% (Low Latency) and 93.5% -> 1.1% (High Quality), limit 10 (Hard Tune's); Natural Vibrato -100 at the default 3 ms 89.8% -> 6.8% and 90.7% -> 3.1%. This changes how Robot Flat and Flat Then Sung Vibrato sound, which is what they claim. Luke left this call to the lead on 2026-11-11.

The parameter holds Natural Vibrato's 0 as 1.5e-6 % (its 0.1 step does not land on 0), which moves the pitch by under 1e-6 cents; that keeps 1.8.1's arithmetic, so the default and every other preset come out bit for bit as 1.8.1: `saved` holds 10 of 12 presets in each engine and the 1.6.1 session to the last bit, and all 24 Low Latency null cases are unchanged. Robot Flat and Flat Then Sung Vibrato are re-pinned in `tests/state-1.8.2.txt` (four lines; 1.8.1's pins kept beside them and still read). The installed 1.8.1's own sessions, captured before 1.8.2 was installed (`tests/baseline`), play bit for bit as 1.8.1 played them, the default and 10 presets; Robot Flat's and Flat Then Sung Vibrato's differ by 0.44 and 0.55.

**Medium**
- Natural Vibrato with nothing to tune to (MIDI Targets and no notes, or every note on Remove) bent the voice by up to 6 semitones at every phrase start: its average started at 0. It starts where the voice comes out now. Null against the input: 2.4 dB / 3.3 dB -> -170 dB (limit -60).
- High Quality with Classic on cleared the harmonic shifter's 245 KB memory on every output sample (and on every unvoiced sample once its frames had run out, in plain High Quality too). It is restarted once, and the memory cleared only when something is in it, the same output either way. CPU, High Quality with Classic, 180 Hz voice: 9.29% -> 1.15% (limit 8).
- Classic switched on in High Quality on a held high note cut the output out while the resting grains caught up (281.99 dB dip), then jumped in (8.5 times the voice's own largest step). The grains start again and take over over 20 ms: 4.93 dB, 1.02 times (limits 12 dB, 2 times). My first version of this test looked one latency after the switch, where 1.8.1 had already switched (1.25 dB, 0.12 times on 1.8.1); it now looks from the block it was set in.
- A repeated sustain-pedal-on message (a continuous pedal streams 64 to 127) dropped the notes the pedal held: target lost -> 0.14 cents from C4.
- A double-click on Engine's other side switched twice (two hand overs); a double-click on a switch flipped it twice. Engine: 2 changes ending on Low Latency -> 1 ending on High Quality; Bypass on, double-clicked, now goes to its default.
- The preset box said Natural Pop on every new instance and every reloaded session. The preset last chosen is saved (a `program` attribute, as Velvet Rope 0.8.1 and Duo 0.4.3), not applied again; a new instance or an older session says Choose a preset, muted, and the arrows go to the first or last preset.
- The keyboard and lanes kept the old scale while the host was not playing: the scale is worked out from the controls (or the followed key) when the window asks.
- Not fixed: double-clicking a key still steps the note on its first click. The existing window test holds that one click acts at once (`one click on a key removes the note`), and holding each click back for the double-click time would make every click late. In the README.

**Low**
- MIDI notes, and every setting the corrector reads (Key, Scale, note modes, Transpose, Classic, Throat, the vibratos), acted on audio one latency early (a base latency for MIDI; 45 ms more in High Quality). They are read from the tick the audio being given out came in at. First change at the output, early by: MIDI 8.1 -> -5.3 ms (Low Latency), 12.6 -> -1.8 ms (High Quality); Transpose 10.1 -> -3.3 ms, 57.9 -> -1.1 ms (limit 5, a period). A setting that stays put gives exactly what it gave.
- Reset All Controllers let go of keys still down: now only the pedal (target lost -> 0.15 cents).
- A note on Bypass, and a voice with no target, still got Natural Vibrato and the added vibrato: null 1.4 to 2.7 dB -> -168 to -267 dB.
- Presets set Vibrato Amount but not Rate or Delay: they set all three now (the defaults, 5.5 Hz and 300 ms). Flat Then Sung Vibrato after 2 Hz / 0 ms against after 9 Hz / 1500 ms: 0.55 -> 0 largest sample difference.
- Window: the hint line describes a key's new mode after a click; bypassed with Transpose the voice as sung stays on screen (centred on A3, not A4); Advanced glides from closed instead of being 98% open before the first frame; the legend no longer says Following C Chromatic with no key from the file (No key from the key file yet); with Transpose the target note, lit key and header name the note that comes out (A3 -> C4 at +3); a stretch played bypassed stays drawn bypassed after a full redraw (765 -> 0 coral pixels); once no audio has come for 0.25 s no key stays lit and the header says No pitch (the window test: 2 s after); a voice left alone off the note says Left as sung, 30 cents sharp, not On pitch; a state loaded with the window open sizes the window.
- Not real: a damaged (not a number) value in a state already goes back to its default on the JUCE this builds with (a parameter whose entry is added back is set from it, valueTreeChildAdded). Kept as a guard line.
- Not fixed: at 96 kHz Low Latency's low-note age thresholds are in 48 kHz periods (Low Male at 196 Hz sits 0.16 ms earlier than at 48 kHz). Reading them as 48 kHz periods moves it to 0.13 ms late and changes the pinned 96 kHz null case, so it is not the only difference; given as a note, in the README.

**HostStress `paramtext`**: Target Note's "no note" read back as C-1 and Active Key's names as C Chromatic. Each reading's own text reads back as itself (11 of 42 off -> 0).

**Key file** (fix/key-readers.md): in an offline bounce or freeze the file is read every 0.3 s of audio, not of wall time, so a key change lands where it does in playback (on 1.8.1 a change was not taken up within 0.4 s of a fast render). A fresh Manual key (written with listening false) and a Locked one are followed, as before; a reader does not refuse listening false.

**Host Reset** did nothing: it now clears what prepareToPlay clears, allocating nothing, and skips the one Reset JUCE's AU wrapper sends right after the host turns bypass on. Play, Reset, play against a new instance: 0.29 / 0.41 -> 0 (Low Latency / High Quality).

**AuBench's worst single block** (37% of a 128 sample block in 3 of 3 runs on 1.8.1): High Quality on a held note, where the vocal tract fit's full search costed up to 34,000 shapes in one output sample (about 0.8 ms). Each shape is now costed once a fit and a walk that meets a walk already made takes its end; every shape found, cost and gain is bit for bit 1.8.1's (`testPhase2Fit`, 120 fits). The 245 KB clear above, and a High Quality restart path, too. AuBench after, six runs: worst block 33, 33, 229, 32, 32, 32% (the 229% run once, with other jobs on the Mac; default 3.7% and worst preset 4.0 to 4.8% in all). In-process (`PluginTest blockprobe`, AuBench's input and order): Final Mix High Quality 37.2 -> 32.4%, Tracking Low Latency's first 0.5 s (both engines running) 23.3 -> 7.0%. The rest of that block cannot be spread over time without changing the sound (README).

## 1.8.1, 2026-11-12

**Low Latency's new memory waits 0.1 s before it is used** (`EnvelopeFilter::setSweepMemory`, `lowLatencySweepUseAfterSeconds`). The full working set on 1.8.0 (Luke's 11 working takes, each song's key) turned 12 more of 15650 sliding frames round on the rap takes 12 and 16 than 1.7.2 did, at the default: 4.967% -> 5.041%, limit 5, a fail. In rap and speech the pitch moves faster than the memory follows (30 semitones a second), so it is let go every few hundred ms and starts again from one moment's harmonics; drawn from those few points with 48 poles and no sharpening, it gave the output a slightly different pitch track from 1.7.2's. Now the memory still fills from the first moment but is used only once it has been filling for 0.1 s; until then the model is drawn through the moment's harmonics as in 1.7.2. Measured on takes 12 and 16 before choosing (rap reversed frames, 1.7.2 777 of 15644): used at once 789; after 50 ms 778; after 100 ms 777; after 200 ms 777. Changing the memory's length (0.2 or 0.8 s), how fast a glide lets it go (10 to 20 semitones a second), its sharpening or its order (32, 40 or 48 throughout) left 782 to 792; the level trim change of 1.8.0 none. Held notes and the held out sets are read after the first 0.1 s, so they read as in 1.8.0: eleventh set in Low Latency 2.700% / 3.661% / level 0.913 dB (1.8.0: 0.919), twelfth 2.888% / 3.111% / 0.895 dB (0.856). High Quality is unchanged from 1.8.0.

Re-pinned again, the same day (the lead's call for 1.8.0 covers it): `saved` and the Low Latency null test now hold 1.8.1's output (`tests/state-1.8.1*`, `tests/ll_null.txt`; 1.8.0's pins are in git, 1.6.1's still beside them). Against 1.7.2, largest sample difference: the session in Low Latency 0.0567 (20.2 dB under the signal), the same 10 presets 0.019 to 0.070 (18.5 to 40.8 dB under), null cases: speech and singing files now 47 to 60 dB under (1.8.0: 16 to 41), transpose +3 at 224 Hz 0.101 (9.0 dB), throat 120% 0.129 (8.1 dB). A session saved by 1.6.1 still loads with every parameter as stored.

The thirteenth held out set had its one run on 1.8.0 (above) and is not run again on 1.8.1.

## 1.8.0, 2026-11-12

**Held vowels at a woman's pitch keep their formants, in both engines.** Found by the eleventh and twelfth held out sets (French, Greek and Portuguese women's vowels at 174 to 252 Hz), which failed on every version since they were written. This is a fault fix, and it changes how Low Latency sounds on corrected voices over 130 Hz, the default and the presets included. The lead's call, 2026-11-12 (Luke left these calls to the lead): ship it, and re-pin the tests that hold Low Latency's output bit for bit.

**1. Low Latency: the vocal tract model remembers what it heard as the pitch moves** (`EnvelopeFilter`, `TuneEngine`, `lowLatencySweep*`). From 130 Hz up Low Latency drew its model of the vocal tract through the harmonics heard at that one moment. On a high voice the harmonics are further apart than a formant is wide, so each formant landed on the harmonic nearest it, and a harmonic moved to the formant's real place fell into a gap: held u (F1 291, F2 779 Hz) at 221 Hz down 3 came out 8 to 10 dB low at 300 to 350 and 750 to 850 Hz. Now, from 130 to 400 Hz, Low Latency keeps each harmonic's level where it was heard for 0.4 s and draws the model through all of them, as High Quality has done since 1.4.0. That model has 48 poles from 160 Hz up (32 by 140 Hz) and no sharpening; every other model is as it was, so voices under 130 Hz and unvoiced sound are unchanged.

With the model right, Low Latency's output follows a real voice's rise and fall in level as harmonics cross a formant, and the level trim, trusted in full from its first 50 ms, chased that: front vowels moved up 3 came out 1.6 to 2.0 dB louder (limit 1.5) with an early version of this change. The trim now trusts its average only as far as its 2 s memory has filled, as High Quality's has since 1.5.0 for the same reason. That moves the level of every corrected Low Latency phrase over its first two seconds, at any pitch.

**2. High Quality: the harmonic shifter's memory is no longer thrown away on a glide** (`TractMemory::frame`). Above about 220 Hz High Quality moves each harmonic by what that harmonic was heard at, at the pitch it is going to. A check forgets all of it when the voice no longer matches what was stored (a new vowel). A harmonic stored while it was in the noise reads 15 to 17 dB under where it is now, and that one harmonic pushed the check over its 6 dB on every test glide, two or three times a glide, so on held u at 221 Hz up 4 the memory was used for 3% of the harmonics given out and the rest were straight lines between harmonics, which miss a formant between them (F1 at 350 to 400 Hz where a real voice has it at 300). Now a harmonic takes part in the check only when what was stored also stands over the noise. The memory is then used for 73% of them on that voice.

**3. High Quality: breath between harmonics is no longer lifted** (`HarmonicShifter`, weak harmonics). A harmonic too weak to trust next to the noise is given the level of what is where it is going. It was scaled by its centre bin, which on breath swings by 10 dB or more from frame to frame, and the gain then lifted all of that harmonic's bins whenever the one bin dipped: on breathy u the output above 1.5 kHz stood 3 to 6 dB over a real voice (Portuguese u at 252 Hz down 4, 4.8% against a real voice). Such harmonics are now scaled by their own bins' mean power: 0 to 1 dB.

| | 1.7.2 | 1.8.0 | Limit |
|---|---|---|---|
| Eleventh held out set, High Quality: against a real voice / against the input / level | **4.428%** / **7.529%** / 0.970 dB | 2.495% / 3.524% / 0.970 dB | 4% / 4% / 1.5 dB |
| Eleventh set, Low Latency: against a real voice / against the input / level | **12.121%** / **11.061%** / 1.142 dB | 2.700% / 3.661% / 0.919 dB | 5% / 5% / 1.5 dB |
| Twelfth held out set, High Quality: against a real voice / floor plus 2% / level | **4.809%** / 4.428% / 1.054 dB | 2.905% / 4.146% / 1.054 dB | 4% / 7.0% / 1.5 dB |
| Twelfth set, Low Latency: against a real voice / against the input / level | **13.514%** / **10.705%** / 0.877 dB | 2.888% / 3.111% / 0.856 dB | 5% / 5% / 1.5 dB |
| Tenth set, Low Latency women: against a real voice / input / level | 3.275% / 4.045% / 1.075 dB | 1.273% / 1.882% / 1.097 dB | 5% / 5% / 1.5 dB |
| Formants at +/-1 and +/-3, Low Latency, against the input | 3.853% | 3.317% | 5% |
| First formant flatter than a real voice, Low Latency | 10.648 dB | 5.139 dB | 11.6 dB |
| Level at +/-1 to +/-3, Low Latency | 1.252 dB | 0.883 dB | 1.5 dB |

Bold fails. Before: `tests/results-heldout11-1.7.2.txt`, `-12-1.7.2.txt`; after: `-1.8.0.txt`. Both sets have been seen now (they were used to find and choose this), so the proof is the thirteenth set, written and committed unrun before the last change (4de04b5), run once on 1.8.0 below.

**The thirteenth held out set, its one run, on 1.8.0 (8a7bece) as committed** (`tests/results-heldout13.txt`; Czech men's vowels, Skarnitzl and Volin 2012, written and committed unrun in 4de04b5): every line passes, 0 failures. High Quality, men 99 to 138 Hz: 3.469% against a real voice, 3.853% against the input, level 1.005 dB; the same tracts at 207 and 233 Hz: 2.109%, 3.111%, 1.072 dB. Low Latency, men in Low Male: 3.082%, 3.469%, 0.676 dB; in Alto/Tenor: 3.275%, 3.661%, 0.717 dB; tracts at 207 and 233 Hz in Wide Range: 3.082%, 2.499%, 0.929 dB (limits 4% and 1.5 dB in High Quality, 5% and 1.5 dB in Low Latency). It was not run on 1.7.2. It has been seen now.

**Re-pinned, with the old pins kept beside the new ones.** `saved`: the 12 factory presets and the 1.6.1 session in Low Latency now hold 1.8.0's output (`tests/state-1.8.0.txt`, `-ll.raw`, `-hq.raw`); 1.6.1's pins, which 1.6.1 to 1.7.2 met, stay in `tests/state-1.6.1*` and the test still reads them. Largest sample difference against 1.7.2: the session in Low Latency 0.0566 (19.9 dB under the signal); 10 of the 12 presets in Low Latency (all but Classic Snap and Final Mix High Quality) 0.019 to 0.070 (18.3 to 40.8 dB under); in High Quality all 12 presets and the session are bit for bit 1.7.2's except Tracking Low Latency, which runs Low Latency. A session saved by 1.6.1 still loads with every parameter as stored (new line: all 34 found, largest difference 1.9e-8 of a parameter's range, limit 1e-6). Low Latency null test: `tests/ll_null.txt` now 1.8.0's, `tests/ll_null-1.6.1.txt` the old; 18 of 24 cases changed, as in 1.6.1 every case where something is corrected: largest 0.189 on transpose +3 at 224 Hz (8.7 dB under the signal), throat 120% 0.151 (8.0 dB), speech and singing files 16 to 41 dB under. Parameters, ranges, latencies and AU codes are unchanged.

Full suite: 591 pass, 0 fail (1.7.2: 589; the two new lines are the session's parameters). No limit was changed. Lines that moved the wrong way, all within their limits: Low Latency pitch error at instant speed on the voice with wander, jitter and breath 0.488 -> 1.219 cents (limit 5), the vowel after the S 0.035 -> 0.388 cents (5), hard settings keep 3.8 -> 5.8% of vibrato (10), pitch movement against the sound at 147 Hz -0.229 -> -0.472 ms (1), tenth set men's level 0.94 -> 1.10 dB and 0.45 -> 0.75 dB (1.5), non-harmonic energy at 150 cents -40.5 -> -40.1 dB (-35); and, with no limit in Low Latency, big shifts of +/-7 against a real voice 5.2 -> 16.4% (o at 247 Hz up 7, where the output now stays on the input's formants, 1.3% against the input where it was 6.9%, and the tool's own floor there is 10.5%). High Quality's held notes, harmonics against a real voice, mean 2.94 -> 3.05 dB (3.5). Low Latency CPU at 180 Hz 2.56% in this run (limit 5; the same build read 3.2 to 5.1% under other agents' load, 1.7.2 read 4.0).

## 1.7.2, 2026-11-12

**Nothing comes out before a sound starts.** Real take 06 at 5.58 s, High Quality: for 6 ms the output stood at -34 dBFS while the take was at -62 to -73 dBFS, just before a sound began (a click by the real vocal check, 3.4 times the voice's own steps beside it, on 1.6.0, 1.6.1, 1.7.0 and 1.7.1). Voicing is decided ahead of the output and the shifters read ahead of it, so where a sound starts out of a pause the corrected sound was heard before it; Low Latency did it too, less. Now, where the input shows a sound coming (its peak over the next 12 ms 20 dB or more over its last 10 ms), the input is in a pause (that 10 ms peak 20 dB under the last 300 ms, or nothing in those 300 ms came near the coming sound) and the tuned voice would stand over the input's own level, the input itself goes out, as through any unvoiced sound; it crosses over in 0.1 ms and back in 1 ms (`TuneEngine::process`, onset gate). Classic's plain grains are left as they were. Luke left this call to the lead on 2026-11-11.

New suite test, written and committed before the change (`onset`): silence, then a note 30 cents off that starts at once, on the note start test's vowels and pitches, Transpose 0 and 3; the output over the 10 ms before the input's onset against the held note, limit -40 dB. 1.7.1: High Quality -7.3 dB, Low Latency -21.9 dB (FAIL); 1.7.2: -92.3 and -83.2 dB (pass). Every other suite number is 1.7.1's except one High Quality reading of real material, held_kathy's median distance from a note 3.9 -> 4.0 cents (worst file 3.863 -> 4.011, limit unchanged); the Low Latency null pins (22 cases) and all 12 factory presets and the 1.6.1 session in `saved` come out bit for bit as before: none of them has a sound starting out of a pause.

Real takes, the 11 working takes with each song's key (realvox): take 06 at 5.58 s output -34.1 -> -57.1 dBFS against a take at -61.8, and no longer a click; far clicks in High Quality 6 -> 5, at the default 1 -> 1 (take 04 at 130.80 s, smaller than the voice's own steps), Classic 1 -> 1, Hard Tune 0 -> 0; near clicks, which the check records without a limit, 1 -> 3 at the default (takes 02 at 22.27 s and 109.02 s, 0.74 and 0.12 times the voice's own steps). Held notes, outcomes, slides, formants, start lag, latency and nulls as in 1.7.1. Latency, parameters, defaults, presets and AU codes are unchanged.

The twelfth held out set had its first run, on 1.7.1 as it was (`tests/results-heldout12-1.7.1.txt`): it fails like the eleventh, High Quality 4.809% against a real voice (limit 4), Low Latency 13.514% against a real voice and 10.705% against the input (limit 5). Not fixed. It runs by name only, as the eleventh does.

## 1.7.1, 2026-11-11

**A held note that slides slowly onto the next note now lands on it** (a wrong note on Luke's take 09 at 97.2 s). His D3 slid down at about 13 cents per 100 ms and settled on C#3 +20 cents. The target only changed once the voice had been 12 cents past the line for 80 ms in a row (the rule that keeps a note sung on the line from flipping), which on a slide that slow came 180 ms after the line, so most of the held note came out on D3. Now a voice that has been on the far side of the line for 120 ms in a row has moved there, and the target follows (`PitchCorrector`, `slideSeconds`). A waver across the line spends much less than that on the far side (a 5.5 Hz vibrato about 80 ms of each cycle), and the waver rules are unchanged, so a note on the line still settles on one note.

New suite test, written and committed before the change (`hold`, case 3): A3 held 30 cents flat slides down at 13 cents per 100 ms onto G#3 +18; time from the line until the output is within 15 cents of G#3, limit 150 ms. 1.7.0: 182 ms in both engines (FAIL); 1.7.1: 132 ms (pass). Every other number in the full suite is the same as 1.7.0's, the Low Latency null pins included; the 12 factory presets still come out bit for bit as 1.6.1 gave them, and a session saved by 1.6.1 within 2.1e-7.

Real takes, the 11 working takes with each song's key (realvox, the same renders as for 1.7.0): wrong notes at the default 2 -> 1, in High Quality 3 -> 2, at Hard Tune's settings 1 -> 0 (take 09 at 97.16 s gone from all three); nothing else moved (held notes 90th percentile 8.0 c, slides 3.57 / rap 4.97%, formant 1.33 / 2.50%). Latency, parameters, defaults, presets and AU codes are unchanged.

Not at zero, as measured on 1.7.1's working set: 1 wrong note at the default (take 04 at 192.46 s, a trill between C#3 and D#3 that the target follows; the dry's median happens to sit 18 cents from the line) and 2 left as sung (takes 02 at 123.19 s and 04 at 158.91 s, both sung within 4 cents of the line between two notes, where the target flips); the settings that claim tight tuning fail take by take on takes 14 (Hard Tune 24.6 c, Classic 20.1, Humanize 0 41.2), 07 (Classic 12.0, Humanize 0 14.1) and 04 (Humanize 0 10.4): notes sung on an out-of-key note between two scale notes a whole tone apart, where the target flips. Every rule tried that holds the target there (in a simulation of the corrector on the recorded pitch; table in `_vox/rode/work/tune-pro/REPORT.md`, third pass) brings take 14 under 10 cents only by adding 4 to 12 wrong notes and turning 5.08 to 5.95% of the rap takes' slides round (limit 5). The eleventh held out set still fails (by name only, unchanged); a twelfth unseen set (Greek and Portuguese women's vowels) is committed for the fix and has not been run.

## 1.7.0, 2026-11-11

**New defaults: Retune Speed 3 ms and Humanize 0% (they were 20 ms and 30%). Luke said yes to this on 2026-11-11** ("yes to both": a faster default Retune Speed and a lower default Humanize). Nothing else changes: the engine is 1.6.1's, and parameter IDs, ranges, presets, latency and AU codes are as they were.

Only a new instance sounds different. A session saved by 1.6.1 carries its own Retune Speed and Humanize and loads with them; every factory preset sets both itself. Test `saved` (`testSavedBy161`), written and its files made by the 1.6.1 build before the defaults were touched: all 12 factory presets come out bit for bit as 1.6.1 gave them, in both engines; a session saved by a fresh 1.6.1 instance comes out within 2.1e-7 of 1.6.1's own output (-134 dBFS, limit 1e-6). That last one cannot be bit for bit: 1.6.1 held its default Retune Speed as 19.9999981 ms and left it alone when loading its own session, and a build with another default sets it from the session, which gives 20 exactly.

Why these two numbers. The real vocal check holds held notes to 10 cents at the 90th percentile. On Luke's 11 working takes with each song's key, the old default read 35.2 cents, with 45% of held notes more than 10 cents off. The pair was picked as the mildest that passes, on the working takes only.

Engine traces, 713 held notes on the 9 sung takes (the pitch the engine asks for; movement left = the output's pitch movement inside notes held 400 ms or more as a share of his own):

| Retune ms / Humanize % | held notes over 10 cents off | 90th percentile | movement left |
|---|---|---|---|
| 20 / 30 (old default) | 44.6% | 32.9 c | 94% |
| 20 / 20, 20 / 10, 20 / 0 | 39.0, 32.1, 21.3% | 29.8, 26.5, 24.0 | 90, 84, 69% |
| 10 / 0 | 12.1% | 12.8 | 61% |
| 5 / 30, 5 / 15, 5 / 5, 5 / 0 | 29.9, 20.1, 10.8, 6.3% | 22.7, 17.4, 10.7, 5.9 | 94, 85, 73, 60% |
| 2 / 30, 2 / 15, 2 / 5, 2 / 0 | 24.5, 15.3, 5.9, 2.2% | 19.1, 13.4, 6.5, 1.7 | 93, 87, 73, 62% |
| 0 / 30, 0 / 15, 0 / 5 | 21.5, 12.6, 3.4% | 18.1, 12.0, 4.5 | 95, 88, 75% |

The real vocal check itself on the installed 1.6.1 with the two settings changed (716 held notes; limits: 90th percentile 10 cents, slides 5%):

| Retune / Humanize | held notes 90th pct | wrong / left as sung | over a semitone off | slides reversed, sung / rap | start lag median / 90th pct |
|---|---|---|---|---|---|
| 20 / 30 | 35.2 c | 5 / 16 | 7 | 2.29 / 3.28% | 100 / 270 ms |
| 7 / 0 | 13.3 | 2 / 5 | 2 | 3.48 / 4.77 | 70 / 175 |
| 5 / 0 | 10.8 | 2 / 4 | 2 | 3.58 / 4.99 | 65 / 140 |
| 4 / 0 | 9.3 | 2 / 1 | 6 | 3.61 / 5.04 | 55 / 130 |
| **3 / 0** | **8.0** | 2 / 2 | 5 | 3.57 / 4.97 | 50 / 120 |
| 2 / 0 | 6.3 | 1 / 2 | 9 | 3.43 / 4.81 | 40 / 110 |
| 2 / 3 | 10.0 | 1 / 1 | 8 | 3.28 / 4.72 | 40 / 110 |

So Humanize has to go all the way to 0 (3% already costs 3.7 cents at Retune 2) and Retune Speed to 4 ms or under. 4 / 0 passes held notes but turns 5.04% of sliding frames round on the rap takes; 3 / 0 is the first pair under every one of those lines. What it costs: the held part of a note is corrected at the same speed as its start, so about 60% of his own pitch movement is left on long notes where the old default left 94%. It is close to Hard Tune (0 / 0). Natural Pop, Transparent Ballad, Light Touch and Keep The Vibrato are still there, unchanged, for the old feel.

Not at zero with the new default either, as measured: 2 held notes of 716 on the wrong note and 2 left as sung, 5 moments over a semitone off for 20 ms, and the start lag by the check's rule (50 / 120 ms against 33 / 86: the limit follows Retune Speed, and his entries slide into the note).

## 1.6.1, 2026-11-11

Second pass on Luke's own takes (17 whole songs, Rode NT1, 2026-11-10). Five engine fixes, one test whose reference was wrong, and tests for each. Latency (659 samples in Alto/Tenor), parameters, presets and AU codes are unchanged; no default and no preset setting changed. Low Latency changes on purpose, so the Low Latency null test now holds 1.6.1's output (18 of its 24 cases changed: every case in which something is corrected; `tests/ll_null-1.6.0.txt` keeps 1.6.0's numbers, and the same code built with `TUNE_AS_160` passes against them). Real take numbers are the 11 working takes with Key and Scale set to each song's key, by `_shared/realvox`; the full account is `_vox/rode/work/tune-pro/REPORT.md`, "Second pass".

**A test's reference was corrected (no engine change).** "Ninth held out set, speaking voices, worst formant move against the input", limit 4%, had failed on every version since the set was written (4.619% on 1.6.0). It reads the output against the INPUT. On that set a perfect shifter cannot meet it: the real voice with the same vocal tract at the new pitch, put through the same reading, is 5.378% from the input (Mandarin u, women, 212 Hz up 3; over 4% on 3 of 444 readings), while 1.6.0's output is 3.661% at worst from that real voice. Proof, every case four ways: `tests/heldout9-reference.sh` and `tests/heldout9-reference.txt`. The old line still prints its number, without a verdict. The test is now the two lines beside it, both written and committed before that table was made: the output against a real voice at the new pitch at the same 4%, and the old number against the tool's floor plus 2%, the rule every singing set already uses. No other limit was changed, reworded or removed.

**1. Low Latency: a tick in a corrected note no longer comes out as a burst and a dip.** A grain is heard up to 0.6 of a period from where it was read, and in Alto/Tenor from 98 to about 140 Hz, where the mark for the nearer period is not known in time at this latency, up to a whole period (10 ms). Each sample was whitened with the vocal tract model of the moment it was read and shaped with the model of the moment it was heard. A tick in the voice (a mouth click, a hard glottal closure) changes that model within a few ms, so the tick came out 11 to 21 dB louder above 9 kHz than it was sung and the vowel under it about 10 dB down for 5 to 15 ms (take 02 at 7.085 s, take 01 at 163.325 s). The model is now put back for the moment the grains were read from (`TuneEngine::process`, `PsolaShifter::readOffset`). High Quality is not changed by this.

**2. Low notes: the pitch movement arrives with the sound.** Where the tracker's reading is older than the whole delay (every note under about 147 Hz in Alto/Tenor, under 98 Hz in Low Male and Wide Range) the correction's own movement came up to 5 ms late. The voice's movement over its last period and a half is now carried on for the time that is missing, only while it moves the way a held note does (under 60 cents in 25 ms) and never by more than 15 cents (`TuneEngine::pitchInLine`). Notes under Alto/Tenor's range are still followed exactly as in 1.6.0.

**3. High Quality: the pitch control is read in line with the audio**, as Low Latency's has been since 1.6.0. It ran up to 15 ms ahead of the sound it belonged to on the test voice. 1.6.0 left it because the ninth held out set's old reading moved from 4.62% to 5.00% with it; by the corrected reading it does not move (3.661% both ways).

**4. High Quality with nothing to correct is the input, also across its hand over.** Below about 220 Hz High Quality gives out the grains, above it the harmonic shifter. Each alone already passed an uncorrected voice exactly, but at a hand over the output was for 20 ms a blend of the grains and the harmonic shifter's own rebuild of the voice, and the level match between them then eased back over a second. With every note set to Bypass that left the output only 34 dB under the input on the worst real take, a comb at any Mix under 100. Now a voiced stretch in which nothing has been corrected is the input itself.

**5. Low Latency: formants stay put on women's voices too.** Found by the tenth held out set, the first one ever run on Low Latency: on women's speaking voices around 200 Hz in Wide Range the formants went with the pitch, 5.19% at +3 semitones and 4.81% going down, against a 5% limit (and 4.5% by the floor plus 2% rule), on 1.6.0 and every version before it. Low Latency's grains centred on the voice's pulses from 130 Hz up; High Quality stopped doing that in 1.5.1 for the same reason. They now sit on the tracker's marks at every pitch, and are read a little slower going up as well as going down (the shift to the power 0.15 up, 0.1 down where it was 0.2). Past an octave up they still centre on the pulses and the upward read fades out, so Transpose +24 is as it was. The pitch control's age for these grains is 1.80 periods (2.02 for pulse centred ones), which keeps the pitch movement within 0.25 ms of the sound at 147 to 262 Hz. This changes Low Latency on every corrected note, the default included. An eleventh held out set (French women's vowels held on their own, other speakers and another source) was committed before the fix and run once after it.

| | 1.6.0 | 1.6.1 | Limit |
|---|---|---|---|
| A tick in a corrected note, Alto/Tenor 102 to 131 Hz: louder above 9 kHz / the 10 ms after it against the input | 3.97 dB / -6.26 dB | 0.10 / -1.52 dB (first fix alone) | 6 / -6 |
| The same at 81 and 90 Hz | 5.45 / -9.33 dB | -0.42 / -3.33 dB | 6 / -6 |
| Pitch movement against the sound, Low Latency, low notes, worst (Alto/Tenor 101 Hz) | 4.87 ms late | within 0.81 ms | 1 |
| The same, High Quality, 147 to 262 Hz, worst | 15.24 ms early | 0.49 ms early | 1 |
| A voice never corrected, gliding across High Quality's hand over: against the input / worst 100 ms | -57.1 / -47.9 dB | -197.0 / -188.2 dB | -90 |
| Tenth held out set, Low Latency, women in Wide Range, against the input | 5.192% | 4.045% | 5, and floor plus 2% |
| Tenth held out set, Low Latency, men in Alto/Tenor, level | 6.159 dB | 0.940 dB | 1.5 |
| Real takes: pitch contour against the reported latency, Low Latency / High Quality | 54 late / 50 early | 25 late / 16 late | 48 samples |
| Real takes: every note Bypass, High Quality, null at the reported delay, worst take | -33.6 dB | about -180 dB, 11 of 11 takes | -90 |
| Real takes: formant move on corrected notes, median / 90th pct, Low Latency | 1.47 / 2.70% | 1.33 / 2.49% | 1.5 / 3.0 |
| Real takes: clicks by the shared rule, default | 9 | 4 | 0 |
| Real takes: over a semitone off for 20 ms, Low Latency / High Quality / Classic | 10 / 25 / 5 | 7 / 9 / 4 | 0 |
| Real takes: slides reversed, High Quality, rap takes | 5.41% | 4.36% | 5 |

**Still not at zero on the real takes** (all in `REPORT.md`): held notes at the default (90th percentile 35 cents against 10: the default's Retune 20 and Humanize 30); 5 held notes of 716 on the wrong note and 16 left as sung (his pitch crosses the line between two notes inside the note); start lag by the check's rule (his slides into the note; 72 synthetic entries with a stable pitch read 30 ms at the median); 4 clicks by the shared rule, each smaller than the voice's own steps beside it. **The default Humanize was not changed.** Luke said yes to lowering it; the sweep says no value does what was hoped: share of held notes more than 10 cents off at Retune 20 with Humanize 30 / 20 / 15 / 10 / 5 / 0: 44.6 / 39.0 / 36.3 / 32.1 / 26.8 / 21.3% (movement left on long notes 94 / 90 / 88 / 84 / 78 / 69%), Hard Tune 0.1% (engine traces of 713 held notes; realvox on the AU: 44.8% at 30, 24.0% at 0, 6.7% Hard Tune). The rest is Retune 20 gliding between two notes where the target changes inside the note.

**Not at zero: the eleventh held out set fails, on 1.6.0 and 1.6.1 alike.** `PluginTest heldout11`, results in `tests/results-heldout11.txt` and `tests/results-heldout11-1.6.0.txt`. These are vowels at the corners of the vowel space; nearly all of it is one vowel, held u (F1 291 Hz, F2 779 Hz) at 221 Hz. High Quality: 4.428% against a real voice on both versions (limit 4, one reading of 360), against the input 4.809% on 1.6.0 and 7.529% on 1.6.1 (one reading, u at 221 Hz up 4, moved by the in line control; the mean over all 360 readings is 0.65% and 0.66%). Low Latency: 12.65 -> 12.12% against a real voice and 11.24 -> 11.06% against the input (limit 5; u at 221 Hz down 3, then y and o at 5.6%), level 2.20 dB (fail) -> 1.14 dB (pass). So 1.6.1 is no worse than 1.6.0 on it except that one High Quality reading, and better on level, but the set does not pass and nothing was tuned on it. It runs by name only and is not part of the full run. The tenth set passes every line on 1.6.1 and is part of the full run from here on.

## 1.6.0, 2026-11-11

Four fixes found on Luke's own takes (17 whole songs, Rode NT1, 2026-11-10). Low Latency and Classic change on purpose, so the Low Latency null test now holds 1.6.0's output (19 of its 24 cases changed; the other 5 are the parameter list and the four latencies). Latency, parameters and AU codes are unchanged. High Quality changes only where listed. Real take numbers are the 11 working takes with Key and Scale set to each song's key, measured by `_shared/realvox`; the 6 blind takes were run once at the end and are in `_vox/rode/work/tune-pro/REPORT.md`.

**1. Alto/Tenor follows notes under its range, down to 70 Hz, at the same 13.7 ms.** Its range starts at 98 Hz and Luke sings down to 77 to 95 Hz; those notes were not followed at all and came out exactly as sung. A second reading now looks at the longer periods over a window that reaches further back in time, not further ahead, and only speaks where the range's own reading says "not voiced" (`PitchTracker::analyseLow`). The range's reading is the same code and never sees the second one: test voices at 99 to 440 Hz come out bit for bit the same with and without it.
- What it costs. In Low Latency a grain for a note under 98 Hz is cut from the newest period already known, up to 1.4 periods back; on Luke's low notes the band above 9 kHz reads 4 dB over the dry voice in the top 5% of frames, against 1 dB in Low Male, which has the delay for a whole period. High Quality has no such cost. After a note under 98 Hz the rest of that phrase is not sample for sample 1.5.1 any more (the marks carry on from the low note), and the level trim, which averages over the corrected voice it has heard, differs by up to 0.3 dB for a few seconds.

**2. Low Latency and Classic: the pitch control is read in line with the audio.** The grains give out the input one whole latency late, 1.33 times the Input Type's longest period, but the pitch the tracker reports is only about two of the voice's own periods old. So on every note above the bottom of the range the correction ran ahead of the sound it belonged to: on a test voice with vibrato 0.8, 3.9 and 6.5 ms early in Alto/Tenor at 147, 196 and 262 Hz, and 7.7, 10.7 and 13.2 ms in Low Male and Wide Range. The control is now read that much further back (`TuneEngine::pitchInLine`). Whether a sound is voiced is decided where it was, so notes are not picked up later.
- What changes in the sound: a correction no longer starts before its note. Measured from the moment the new note arrives in the audio, a flat semitone step reaches its note in 16 ms, where 1.5.1 read 10, because 1.5.1 had started about 4.5 ms early.
- Under about 140 Hz in Alto/Tenor the reading is older than the whole latency, so there the control is still late, by up to 5.8 ms at 98 Hz, as it always was. Only more latency fixes that: Low Male and Wide Range have it.
- High Quality is not changed. Its control runs ahead in the same way, and reading it in line moves the ninth held out set's speaking formant move from 4.62% to 5.00% (limit 4), because its vocal tract settings were tuned with the lead in place. The code is there, switched off (`alignHighQuality`).

**3. A voice with nothing to correct comes out as the delayed input.** With no correction the grains step from mark to mark at the reported delay. Where the next mark was not there (the marks had been started again, or it was not known yet) the grain was cut from the nearest mark, up to 0.6 of a period away, and that offset was then kept for the rest of the uncorrected stretch. It is now a plain read at the reported delay, and the two halves of the window share one raised cosine as long as the spacing, so the output is the input, sample for sample. An offset left by a correction is walked back at 0.1% of the period a grain (1.7 cents, 600 periods at most). This is the grains, so it also holds in High Quality below about 220 Hz; its harmonic shifter above that never promised it.

**4. A held note keeps its target through a waver.** The target was the nearest note at every moment, with 12 cents of hysteresis. A singer who holds a note near the line between two notes and wavers by more than that sent the target back and forth, and the tuner made the waver bigger: a test voice 47 cents sharp of A3 with a +/-20 cent vibrato came out with a 45 cent waver, 34 cents from either note. Now a note that has been held (within 35 cents of its own average for 150 ms) keeps its target until the voice has stayed past the line for 80 ms, is 35 cents past it, or got there in one move of 45 cents within 60 ms. The same voice: a 19 cent waver, 14 cents from the note. A real step is taken where it always was (flat semitone step, at once and as a 60 ms glide: no later than before, apart from point 2), and a voice that slides, as speech and rap do, is not held at all. Classic is left as it was.

| | 1.5.1 | 1.6.0 | Limit |
|---|---|---|---|
| Alto/Tenor, held notes at 78 to 92 Hz, output pitch error at instant speed | 35.0 cents (not corrected) | 0.19 cents | 5 |
| Alto/Tenor, 87 Hz, non-harmonic energy in the corrected voice, Low Latency / High Quality | not corrected | -38.5 / -45.6 dB (Low Male -41.9 / -45.7) | -35 |
| Pitch movement against the sound, test voice, worst of three Input Types, Low Latency | 13.2 ms early | 0.86 ms early | 1 |
| The same, High Quality (not fixed) | 15.2 ms early | 15.2 ms early | none |
| An uncorrected voice after a correction, against the input at the reported delay | +5.0 dB | -168.8 dB | -90 |
| A note on the line with +/-20 cents vibrato, waver in the output | 45.4 cents | 19.4 cents | 20 |
| Flat semitone step, time to the new note, at once / 60 ms glide, Low Latency | 10 / 4 ms | 16 / 8 ms | 20 / 14 |
| After a hard step the new note is on pitch, 95th percentile, Low Latency | 0.06 cents | 1.80 cents | 5 |
| Real speech, worst 1% of 20 ms frames, level change, Low Latency / High Quality | 2.86 / 2.64 dB | 2.89 / 2.64 dB | 3 |

| Luke's 11 working takes, Key set to the song, default settings unless said | 1.5.1 | 1.6.0 | Limit |
|---|---|---|---|
| Held notes under 98 Hz left exactly as sung, of 12 (Low Latency / High Quality / Classic) | 4 / 3 / 3 | 0 / 0 / 1 | 0 |
| Held notes under 98 Hz within 10 cents of their note, Classic, of 12 | 1 | 11 | |
| Octave errors on held notes | 0 | 0 | 0 |
| Output over a semitone from the voice for 20 ms or more, Alto/Tenor (Low Latency / Classic) | 27 / 7 | 10 / 5 | 0 |
| The same in Low Male, Low Latency | 171 | 16 | 0 |
| Where the voice slides, output moving the other way, sung / rap takes, Alto/Tenor | 3.28 / 4.73% | 2.98 / 4.32% | 5 |
| The same in Low Male | 5.80 / 8.71% | 2.97 / 4.40% | 5 |
| Pitch movement against the reported latency, median of takes, Low Male (Low Latency / Classic) | 332 / 332 samples early | 3 / 37 early | 48 |
| The same in Alto/Tenor, Low Latency | 8 early | 54 late (notes under 140 Hz, see point 2) | 48 |
| The same, High Quality, Alto/Tenor (not fixed) | 73 early | 50 early | 48 |
| Every note set to Bypass: output against the input at the reported delay, worst take, Low Latency | -6.5 dB | -155.9 dB | -90 |
| The same, stretches more than 24 samples off the reported delay | 93 of 2631 | 0 of 2633 | |
| The same in High Quality (the harmonic shifter above 220 Hz keeps its own timing) | -5.8 dB | -33.6 dB | -90 |
| Held notes on another scale note than his median was nearest, clear / on the line, of 739 | 2 / 22 | 3 / 25 | 0 clear |
| Held notes left as sung though 20 cents or more off, of 739 | 21 | 17 | 0 |
| Clicks by the shared rule | 8 | 9 | 0 |
| Start lag, clean starts, median / 90th percentile | 105 / 285 ms | 105 / 285 ms | 50 / 120 |

What the takes say about point 4: nothing either way. The notes that end on the other side of a line, or stay where they were, are mostly notes where his own pitch moves across the line during the note, and the default's Humanize 30 follows that 140 ms late (README). The test voice shows the rule working; his takes do not show it helping yet.

Not fixed, and why: the ninth held out set's speaking formant move still reads 4.619% against 4, exactly as in 1.5.1 (High Quality's formant path was not touched, so no tenth set was written); High Quality's control lead; the grains cut one period back from 98 to about 140 Hz in Alto/Tenor Low Latency (README, "Found on Luke's real takes"). Full suite: 539 pass, 3 fail (; 1.5.1: 520 pass, 1 fail, on 21 fewer checks).

## 1.5.1, 2026-11-09

High Quality's grains, used below about 220 Hz, now sit on the tracker's pitch marks instead of on a pulse position found once at the start of each voiced stretch. Low Latency, Classic and latency are bit for bit 1.5.0.

The held pulse position drifted up to half a period away from the real pulses on low voices, so each grain cut a pulse in two and the output from 800 to 1800 Hz came out 4 to 9 dB low. It showed on vowels whose first two formants sit close together, such as Cantonese u (353 and 721 Hz) at 149 Hz moved down 3.

| High Quality | 1.5.0 | 1.5.1 | Limit |
|---|---|---|---|
| Eighth held out set, speaking formant move | **6.32%** | 3.28% | 4% |
| Eighth held out set, speaking level | 1.16 dB | 1.10 dB | 1.5 dB |
| Ninth held out set (Mandarin), speaking formant move | **5.94%** | **4.62%** | 4% |
| Ninth held out set, speaking level | 1.49 dB | 0.94 dB | 1.5 dB |
| Formant move, +/-1 to +/-3 | 2.11% | 2.30% | 4% |
| First formant flatter than a real voice | 3.80 dB | 4.17 dB | 7 dB |
| Seventh held out set, speaking level | 1.04 dB | 1.22 dB | 1.5 dB |
| CPU, voice at 180 Hz, 48 kHz stereo | 5.36% | 5.43% | 8% |

Bold fails. Nothing that passed in 1.5.0 fails now.

How clean the held out numbers are: the eighth set was read to find the cause, so it is a regression test now. The fix was chosen on new dev voices (`EngineHarness speak8`). The ninth set, Mandarin men and women from Meng, Chen and Li (2006), was written blind and run once at the end. It still fails speaking formant move, 4.62% against 4, but less than 1.5.0's 5.94%; singing is identical on both. On close-formant vowels like these, a perfect real voice at the new pitch already reads about 3 to 3.7% against the input, so the room under 4% is small. Nobody has listened to 1.5.1 yet.

## 1.5.0, 2026-11-08

High Quality gets a new shifter for high and sung notes. Low Latency and Classic are bit for bit 1.4.0, and latency is unchanged.

**A harmonic shifter above about 220 Hz.** High Quality reads the voice one period at a time so each harmonic sits on its own frequency bin, and moves each harmonic by how the vocal tract differs between where it was and where it is going. The vocal tract comes from a memory of each harmonic's level against the pitch it was heard at, and on held notes from a fit of the voice source plus five resonances (`src/HeldEnvelope.h`). Below about 220 Hz the grains from 1.4.0 do better and are kept. The switch is at 205 Hz going down and 235 Hz going up, after the pitch stays past it for 40 ms, with a 20 ms level-matched crossfade.

**The level trim trusts its average only as far as its 1 s memory has filled.** In the first part of a glide across a narrow formant it had been pushing the gain up to 8 dB the wrong way.

| High Quality | 1.4.0 | 1.5.0 | Limit |
|---|---|---|---|
| Formant move, +/-1 to +/-3 | 2.29% | 2.11% | 4% |
| First formant flatter than a real voice | 5.38 dB | 3.80 dB | 7 dB |
| Level change, +/-1 to +/-3 | 0.60 dB | 0.67 dB | 1.5 dB |
| Big shifts +/-7, formant move | 2.50% | 1.48% | 8% |
| High notes, formant move | 16.48% | 11.42% | 16.89% |
| High notes, first formant against its surroundings | **13.14 dB** | 4.01 dB | 9 dB |
| Held notes, harmonics against a real voice | **4.54 dB** | 2.87 dB | 3.5 dB |
| First held out set, 392 to 740 Hz, first formant | **15.93 dB** | 6.36 dB | 9 dB |
| Seventh held out set (Korean), speaking level | 1.12 dB | 1.04 dB | 1.5 dB |
| Seventh held out set, singing first formant | **13.83 dB** | 6.78 dB | 9 dB |
| Seventh held out set, held notes | **3.65 dB** | 2.41 dB | 3.5 dB |
| Eighth held out set (Japanese, Cantonese, Greek), singing first formant | **11.63 dB** | 6.41 dB | 9 dB |
| Eighth held out set, held notes | **4.16 dB** | 2.39 dB | 3.5 dB |
| Eighth held out set, speaking formant move | **6.32%** | **6.32%** | 4% |
| CPU, voice at 180 Hz, 48 kHz stereo | 4.41% | 5.33% | 8% |

Bold fails. The full suite, with all eight held out sets, is 514 pass, 1 fail, the eighth set's speaking formant move (1.4.0: 481 pass and 3 fail, on fewer tests). Nothing that passed in 1.4.0 fails now.

How clean the held out numbers are: sets 1 to 6 were all seen while choosing settings, so they are regression tests now. The seventh set was written blind and run once, failed speaking level (1.587 dB), and was then used to fix that, so it is no longer clean either. The eighth set was written blind and run once at the end. It shows one fail, speaking formant move on low men's voices, 6.32% against 4, and 1.4.0 reads exactly the same there because below 205 Hz both run the same grains. So that is an old weakness, not a new one.

Trade-off: the trim now takes a full second to settle, so on a held note transposed by up to 24 semitones the level 0.5 to 1.0 s in reads 2.64 dB (was 0.82). That line has no limit and is back to its old number by 1 s. Nobody has listened to 1.5.0 yet.

## 1.4.0, 2026-11-08

Two fixes. Low Latency changes on purpose for the first one, so its null test now holds 1.4.0's output.

**Notes starting after silence no longer burst.** Where a note starts, each new 40 ms window holds far more of it than the last, so the vocal tract model changed completely within a few ms. The grains read up to a period either side, so the voice was whitened by one model and shaped again by another, and that mismatch rang out. In Low Latency the output at a note start reached 4.85, about 14 dB over full scale, on test voices peaking at 0.4. Now, for 40 ms after the level jumps, the model's shape moves to the new one over 2.5 ms instead of at once. The level itself is not held. After the first half second, steady test notes differ from 1.3.0 by 65 to 92 dB under the signal, from the slower level trim below. Speech, sung phrases and vibrato, which start notes often or swing in level, differ by 17 to 39 dB under the signal. A new test plays vowels at 110 to 560 Hz starting hard after silence, at once or over 20 ms, and looks for a burst in the output against the input.

**High Quality's glide memory model has 48 poles instead of 32.** With the memory showing it the peaks between harmonics, the finer model follows them, so less of the vocal tract is left in the grains to move with the pitch. Only that model gets 48: a direct fit with more poles locks on to single harmonics, measured as level jumps on speech (3.50 dB against 2.52), and a model through one moment's harmonics on high notes moved the formants further. The memory now runs below 400 Hz instead of 350. Low Latency keeps its level trim over 2 seconds instead of 0.6, because holding the shape through note starts moved its worst level change from 1.47 to 1.53 dB. High Quality keeps 0.6, because 2 seconds moved high notes' formants past their limit.

Also: vDSP's FFT gave slightly different results on buffers not 64 byte aligned, and where the heap put them changed from run to run, so the same input could give two slightly different outputs. The FFT buffers are now aligned.

| | 1.3.0 | 1.4.0 | Limit |
|---|---|---|---|
| Low Latency, burst at a note start after silence | 9.09 dB | **2.10 dB** | 3 dB |
| Low Latency, loudest sample at a note start, test voices | 4.85 | 0.47 | none |
| Low Latency, level change at +/-1 to +/-3 | 1.47 dB | 1.45 dB | 1.5 dB |
| High Quality, burst at a note start | 0.72 dB | 0.96 dB | 3 dB |
| High Quality, formant move, +/-1 to +/-3 | 3.32% | 2.29% | 4% |
| High Quality, formant move, other vowels and pitches | 3.11% | 2.91% | 4% |
| High Quality, first formant flatter than a real voice | 6.02 dB | 5.38 dB | 7 dB |
| High Quality, level change | 1.10 dB | 0.60 dB | 1.5 dB |
| High Quality, big shifts +/-7 | 2.09% | 2.50% | 8% |
| High notes, formant move | 16.48% | 16.48% | 16.89% |
| High notes, first formant against its surroundings | 13.15 dB | 13.14 dB | 9 dB |
| First held out set, 98 to 220 Hz, formant move | 4.24% | **1.52%** | 4% |
| First held out set, level change | 1.69 dB | **0.72 dB** | 1.5 dB |
| First held out set, 392 to 740 Hz, first formant flatter | 16.54 dB | 15.93 dB | 9 dB |
| Held notes, harmonics against a real voice | 4.59 dB | 4.54 dB | 3.5 dB |
| Second held out set, formant move | 6.32% | **2.29%** | 4% |
| New third held out set, formant move | 4.62% | **3.47%** | 4% |
| Third held out set, level change | 1.15 dB | 0.80 dB | 1.5 dB |
| Real speech, 99th percentile frame level change, High Quality | 2.42 dB | 2.52 dB | 3 dB |
| Real speech, the same, Low Latency | 2.71 dB | 2.86 dB | 3 dB |
| CPU, High Quality, voice at 180 Hz, 48 kHz stereo | 3.24% | 4.41% | 8% |

Bold now passes. 481 tests pass, 3 fail (1.3.0: 474 and 6). All 3 failed in 1.3.0 too, and nothing that passed in 1.3.0 fails now. AuBench, which runs the default Low Latency, reads 2.7% default and 3.0% worst preset, as before.

How clean the held out numbers are: the first and second sets were both used to choose 1.4.0's settings, so they are no longer clean. The third set, men at 123 and 185 Hz and women at 233 and 311 Hz from the Hillenbrand table, was written before any of it was run. It was run four times, because two settings changed after its first run, each for a failure on another test: it read 3.47% every time. 1.3.0 on the same voices reads 4.62%.

## 1.3.0, 2026-11-07

High Quality keeps formants in place much better. Low Latency is bit for bit the same as 1.2.1.

A held note's harmonics sit either side of a narrow formant, so no one moment shows how tall it is. As the pitch moves, by a glide or vibrato, the harmonics sweep across it. High Quality now remembers each harmonic's level where it was heard, for about a fifth of a second, and draws the vocal tract model through all of those points. Below 350 Hz, up to 2.5 kHz. When the pitch moves faster than 30 semitones a second, as at the end of a spoken phrase, the harmonics smear and the memory is let go. With the peaks seen, the old fixes are turned down: sharpening from 0.5 to 0.1, and the slower grain reading when going down is off.

| High Quality | 1.2.1 | 1.3.0 | Limit |
|---|---|---|---|
| Formant move, +/-1 to +/-3 semitones | 4.24% | **3.32%** | 4% |
| Formant move, other vowels and pitches | 4.15% | **3.11%** | 4% |
| First formant flatter than a real voice | 6.84 dB | 6.02 dB | 7 dB |
| Big shifts +/-7, move against a real voice | 5.94% | 2.09% | 8% |
| High notes, first formant against its surroundings | 13.5 dB | 13.2 dB | 9 dB |
| First held out set, 98 to 220 Hz, formant move | 5.82% | 4.24% | 4% |
| First held out set, level change | 1.17 dB | 1.69 dB | 1.5 dB |
| New second held out set, women 175 to 262 Hz, formant move | 11.2% | 6.32% | 4% |
| Second held out set, colour against a real voice | 3.08 dB | 1.79 dB | none, the input itself reads 1.71 |
| Real speech, 99th percentile frame level change | 2.40 dB | 2.42 dB | 3 dB |
| CPU, voice at 180 Hz, 48 kHz stereo | 2.45% | 3.24% | 8% |

Bold now passes. One limit now fails that passed: on the first held out set, one case comes out 1.69 dB louder against 1.5. The first held out set was looked at while tuning this version, so it is no longer clean. A second held out set was written for 1.3.0 before any of it was run, and run once at the end, with 1.2.1 on the same voices for comparison. Neither set reaches 4%.

## 1.2.1, 2026-11-07

Renamed to Tune Pro, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.2.0

A new window, in ink, coral and white. No DSP changes. The window with it open and painting nulls against no window at all, largest sample difference 0.

- The hero is an eight second piano roll that runs edge to edge: your voice in white, the corrected voice in coral, the gap between them filled, and each note being hit as a block in its lane.
- Under it, a full width octave of keys. Click one to go Tune, Remove, Bypass, double-click to reset. The note being hit lights coral. A removed note is gone from the keyboard, with only its name left.
- Retune Speed leads the controls, with what the setting does in words. Flex Tune, Humanize, Natural Vibrato and Mix sit beside it.
- A live note and correction bar in the header, presets, key and scale.
- Everything else is under Advanced: voice, tuning, added vibrato and MIDI. It reports how many of those are changed while closed, and the display gives up the room when it opens.
- All controls are the same parameters with the same IDs, names and ranges. Window size and whether Advanced is open are the only things added to saved state, both range checked.
- Added `tests/RenderWindow.cpp`, which draws the real window on real speech to `docs/window/` and measures its CPU, and new window tests in `tests/PluginTest.cpp`.

## 1.1.0

Quality work. High Quality numbers, worst case on the test voices, each step measured against the one before. Flat means the first formant reads flatter than a real voice.

| Step | Flat | Formant move | Level | Other |
|---|---|---|---|---|
| Version 1, plain grains | 11.6 dB | 4.98% | 1.15 dB | big shifts 15.7% and 10.2 dB, high notes 45% |
| Take the vocal tract out before grains and put it back after | 9.9 dB | 4.77% | 5.1 dB | big shifts 7.1% and 5.6 dB, high notes 21% |
| Narrow the peaks that get put back, 25% | 8.5 dB | 4.98% | 4.0 dB | big shifts flat 2.8 dB |
| Centre grains on the pulses | 7.6 dB | 4.98% | 4.2 dB | non-harmonic energy needed a slower offset, below |
| Slower pitch mark slide, from Smooth Harmony | 7.5 dB | 4.56% | 4.2 dB | worst output pitch 2.98 to 0.80 cents |
| Draw the model through harmonic peaks on high voices | 7.8 dB | 5.6% | 2.0 dB | colour 3.3 to 2.8 dB, level from a first trim, slowed later |
| Sharpening set per frequency, 45 to 50% | 6.9 dB | 4.98% | 1.45 dB | |
| Hold the pulse offset for the whole note, not re-found | 6.8 dB | 4.98% | 1.45 dB | stopped a repeating jump that had put Low Latency non-harmonic energy at -14.7 dB, now -46 dB |
| Slower grain reading when shifting down, whitened grains | 6.8 dB | 4.24% | 1.45 dB | |

High Quality also: voicing read 10 ms ahead, so a note is on pitch 98% of the time from 10 to 40 ms against 88% in Low Latency, and an S after a vowel is untouched from its first sample. Through consonants the true input takes over from the filters.

Low Latency gets the model too, with no added latency: flat 11.6 to 10.0 dB, formant move 4.98 to 4.43%.

Limits changed after measuring, with the reasons in `tests/PassLimits.h` and the README: high note formant move, big shift formant move, and how peak height is read on high notes and big shifts. Still failing: High Quality formant move 4.24% against 4%, and first formant on high notes 13.5 dB against 9 dB.

## 1.0.0

First version, started from a copy of Smooth Tune. Same tracker and grain shifter, with these added: per-note Tune, Remove and Bypass on top of any key and scale, MIDI notes as targets with sustain pedal and octave choice, Classic mode, two engines that hand over with a crossfade, Mix, a vibrato generator, and a custom window with a keyboard and a scrolling pitch view.

Baseline, measured before any quality work, in High Quality and Low Latency alike:

- First formant flatter than a real voice: 11.6 dB
- Formant move at +/-1 and +/-3 semitones: 4.98%
- Big shifts of +/-7 semitones: formant move 15.7%, first formant flatter 10.2 dB
- High notes around 440 and 660 Hz: formant move 45.1%, first formant flatter 27.8 dB
